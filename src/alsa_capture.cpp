#include "audiostack/alsa_capture.hpp"
#include "audiostack/log.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <poll.h>

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
    int rc = snd_pcm_open(&pcm_, req.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) {
        pcm_ = nullptr;
        err = "snd_pcm_open(" + req.device + "): " + snd_strerror(rc);
        return false;
    }
    for (const auto& f : kFmtOrder) {
        std::string e;
        if (try_params(req, f.fmt, e)) {
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
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer, &dir);
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
    if (!pcm_) return -1;
    int count = snd_pcm_poll_descriptors_count(pcm_);
    if (count <= 0) return -1;
    struct pollfd pfds[8];
    if (count > 8) count = 8;
    snd_pcm_poll_descriptors(pcm_, pfds, unsigned(count));
    int pr = poll(pfds, nfds_t(count), ms);
    if (pr < 0) return -1;
    unsigned short revents = 0;
    snd_pcm_poll_descriptors_revents(pcm_, pfds, unsigned(count), &revents);
    if (revents & POLLERR) {
        LOG_WARN("ALSA poll error (device %s), recovering", st_.device.c_str());
        st_.xruns++;
        snd_pcm_recover(pcm_, -EPIPE, 1);
        return -1;
    }
    return (revents & POLLIN) ? 1 : 0;
}

snd_pcm_sframes_t AlsaCapture::readi(void* dst, snd_pcm_uframes_t frames) {
    snd_pcm_sframes_t r = snd_pcm_readi(pcm_, dst, frames);
    if (r == -EPIPE) { // XRUN (buffer overrun) — recover immediately
        st_.xruns++;
        LOG_WARN("ALSA XRUN on %s (total %ld), recovering", st_.device.c_str(), st_.xruns);
        int rc = snd_pcm_recover(pcm_, int(r), 0);
        if (rc < 0) LOG_ERROR("snd_pcm_recover failed: %s", snd_strerror(rc));
    } else if (r < 0) {
        LOG_WARN("snd_pcm_readi(%s): %s", st_.device.c_str(), snd_strerror(int(r)));
        int rc = snd_pcm_recover(pcm_, int(r), 1);
        if (rc < 0) LOG_ERROR("snd_pcm_recover failed: %s", snd_strerror(rc));
    }
    return r;
}

snd_pcm_sframes_t AlsaCapture::avail() const {
    if (!pcm_) return 0;
    snd_pcm_sframes_t a = 0;
    snd_pcm_avail_update(pcm_, &a);
    return a;
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
            // form without brackets: " 1: UAC1 - USB Audio" (older kernels)
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
