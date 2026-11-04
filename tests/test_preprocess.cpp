#include "audiostack/preprocess.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace astk;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Put the desired signal in exactly one channel. Every other channel has a
// different nonzero signal, so accidental mixing or wrong indices are visible.
static std::vector<int16_t> interleave(const std::vector<int16_t>& source,
                                       unsigned channels, unsigned selected) {
    std::vector<int16_t> data(source.size() * channels);
    for (size_t i = 0; i < source.size(); ++i)
        for (unsigned ch = 0; ch < channels; ++ch)
            data[i * channels + ch] = ch == selected ? source[i] : int16_t(6000 + 500 * ch);
    return data;
}

static std::vector<int16_t> run(Preprocessor& p, const std::vector<int16_t>& data,
                                unsigned channels, size_t read_frames) {
    std::vector<int16_t> out, block;
    for (size_t i = 0; i < data.size() / channels;) {
        const size_t n = std::min(read_frames, data.size() / channels - i);
        p.process(data.data() + i * channels, n, Encoding::S16LE, block);
        out.insert(out.end(), block.begin(), block.end());
        i += n;
    }
    return out;
}

static void test_six_channels() {
    const std::vector<int16_t> wanted{-1200, 1600, -2400, 2400, 0, 800};
    const auto input = interleave(wanted, 6, 4);
    Preprocessor p({16000, 16000, 6, 4, 0.0, false});
    require(run(p, input, 6, 2) == wanted, "six-channel capture must preserve only the selected channel");
    try {
        Preprocessor invalid({16000, 16000, 6, 6, 0.0, false});
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("out-of-range capture channel was accepted");
}

static void test_stereo_resampling() {
    std::vector<int16_t> input(9999);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = int16_t(100 * int(i % 51) - 2500);
    const auto stereo = interleave(input, 2, 1);
    Preprocessor p({48000, 16000, 2, 1, 0.0, false});
    const auto out = run(p, stereo, 2, 31);
    require(out.size() == input.size() / 3, "short reads lost samples during 48-to-16 kHz conversion");
    for (size_t i = 0; i < out.size(); ++i) {
        const int16_t expected = (input[i * 3] + input[i * 3 + 1] + input[i * 3 + 2]) / 3;
        require(out[i] == expected, "stereo beam channel or resampling result was incorrect");
    }

    Preprocessor fractional({44100, 16000, 2, 1, 0.0, false});
    const auto whole = run(fractional, stereo, 2, input.size());
    fractional.reset();
    const auto split = run(fractional, stereo, 2, 17);
    require(whole == split, "fractional conversion depends on ALSA read boundaries");
}

static void test_rnnoise(unsigned rate, unsigned channels) {
    std::vector<int16_t> noise(size_t(rate) * 3);
    uint32_t rng = 12345;
    for (auto& s : noise) {
        rng = rng * 1664525u + 1013904223u;
        s = int16_t(int(rng >> 21) - 1024);
    }
    const auto input = interleave(noise, channels, 0);
    Preprocessor p({rate, 16000, channels, 0, 0.0, true});
    const auto processed = run(p, input, channels, 101);
    require(processed.size() == 3 * 16000 - 160, "RNNoise stream lost more than its priming frame");
    p.reset();
    const auto after_reset = run(p, input, channels, 113);
    require(processed == after_reset, "RNNoise/SpeexDSP state differs after reset or short reads");

    Preprocessor bypass({rate, 16000, channels, 0, 0.0, false});
    const auto raw = run(bypass, input, channels, 101);
    constexpr size_t second = 16000;
    const float before = rms(raw.data() + second, second);
    const float after = rms(processed.data() + second, second);
    require(after < before * 0.75f, "RNNoise did not attenuate synthetic noise");
    std::printf("%u Hz/%u ch: synthetic noise RMS %.1f -> %.1f dBFS\n",
                rate, channels, rms_to_dbfs(before), rms_to_dbfs(after));
}

int main() try {
    test_six_channels();
    test_stereo_resampling();
    if (Preprocessor::rnnoise_available()) {
        test_rnnoise(48000, 2);
        test_rnnoise(16000, 6);
    } else {
        bool rejected = false;
        try { Preprocessor unavailable({48000, 16000, 1, 0, 0.0, true}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "RNNoise was silently bypassed in a build without it");
    }
    std::puts("preprocessing tests passed");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
