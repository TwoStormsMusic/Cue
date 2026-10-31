#pragma once

// What Auto measures: a signal's peak, its energy, and its energy
// K-weighted as BS.1770 has it for loudness.

#include <cmath>
#include <cstdint>

namespace cue {

// BS.1770's K-weighting at any sample rate: a high shelf, then a high pass,
// each a biquad (b0 b1 b2 a1 a2).
struct KWeighting {
    double shelf[5] = {}, highPass[5] = {};

    explicit KWeighting(double rate = 48000.0) {
        const double pi = 3.14159265358979323846;
        double k = std::tan(pi * 1681.974450955533 / rate);
        const double q = 0.7071752369554196, vh = std::pow(10.0, 3.999843853973347 / 20.0), vb = std::pow(vh, 0.4996667741545416);
        double a0 = 1.0 + k / q + k * k;
        shelf[0] = (vh + vb * k / q + k * k) / a0;
        shelf[1] = 2.0 * (k * k - vh) / a0;
        shelf[2] = (vh - vb * k / q + k * k) / a0;
        shelf[3] = 2.0 * (k * k - 1.0) / a0;
        shelf[4] = (1.0 - k / q + k * k) / a0;
        k = std::tan(pi * 38.13547087602444 / rate);
        const double q2 = 0.5003270373238773;
        a0 = 1.0 + k / q2 + k * k;
        highPass[0] = 1.0;
        highPass[1] = -2.0;
        highPass[2] = 1.0;
        highPass[3] = 2.0 * (k * k - 1.0) / a0;
        highPass[4] = (1.0 - k / q2 + k * k) / a0;
    }

    // One sample through both, transposed direct form II; st holds 4 states.
    double operator()(double x, double* st) const {
        const double y1 = shelf[0] * x + st[0];
        st[0] = shelf[1] * x - shelf[3] * y1 + st[1];
        st[1] = shelf[2] * x - shelf[4] * y1;
        const double y2 = highPass[0] * y1 + st[2];
        st[2] = highPass[1] * y1 - highPass[3] * y2 + st[3];
        st[3] = highPass[2] * y1 - highPass[4] * y2;
        return y2;
    }
};

// A stretch of a signal, measured: sums, so a longer stretch is their sum.
struct Measured {
    float peak = 0.0f;
    float energy = 0.0f;  // the sum of its samples squared
    float kEnergy = 0.0f; // ...K-weighted, for loudness

    // Adds frames of x, with st the K-weighting's 4 states for it. scale
    // weighs the energies (0.5 for each channel of a pair: their mean).
    void add(const KWeighting& k, const float* x, uint32_t frames, double* st, double scale = 1.0) {
        double e = 0.0, ke = 0.0;
        float p = peak;
        for (uint32_t n = 0; n < frames; ++n) {
            const double v = x[n];
            p = std::fmax(p, static_cast<float>(std::fabs(v)));
            e += v * v;
            const double y = k(v, st);
            ke += y * y;
        }
        peak = p;
        energy += static_cast<float>(e * scale);
        kEnergy += static_cast<float>(ke * scale);
    }
};

} // namespace cue
