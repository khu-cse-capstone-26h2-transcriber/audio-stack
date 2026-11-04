#include "audiostack/alsa_capture.hpp"
#include "audiostack/log.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace astk {

static const struct {
    snd_pcm_format_t fmt;
    Encoding enc;
} kFmtOrder[] = {
    {SND_PCM_FORMAT_S16_LE, Encoding::S16LE},
    {SND_PCM_FORMAT_S32_LE, Encoding::S32LE},
    {SND_PCM_FORMAT_FLOAT_LE, Encoding::FLOAT32},
};

bool AlsaCapture::open(const Request& req, std::string& err) {
    close();
    Request resolved = req;
    if (req.device == "auto:xvf3800") {
        resolved.device.clear();
        for (const auto& pcm : list_pcms()) {
            if (!pcm.capture) continue;
            std::ifstream usb("/proc/asound/card" + std::to_string(pcm.card) + "/usbid");
            std::string id;
            if (!(usb >> id) || id != "2886:001a") continue;
            if (!resolved.device.empty()) {
                err = "multiple XVF3800 capture PCMs found; select one with --device";
                return false;
            }
            resolved.device = "hw:" + std::to_string(pcm.card) + "," + std::to_string(pcm.device);
        }
        if (resolved.device.empty()) {
            err = "no XVF3800 USB capture PCM (2886:001a); check arecord -l or specify --device";
            return false;
        }
    }
    int rc = snd_pcm_open(&pcm_, resolved.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        pcm_ = nullptr;
        err = "snd_pcm_open(" + resolved.device + "): " + snd_strerror(rc);
        return false;
    }
    for (const auto& f : kFmtOrder) {
        std::string e;
        if (try_params(resolved, f.fmt, e)) {
            st_.enc = f.enc;
            LOG_INFO("ALSA opened: device=%s rate=%u ch=%u fmt=%s period=%u buffer=%u",
                     st_.device.c_str(), st_.rate, st_.channels,
                     snd_pcm_format_name(f.fmt), st_.period_frames, st_.buffer_frames);
            return true;
        }
        err = e;
    }
    close();
    return false;
}

bool AlsaCapture::try_params(const Request& req, snd_pcm_format_t fmt, std::string& err) {
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    int rc = snd_pcm_hw_params_any(pcm_, hw);
    if (rc < 0) { err = std::string("hw_params_any: ") + snd_strerror(rc); return false; }

    rc = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) { err = std::string("set_access: ") + snd_strerror(rc); return false; }
    rc = snd_pcm_hw_params_set_format(pcm_, hw, fmt);
    if (rc < 0) { err = std::string("set_format: ") + snd_strerror(rc); return false; }
    rc = snd_pcm_hw_params_set_channels(pcm_, hw, req.channels);
    if (rc < 0) { err = std::string("set_channels: ") + snd_strerror(rc); return false; }

    unsigned rate = req.rate;
    rc = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr);
    if (rc < 0 || rate != req.rate) {
        err = "rate " + std::to_string(req.rate) + " unavailable (got " + std::to_string(rate) + ")";
        return false;
    }

    snd_pcm_uframes_t period = snd_pcm_uframes_t(uint64_t(req.rate) * req.period_ms / 1000);
    if (period == 0) period = 1;
    int dir = 0;
    rc = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, &dir);
    if (rc < 0 || period == 0) { err = std::string("set_period_size: ") + snd_strerror(rc); return false; }

    snd_pcm_uframes_t buffer = period * req.buffer_periods;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer);
    if (rc < 0 || buffer < period * 2) { err = std::string("set_buffer_size: ") + snd_strerror(rc); return false; }

    rc = snd_pcm_hw_params(pcm_, hw);
    if (rc < 0) { err = std::string("snd_pcm_hw_params: ") + snd_strerror(rc); return false; }

    // sw_params: availability/start thresholds at period granularity
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm_, sw);
    snd_pcm_sw_params_set_avail_min(pcm_, sw, period);
    snd_pcm_sw_params_set_start_threshold(pcm_, sw, period);
    rc = snd_pcm_sw_params(pcm_, sw);
    if (rc < 0) { err = std::string("snd_pcm_sw_params: ") + snd_strerror(rc); return false; }

    // Start explicitly: a PREPARED capture stream never becomes poll-readable,
    // and poll()-only loops would deadlock waiting for a stream that only
    // readi() would auto-start.
    rc = snd_pcm_start(pcm_);
    if (rc < 0) { err = std::string("snd_pcm_start: ") + snd_strerror(rc); return false; }

    st_ = Status {};
    st_.device = req.device;
    st_.rate = rate;
    st_.channels = req.channels;
    st_.period_frames = unsigned(period);
    st_.buffer_frames = unsigned(buffer);
    return true;
}

