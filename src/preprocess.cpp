#include "audiostack/preprocess.hpp"
#include "audiostack/filters.hpp"

#include <array>
#include <limits>
#include <stdexcept>

#ifdef ASTK_ENABLE_RNNOISE
#include <rnnoise.h>
#include <speex/speex_resampler.h>
#endif

namespace astk {

#ifdef ASTK_ENABLE_RNNOISE
namespace {
constexpr unsigned kDenoiseRate = 48000;
constexpr size_t kDenoiseFrame = 480;

class DspResampler {
public:
    DspResampler(unsigned in, unsigned out) : in_(in), out_(out) {
        int error = 0;
        state_.reset(speex_resampler_init(1, in, out, 3, &error));
        if (!state_ || error != RESAMPLER_ERR_SUCCESS)
            throw std::runtime_error("cannot initialize SpeexDSP resampler");
    }

    void process(const std::vector<float>& in, std::vector<float>& out) {
        out.clear();
        if (in.empty()) return;
        const auto limit = std::numeric_limits<spx_uint32_t>::max();
        if (in.size() > limit) throw std::length_error("resampler input too large");
        const uint64_t capacity = (uint64_t(in.size()) * out_ + in_ - 1) / in_ + 1;
        if (capacity > limit) throw std::length_error("resampler output too large");
        out.resize(size_t(capacity));
        spx_uint32_t used = spx_uint32_t(in.size()), produced = spx_uint32_t(capacity);
        const int rc = speex_resampler_process_float(state_.get(), 0, in.data(), &used,
                                                      out.data(), &produced);
        if (rc != RESAMPLER_ERR_SUCCESS || used != in.size())
            throw std::runtime_error("SpeexDSP failed to consume the capture block");
        out.resize(produced);
    }

    void reset() { speex_resampler_reset_mem(state_.get()); }

private:
    unsigned in_, out_;
    std::unique_ptr<SpeexResamplerState, decltype(&speex_resampler_destroy)>
        state_{nullptr, speex_resampler_destroy};
};
} // namespace
#endif

struct Preprocessor::Impl {
    Options options;
    Biquad hp;
    std::unique_ptr<Resampler> resample;
    std::vector<float> mono, converted;

#ifdef ASTK_ENABLE_RNNOISE
    std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> noise{nullptr, rnnoise_destroy};
    std::unique_ptr<DspResampler> to_48k, from_48k;
    std::vector<float> at_48k, denoised;
    std::array<float, kDenoiseFrame> noise_input{}, noise_output{};
    size_t fill = 0;
    bool primed = false;

    const std::vector<float>& denoise(const std::vector<float>& in) {
        if (to_48k) to_48k->process(in, at_48k);
        const auto& samples = to_48k ? at_48k : in;
        denoised.clear();
        for (float s : samples) {
            // RNNoise expects floats on the signed 16-bit PCM scale.
            noise_input[fill++] = std::clamp(s * 32768.0f, -32768.0f, 32767.0f);
            if (fill != kDenoiseFrame) continue;
            rnnoise_process_frame(noise.get(), noise_output.data(), noise_input.data());
            fill = 0;
            // Upstream discards the first overlap-add block; it is priming output.
            if (primed) {
                for (float x : noise_output) denoised.push_back(x / 32768.0f);
            }
            primed = true;
        }
        if (!from_48k) return denoised;
        from_48k->process(denoised, converted);
        return converted;
    }
#endif
};

bool Preprocessor::rnnoise_available() {
#ifdef ASTK_ENABLE_RNNOISE
    return true;
#else
    return false;
#endif
}

Preprocessor::Preprocessor(const Options& o) : impl_(std::make_unique<Impl>()) {
    if (o.input_rate == 0 || o.output_rate == 0)
        throw std::invalid_argument("preprocessing rates must be positive");
    if (o.channels == 0 || o.channel_index >= o.channels)
        throw std::invalid_argument("channel_index must be in [0, channels)");
    if (!std::isfinite(o.highpass_hz) || o.highpass_hz < 0 || o.highpass_hz >= o.input_rate / 2.0)
        throw std::invalid_argument("highpass_hz must be zero or below input Nyquist");
    if (o.rnnoise && !rnnoise_available())
        throw std::invalid_argument("RNNoise requested but not built; configure with -DASTK_ENABLE_RNNOISE=ON");

    impl_->options = o;
    if (o.highpass_hz > 0) impl_->hp = make_highpass(o.highpass_hz, o.input_rate);
#ifdef ASTK_ENABLE_RNNOISE
    if (o.rnnoise) {
        if (rnnoise_get_frame_size() != int(kDenoiseFrame))
            throw std::runtime_error("RNNoise library does not use 480-sample frames");
        impl_->noise.reset(rnnoise_create(nullptr));
        if (!impl_->noise) throw std::runtime_error("cannot initialize RNNoise");
        if (o.input_rate != kDenoiseRate)
            impl_->to_48k = std::make_unique<DspResampler>(o.input_rate, kDenoiseRate);
        if (o.output_rate != kDenoiseRate)
            impl_->from_48k = std::make_unique<DspResampler>(kDenoiseRate, o.output_rate);
        return;
    }
#endif
    if (o.input_rate != o.output_rate)
        impl_->resample = std::make_unique<Resampler>(o.input_rate, o.output_rate);
}

Preprocessor::~Preprocessor() = default;

void Preprocessor::process(const void* interleaved, size_t frames, Encoding encoding,
                           std::vector<int16_t>& out) {
    auto& s = *impl_;
    out.clear();
    if (frames == 0) return;
    to_float_mono(interleaved, frames, s.options.channels, s.options.channel_index,
                  encoding, s.mono);
    for (float& x : s.mono) {
        if (!std::isfinite(x)) x = 0.0f;
        if (s.options.highpass_hz > 0) x = float(s.hp.process(x));
    }
    const std::vector<float>* samples = &s.mono;
#ifdef ASTK_ENABLE_RNNOISE
    if (s.noise) samples = &s.denoise(s.mono);
    else
#endif
    if (s.resample) {
        s.converted.clear();
        s.resample->process(s.mono.data(), s.mono.size(), s.converted);
        samples = &s.converted;
    }
    out.resize(samples->size());
    float_to_s16(samples->data(), samples->size(), out.data());
}

void Preprocessor::reset() {
    auto& s = *impl_;
    s.hp.reset();
    if (s.resample) s.resample->reset();
#ifdef ASTK_ENABLE_RNNOISE
    if (s.noise) {
        if (rnnoise_init(s.noise.get(), nullptr) != 0)
            throw std::runtime_error("cannot reset RNNoise");
        if (s.to_48k) s.to_48k->reset();
        if (s.from_48k) s.from_48k->reset();
        s.fill = 0;
        s.primed = false;
    }
#endif
}

} // namespace astk
