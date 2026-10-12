// astk-capture-wav — dump the full pipeline (capture -> channel extraction ->
// high-pass -> 16k resample -> S16) to a WAV file for verification.
// usage: astk-capture-wav --device plughw:1,0 --seconds 5 --out /tmp/test.wav
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audiostack/alsa_capture.hpp"
#include "audiostack/filters.hpp"
#include "audiostack/format.hpp"
#include "audiostack/log.hpp"

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

int main(int argc, char** argv) {
    std::string device = "default", out = "/tmp/astk_test.wav";
    int seconds = 5;
    unsigned target = 16000;
    unsigned channels = 1, ch_idx = 0;
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
        else if (a == "--channels") channels = unsigned(std::stoi(next("--channels")));
        else if (a == "--channel-index") ch_idx = unsigned(std::stoi(next("--channel-index")));
        else if (a == "--no-highpass") hpz = 0;
        else if (a == "--help") {
            std::printf("usage: astk-capture-wav [--device D] [--seconds N] [--out F] [--rate 16000]"
                        " [--channels N] [--channel-index I] [--no-highpass]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    AlsaCapture::Request req;
    req.device = device;
    req.channels = channels;
    req.period_ms = 20;
    req.buffer_periods = 4;
    AlsaCapture cap;
    bool opened = false;
    std::string err;
    for (unsigned r : {target, 48000u, 44100u}) {
        req.rate = r;
        if (cap.open(req, err)) { opened = true; break; }
    }
    if (!opened) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    const auto st = cap.status();
    Biquad hp = hpz > 0 ? make_highpass(hpz, double(st.rate)) : Biquad {};

    std::unique_ptr<Resampler> rs;
    if (st.rate != target) rs = std::make_unique<Resampler>(double(st.rate), double(target));

    std::vector<uint8_t> raw(cap.period_bytes());
    std::vector<float> fbuf, rbuf;
    std::vector<int16_t> mono, all;
    all.reserve(size_t(target) * size_t(seconds));

    LOG_INFO("capturing %d s from %s (%u Hz, %u ch, period %u) -> %s", seconds, device.c_str(),
             st.rate, st.channels, st.period_frames, out.c_str());
    int64_t need = int64_t(target) * seconds;
    while (int64_t(all.size()) < need) {
        if (cap.wait_ms(1000) != 1) continue;
        snd_pcm_sframes_t fr = cap.readi(raw.data(), cap.period_frames());
        if (fr <= 0) continue;
        to_float_mono(raw.data(), size_t(fr), st.channels, ch_idx, st.enc, fbuf);
        if (hpz > 0) for (float& x : fbuf) x = float(hp.process(x));
        const float* src = fbuf.data();
        size_t n = fbuf.size();
        if (rs) { rbuf.clear(); rs->process(fbuf.data(), fbuf.size(), rbuf); src = rbuf.data(); n = rbuf.size(); }
        mono.resize(n);
        float_to_s16(src, n, mono.data());
        all.insert(all.end(), mono.begin(), mono.end());
    }
    if (st.xruns) LOG_WARN("XRUNs: %ld (re-tune period/buffer)", st.xruns);

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    wav_header(f, target, uint32_t(all.size() * 2));
    std::fwrite(all.data(), 2, all.size(), f);
    std::fclose(f);
    std::printf("wrote %zu samples (%.2f s @%u Hz mono s16) -> %s\n", all.size(),
                double(all.size()) / target, target, out.c_str());
    return 0;
}
