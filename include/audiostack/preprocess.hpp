#pragma once

#include <memory>

#include "audiostack/format.hpp"

namespace astk {

// Converts one selected ALSA channel to the engine's mono PCM stream.
// Use one instance per capture stream. Construction rejects invalid options.
class Preprocessor {
public:
    struct Options {
        unsigned input_rate = 48000;
        unsigned output_rate = 16000;
        unsigned channels = 1;
        unsigned channel_index = 0;
        double highpass_hz = 50.0;
        bool rnnoise = false;
    };

    explicit Preprocessor(const Options& options);
    ~Preprocessor();
    static bool rnnoise_available();

    // Replaces out. Short reads are buffered; zero output is possible while priming.
    void process(const void* interleaved, size_t frames, Encoding encoding,
                 std::vector<int16_t>& out);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astk
