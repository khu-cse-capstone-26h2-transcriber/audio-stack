// astk-capture-wav — record the same preprocessing chain as audiostackd.
// usage: astk-capture-wav --device plughw:0,2 --seconds 5 --out /tmp/test.wav
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "audiostack/alsa_capture.hpp"
#include "audiostack/log.hpp"
#include "audiostack/preprocess.hpp"

using namespace astk;

static void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

static void wav_header(FILE* f, uint32_t rate, uint32_t data_bytes) {
    std::fwrite("RIFF", 4, 1, f);
    put32(f, 36 + data_bytes);
    std::fwrite("WAVE", 4, 1, f);
    std::fwrite("fmt ", 4, 1, f);
    put32(f, 16);
    put16(f, 1);                    // PCM
    put16(f, 1);                    // mono
    put32(f, rate);
    put32(f, rate * 2);
    put16(f, 2);
    put16(f, 16);
    std::fwrite("data", 4, 1, f);
    put32(f, data_bytes);
}

int main(int argc, char** argv) try {
    std::string device = "plughw:0,2", out = "/tmp/astk_test.wav", denoise = "off";
    int seconds = 5;
    unsigned target = 16000;
    int native_rate = 0, channels = 1, ch_idx = 0;
    double hpz = 50.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--device") device = next("--device");
        else if (a == "--out") out = next("--out");
        else if (a == "--seconds") seconds = std::stoi(next("--seconds"));
        else if (a == "--rate") target = unsigned(std::stoi(next("--rate")));
        else if (a == "--native-rate") native_rate = std::stoi(next("--native-rate"));
        else if (a == "--channels") channels = std::stoi(next("--channels"));
        else if (a == "--channel-index") ch_idx = std::stoi(next("--channel-index"));
        else if (a == "--denoise") denoise = next("--denoise");
        else if (a == "--no-highpass") hpz = 0;
        else if (a == "--help") {
            std::printf("usage: astk-capture-wav [--device D] [--seconds N] [--out F] [--rate 16000]\n"
                        "       [--native-rate Hz] [--channels N] [--channel-index I]\n"
                        "       [--no-highpass] [--denoise off|rnnoise]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (channels < 1 || ch_idx < 0 || ch_idx >= channels)
        throw std::invalid_argument("--channel-index must be in [0, channels)");
    if (seconds < 1 || target < 1 || native_rate < 0 ||
        uint64_t(target) * unsigned(seconds) * 2 > UINT32_MAX - 36)
        throw std::invalid_argument("invalid WAV duration or sample rate");
    if (denoise != "off" && denoise != "rnnoise")
        throw std::invalid_argument("--denoise must be off or rnnoise");
    if (denoise == "rnnoise" && !Preprocessor::rnnoise_available())
        throw std::invalid_argument("RNNoise requested but not built; configure with -DASTK_ENABLE_RNNOISE=ON");

    AlsaCapture::Request req;
    req.device = device;
    req.channels = unsigned(channels);
    req.period_ms = 20;
    req.buffer_periods = 4;
    AlsaCapture cap;
    bool opened = false;
    std::string err;
    for (unsigned r : {native_rate ? unsigned(native_rate) : 48000u, target, 44100u}) {
        req.rate = r;
        if (cap.open(req, err)) { opened = true; break; }
        if (native_rate != 0) break;
    }
    if (!opened) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    const auto st = cap.status();
    Preprocessor pre({st.rate, target, st.channels, unsigned(ch_idx), hpz, denoise == "rnnoise"});

    std::vector<uint8_t> raw(cap.period_bytes());
    std::vector<int16_t> mono, all;
    all.reserve(size_t(target) * size_t(seconds));

    LOG_INFO("capturing %d s from %s (%u Hz, %u ch, period %u) -> %s", seconds, device.c_str(),
             st.rate, st.channels, st.period_frames, out.c_str());
    const size_t need = size_t(target) * unsigned(seconds);
    auto check_capture_error = [&](int rc) {
        if (rc == -EPIPE || rc == -ESTRPIPE) pre.reset();
        else if (rc < 0 && rc != -EAGAIN && rc != -EINTR)
            throw std::runtime_error(snd_strerror(rc));
    };
    while (all.size() < need) {
        const int ready = cap.wait_ms(1000);
        if (ready != 1) { check_capture_error(ready); continue; }
        const auto fr = cap.readi(raw.data(), cap.period_frames());
        if (fr <= 0) { check_capture_error(int(fr)); continue; }
        pre.process(raw.data(), size_t(fr), st.enc, mono);
        all.insert(all.end(), mono.begin(), mono.begin() + std::min(mono.size(), need - all.size()));
    }
    if (cap.status().xruns) LOG_WARN("XRUNs: %ld (re-tune period/buffer)", cap.status().xruns);

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    wav_header(f, target, uint32_t(all.size() * 2));
    std::fwrite(all.data(), 2, all.size(), f);
    std::fclose(f);
    std::printf("wrote %zu samples (%.2f s @%u Hz mono s16) -> %s\n", all.size(),
                double(all.size()) / target, target, out.c_str());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
