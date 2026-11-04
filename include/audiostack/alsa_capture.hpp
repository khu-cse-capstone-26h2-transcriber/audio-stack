// ============================================================================
// AlsaCapture — implements the survey's decision as-is:
//   * direct ALSA C API (no PulseAudio; PipeWire bridge optional)
//   * 16 kHz / S16_LE / mono baseline, period 20 ms (320 samples),
//     buffer 80 ms (4 periods)
//   * poll()-based async I/O + snd_pcm_recover() on XRUN
// The RUBIK Pi 3 3.5 mm headset microphone input uses 48 kHz in the board
// configuration (plughw:0,2 in the official example). Open at native_rate/channels;
// the upstream pipeline converts to 16k mono.
// ============================================================================
#pragma once

#include <alsa/asoundlib.h>

#include <string>
#include <vector>

#include "audiostack/format.hpp"

namespace astk {

struct CardInfo {
    int num = -1;
    std::string id;   // bracketed name in /proc/asound/cards -> hw:CARD=<id>
    std::string name;
};

struct PcmInfo {
    int card = -1, device = -1;
    std::string name;
    bool playback = false, capture = false;
};

class AlsaCapture {
public:
    struct Request {
        std::string device = "default"; // generic library default; daemon selects the board input
        unsigned rate = 16000;          // requested (fails if hardware cannot do it)
        unsigned channels = 1;          // all channels exposed by the chosen capture profile
        unsigned period_ms = 20;        // survey: 320 samples @16k
        unsigned buffer_periods = 4;    // survey: 1280 samples = 4 periods
    };
    struct Status {
        std::string device;
        unsigned rate = 0, channels = 0;
        unsigned period_frames = 0, buffer_frames = 0;
        Encoding enc = Encoding::S16LE;
        long xruns = 0;
    };

    ~AlsaCapture() { close(); }

    // negotiates formats in order: S16_LE -> S32_LE -> FLOAT32
    bool open(const Request& req, std::string& err);
    void close();

    bool is_open() const { return pcm_ != nullptr; }
    const Status& status() const { return st_; }

    // bytes needed to hold period_frames samples
    size_t period_bytes() const { return size_t(st_.period_frames) * st_.channels * bytes_per_sample(st_.enc); }
    unsigned period_frames() const { return st_.period_frames; }

    // 1=ready, 0=timeout, negative errno on error.
    // -EPIPE/-ESTRPIPE indicate a recovered discontinuity for both calls.
    int wait_ms(int ms);
    // Read up to `frames` interleaved frames.
    snd_pcm_sframes_t readi(void* dst, snd_pcm_uframes_t frames);
    // unread frames currently in the buffer
    snd_pcm_sframes_t avail() const;

private:
    int recover_error(int error);
    bool try_params(const Request& req, snd_pcm_format_t fmt, std::string& err);
    snd_pcm_t* pcm_ = nullptr;
    Status st_ {};
};

// parsers for /proc/asound/cards and /proc/asound/pcm
// (used by tools and automatic device selection)
std::vector<CardInfo> list_cards();
std::vector<PcmInfo> list_pcms();

} // namespace astk
