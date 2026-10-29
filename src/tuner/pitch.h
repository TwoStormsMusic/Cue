// Ported from the TwoStorms Tuner (github.com/TwoStormsMusic/Plugins, src/core/pitch.h), unchanged but
// for its namespace, so the two can be compared line by line.
// The Tuner's pitch detector: the McLeod pitch method (the normalized square difference function, from an
// autocorrelation by FFT), its first strong peak as the period; then again low-passed just above that fundamental
// (so a stiff string's sharp upper partials don't pull the reading), refined over as many periods as the window holds
// (k periods measured at once: k times the precision). Runs in the Tuner's window, never on the audio thread.
#pragma once
#include <vector>

namespace cue::tuner {

class PitchDetector {
public:
    enum { R_GUITAR = 0, R_BASS = 1, R_VOCALS = 2, R_WIDE = 3 };
    struct Result {
        double f = 0;          // Hz (0: none)
        double clarity = 0;    // 0..1: how periodic
        double rmsDb = -200;   // the window's level, dBFS
    };
    // sr: the source's sample rate; range: R_*
    void setup(double sr, int range);
    int need() const { return (pre_ + W_) * dec_; }   // samples wanted, at the source's rate (oldest first)
    double sampleRate() const { return sr_; }
    int range() const { return range_; }
    bool detect(const float *x, Result &r);

private:
    static constexpr double LP = 1.3;   // the second pass's cutoff, over the fundamental
    double sr_ = 0, srd_ = 48000, fmin_ = 27, fmax_ = 2000;
    int range_ = -1, dec_ = 1, W_ = 4096, pre_ = 0;
    std::vector<double> x_, y_, d_, n_;
    void nsdf(const double *x, double sq, int tmax);
};

} // namespace cue::tuner