void AlsaCapture::close() {
    if (pcm_) {
        snd_pcm_drop(pcm_); // capture: no need to drain
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

int AlsaCapture::wait_ms(int ms) {
    if (!pcm_) return -EBADFD;
    const int rc = snd_pcm_wait(pcm_, ms);
    return rc < 0 ? recover_error(rc) : rc;
}

snd_pcm_sframes_t AlsaCapture::readi(void* dst, snd_pcm_uframes_t frames) {
    if (!pcm_) return -EBADFD;
    const auto rc = snd_pcm_readi(pcm_, dst, frames);
    return rc < 0 ? recover_error(int(rc)) : rc;
}

int AlsaCapture::recover_error(int error) {
    if (error != -EPIPE && error != -ESTRPIPE) return error;
    if (error == -EPIPE) ++st_.xruns;
    LOG_WARN("ALSA discontinuity on %s: %s (XRUNs %ld)",
             st_.device.c_str(), snd_strerror(error), st_.xruns);
    int rc = snd_pcm_recover(pcm_, error, 1);
    if (rc >= 0 && snd_pcm_state(pcm_) == SND_PCM_STATE_PREPARED)
        rc = snd_pcm_start(pcm_);
    if (rc >= 0) return error;
    LOG_ERROR("cannot recover %s: %s", st_.device.c_str(), snd_strerror(rc));
    return (rc == -EPIPE || rc == -ESTRPIPE) ? -EIO : rc;
}

snd_pcm_sframes_t AlsaCapture::avail() const {
    if (!pcm_) return 0;
    return snd_pcm_avail_update(pcm_);
}

// ------------------------------------------------------------- enumeration --
static std::string trim_copy(std::string s) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) s.erase(s.begin());
    while (!s.empty() && space(s.back())) s.pop_back();
    return s;
}

std::vector<CardInfo> list_cards() {
    std::vector<CardInfo> out;
    std::ifstream f("/proc/asound/cards");
    std::string line;
    while (std::getline(f, line)) {
        CardInfo ci;
        ci.num = std::atoi(line.c_str());
        size_t lb = line.find('['), rb = line.find(']'), colon = line.find(':', rb);
        if (lb != std::string::npos && rb != std::string::npos && colon != std::string::npos) {
            ci.id = trim_copy(line.substr(lb + 1, rb - lb - 1));
            std::string rest = line.substr(colon + 1);
            size_t dash = rest.find(" - ");
            if (dash != std::string::npos) {
                ci.name = trim_copy(rest.substr(dash + 3));
            } else {
                ci.name = trim_copy(rest);
            }
            out.push_back(ci);
        } else {
            // form without brackets: " 1: Loopback - Loopback" (older kernels)
            size_t colon2 = line.find(':');
            if (colon2 == std::string::npos) continue;
            std::string rest = trim_copy(line.substr(colon2 + 1));
            size_t dash = rest.find(" - ");
            ci.id = trim_copy(dash == std::string::npos ? rest : rest.substr(0, dash));
            ci.name = dash == std::string::npos ? "" : trim_copy(rest.substr(dash + 3));
            out.push_back(ci);
        }
    }
    return out;
}

std::vector<PcmInfo> list_pcms() {
    std::vector<PcmInfo> out;
    std::ifstream f("/proc/asound/pcm");
    std::string line;
    while (std::getline(f, line)) {
        // "00-02: PRIMARY MI2S TX :  : capture 1"
        PcmInfo pi;
        if (line.size() < 6 || line[2] != '-') continue;
        pi.card = std::atoi(line.substr(0, 2).c_str());
        pi.device = std::atoi(line.substr(3, 2).c_str());
        size_t c1 = line.find(':'), c2 = line.find(':', c1 + 1), c3 = line.find(':', c2 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        pi.name = trim_copy(line.substr(c1 + 1, c2 - c1 - 1));
        std::string tail = c3 != std::string::npos ? line.substr(c3 + 1) : line.substr(c2 + 1);
        pi.playback = tail.find("playback") != std::string::npos;
        pi.capture = tail.find("capture") != std::string::npos;
        out.push_back(pi);
    }
    return out;
}

} // namespace astk
