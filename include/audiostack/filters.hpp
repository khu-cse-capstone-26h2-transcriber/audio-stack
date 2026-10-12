// ============================================================================
// Pre-processing filters. Implements the "50 Hz high-pass filter" from the
// survey's Whisper pipeline notes using an RBJ-cookbook biquad.
// ============================================================================
#pragma once

#include <cmath>

namespace astk {

constexpr double kPi = 3.14159265358979323846; // avoid M_PI dependency for portability

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    double process(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
    void reset() { x1 = x2 = y1 = y2 = 0; }
};

// RBJ audio-EQ cookbook: 2nd-order high-pass
inline Biquad make_highpass(double f0, double fs, double q = 0.7071067811865476) {
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 + cw) / 2.0 / a0;
    b.b1 = -(1.0 + cw) / a0;
    b.b2 = (1.0 + cw) / 2.0 / a0;
    b.a1 = (-2.0 * cw) / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

} // namespace astk
