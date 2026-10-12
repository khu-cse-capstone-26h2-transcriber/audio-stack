// ============================================================================
// Audio format conversion: encodings (s16/s32/f32) <-> float, channel
// extraction, resampling. The pipeline target matches the Whisper standard
// input: 16,000 Hz / mono / S16_LE. (The Rubik Pi 3 on-board mic captures at
// 48 kHz, so software resampling is applied; the ReSpeaker v2.0 is native 16k.)
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace astk {

enum class Encoding : uint32_t { S16LE = 0, S32LE = 1, FLOAT32 = 2 };

inline size_t bytes_per_sample(Encoding e) {
    switch (e) {
        case Encoding::S16LE:   return 2;
        case Encoding::S32LE:   return 4;
        case Encoding::FLOAT32: return 4;
    }
    return 2;
}

inline float s16_to_f(int16_t v) { return float(v) / 32768.0f; }
inline int16_t f_to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return int16_t(std::lround(v * 32767.0f));
}
inline float s32_to_f(int32_t v) { return float(v) / 2147483648.0f; }

// interleaved source buffer (any encoding) -> float mono (one channel selected)
inline void to_float_mono(const void* src, size_t frames, size_t nch, size_t ch,
                          Encoding enc, std::vector<float>& dst) {
    dst.resize(frames);
    switch (enc) {
        case Encoding::S16LE: {
            const int16_t* in = static_cast<const int16_t*>(src);
            for (size_t i = 0; i < frames; i++)
                dst[i] = s16_to_f(in[i * nch + (nch > 1 ? ch : 0)]);
            break;
        }
        case Encoding::S32LE: {
            const int32_t* in = static_cast<const int32_t*>(src);
            for (size_t i = 0; i < frames; i++)
                dst[i] = s32_to_f(in[i * nch + (nch > 1 ? ch : 0)]);
            break;
        }
        case Encoding::FLOAT32: {
            const float* in = static_cast<const float*>(src);
            for (size_t i = 0; i < frames; i++)
                dst[i] = in[i * nch + (nch > 1 ? ch : 0)];
            break;
        }
    }
}

// float mono -> S16_LE mono
inline void float_to_s16(const float* in, size_t n, int16_t* out) {
    for (size_t i = 0; i < n; i++) out[i] = f_to_s16(in[i]);
}

// RMS (0..1) and dBFS
inline float rms(const int16_t* s, size_t n) {
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) { double v = double(s[i]) / 32768.0; acc += v * v; }
    return float(std::sqrt(acc / double(n)));
}
inline float rms_to_dbfs(float r) {
    if (r <= 1e-7f) return -140.0f;
    return 20.0f * std::log10(r);
}

// ---------------------------------------------------------------------------
// Simple fractional-ratio resampler (linear interpolation), good enough for
// speech (48k -> 16k etc.). For integer ratios a box-average pre-filter is
// used to reduce aliasing. Swap in GStreamer audioresample if higher quality
// is ever needed — the interface is plain PCM either way.
// ---------------------------------------------------------------------------
class Resampler {
public:
    Resampler(double in_rate, double out_rate)
        : in_rate_(in_rate), out_rate_(out_rate),
          step_(in_rate / out_rate), pos_(0.0),
          int_ratio_(std::fabs(step_ - std::round(step_)) < 1e-9 && step_ > 1.0
                         ? int(step_)
                         : 0) {}

    void process(const float* in, size_t n, std::vector<float>& out) {
        if (int_ratio_ > 0) {
            // box-average decimation (integer ratio)
            acc_ = 0.0f; cnt_ = 0;
            for (size_t i = 0; i < n; i++) {
                acc_ += in[i];
                if (++cnt_ == int_ratio_) {
                    out.push_back(acc_ / float(int_ratio_));
                    acc_ = 0.0f; cnt_ = 0;
                }
            }
            return;
        }
        for (size_t i = 0; i < n; i++) buf_.push_back(in[i]);
        while (pos_ + 1.0 < double(buf_.size())) {
            size_t i = size_t(pos_);
            double fr = pos_ - double(i);
            out.push_back(float((1.0 - fr) * buf_[i] + fr * buf_[i + 1]));
            pos_ += step_;
        }
        size_t keep = size_t(pos_);
        if (keep > 1) keep = 1;
        if (keep > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + long(keep));
            pos_ -= double(keep);
        }
    }

    void reset() { buf_.clear(); pos_ = 0.0; acc_ = 0.0f; cnt_ = 0; }

private:
    double in_rate_, out_rate_, step_, pos_;
    int int_ratio_ = 0;
    float acc_ = 0.0f;
    int cnt_ = 0;
    std::vector<float> buf_;
};

} // namespace astk
