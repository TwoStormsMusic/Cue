// Ported from the TwoStorms Tuner (github.com/TwoStormsMusic/Plugins, src/core/pitch.cpp), unchanged but
// for its namespace, so the two can be compared line by line.
#include "tuner/pitch.h"
#include "tuner/fft.h"
#include <algorithm>
#include <cmath>

namespace cue::tuner {

void PitchDetector::setup(double sr, int range)
{
    if (sr == sr_ && range == range_) return;
    sr_ = sr;
    range_ = range;
    // high rates are averaged down to 44.1 / 48 kHz first (the fundamentals are all far below that)
    dec_ = sr > 140000 ? 4 : (sr > 70000 ? 2 : 1);
    srd_ = sr / dec_;
    // the range: low B of a five-string bass (31 Hz) up to the top of a guitar or a voice
    switch (range) {
    case R_GUITAR: fmin_ = 50; fmax_ = 1500; break;
    case R_BASS: fmin_ = 27; fmax_ = 700; break;
    case R_VOCALS: fmin_ = 65; fmax_ = 1400; break;
    default: fmin_ = 27; fmax_ = 2000; break;
    }
    // the window: at least two of the lowest periods; the voice a little shorter (it moves)
    double want = range == R_VOCALS ? 0.045 : 0.085;
    W_ = 1024;
    while (W_ < want * srd_ && W_ < 8192) W_ *= 2;
    while (W_ < 2.2 * srd_ / fmin_ && W_ < 8192) W_ *= 2;
    // the run-in for the second pass's low-pass (it settles before the window starts): twelve of its time constants
    // at the lowest cutoff
    pre_ = (int)std::ceil(12 * srd_ / (2 * M_PI * 0.7071 * LP * fmin_));
    // all of it fits in an audio-key ring with room to spare (a 176.4 or 192 kHz source: a shorter run-in)
    pre_ = std::max(256, std::min(pre_, 24576 / dec_ - W_));
    x_.assign(pre_ + W_, 0.0);
    y_.assign(W_, 0.0);
    d_.assign(2 * W_, 0.0);
    n_.assign(W_, 0.0);
}

// the NSDF of x (W samples) up to lag tmax: n(t) = 2 r(t) / m(t), r the autocorrelation (by FFT, zero-padded to
// twice the window; left in d_[0, W) for the lags beyond), m(t) the energy of both overlapping parts
void PitchDetector::nsdf(const double *x, double sq, int tmax)
{
    const int W = W_, N = 2 * W;
    double *d = d_.data();
    // real FFTs (half the work): the packed spectrum is twice the DFT, so the power is a quarter of its square
    for (int i = 0; i < W; ++i) d[i] = x[i];
    for (int i = W; i < N; ++i) d[i] = 0;
    fft::realForward(d, N);
    d[0] = d[0] * d[0] * 0.25;
    d[1] = d[1] * d[1] * 0.25;
    for (int k = 1; k < W; ++k) {
        double a = d[2 * k], b = d[2 * k + 1];
        d[2 * k] = (a * a + b * b) * 0.25;
        d[2 * k + 1] = 0;
    }
    fft::realInverse(d, N);   // N times the autocorrelation
    const double inv = 1.0 / N;
    for (int t = 0; t < W; ++t) d[t] *= inv;
    double m = 2 * sq;
    for (int t = 0; t <= tmax + 1 && t < W; ++t) {
        if (t > 0) m -= x[t - 1] * x[t - 1] + x[W - t] * x[W - t];
        n_[t] = m > 1e-20 ? 2 * d[t] / m : 0;
    }
}

static void parabola(double a, double b, double e, double &dt, double &v)
{
    double den = a - 2 * b + e;
    dt = std::fabs(den) > 1e-12 ? 0.5 * (a - e) / den : 0;
    dt = std::max(-0.5, std::min(0.5, dt));
    v = b - 0.25 * (a - e) * dt;
}

bool PitchDetector::detect(const float *in, Result &r)
{
    r = Result();
    const int W = W_, T = pre_ + W;
    // averaged down, the level (of the window), the mean (of the window) taken off everything
    double sum = 0;
    for (int i = 0; i < T; ++i) {
        double v = 0;
        for (int k = 0; k < dec_; ++k) v += in[i * dec_ + k];
        x_[i] = v / dec_;
        if (i >= pre_) sum += x_[i];
    }
    double mean = sum / W, sq = 0;
    for (int i = 0; i < T; ++i) x_[i] -= mean;
    const double *x = x_.data() + pre_;
    for (int i = 0; i < W; ++i) sq += x[i] * x[i];
    r.rmsDb = sq > 1e-30 ? 10 * std::log10(sq / W) : -200;
    if (r.rmsDb < -80) return false;
    const int tmax = std::min(W - 2, (int)std::ceil(srd_ / fmin_) + 2);
    const int tmin = std::max(2, (int)std::floor(srd_ / fmax_) - 1);

    // the first pass, on the signal as it is: the key maxima (the largest of each positive stretch, after the one
    // around zero lag), the first one close to the best is the period (its multiples look as good, never better)
    nsdf(x, sq, tmax);
    int t = 1;
    while (t <= tmax && n_[t] > 0) ++t;
    if (t > tmax) return false;
    int keyT[64];
    double keyV[64];
    int nk = 0;
    double best = 0;
    while (t <= tmax && nk < 64) {
        while (t <= tmax && n_[t] <= 0) ++t;
        int mt = -1;
        double mv = -1;
        while (t <= tmax && n_[t] > 0) {
            if (n_[t] > mv) { mv = n_[t]; mt = t; }
            ++t;
        }
        if (mt >= tmin && mt > 0 && mt < tmax) {
            keyT[nk] = mt;
            keyV[nk] = mv;
            ++nk;
            best = std::max(best, mv);
        }
    }
    if (nk == 0 || best < 0.5) return false;
    int c = 0;
    while (c < nk && keyV[c] < 0.9 * best) ++c;
    if (c >= nk) return false;
    double dt, pv;
    parabola(n_[keyT[c] - 1], n_[keyT[c]], n_[keyT[c] + 1], dt, pv);
    double P = keyT[c] + dt;
    r.clarity = std::max(0.0, std::min(1.0, pv));
    if (r.clarity < 0.6) return false;

    // the second pass, low-passed just above the fundamental: the reading follows the fundamental, not the upper
    // partials (sharp on a stiff string, louder at the attack, so the needle would drift as they fade). Two
    // Butterworth sections from the run-in, so they've settled when the window starts.
    {
        double fc = std::min(LP * srd_ / P, 0.4 * srd_);
        double w = 2 * M_PI * fc / srd_, cw = std::cos(w), al = std::sin(w) / (2 * 0.70710678);
        double a0 = 1 + al, b0 = (1 - cw) / 2 / a0, b1 = (1 - cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - al) / a0;
        // the run-in this cutoff needs (higher notes settle sooner), the sections starting as if the first sample
        // had always been there
        int run = (int)std::ceil(12 * srd_ / (2 * M_PI * 0.7071 * fc));
        int i0 = std::max(0, pre_ - run);
        double v0 = x_[i0];
        double s2 = v0 * (b2 - a2), s1 = v0 * (b1 - a1) + s2, u1 = s1, u2 = s2;
        double sq2 = 0;
        for (int i = i0; i < T; ++i) {
            double v = x_[i];
            double o = b0 * v + s1;
            s1 = b1 * v - a1 * o + s2;
            s2 = b2 * v - a2 * o;
            double o2 = b0 * o + u1;
            u1 = b1 * o - a1 * o2 + u2;
            u2 = b2 * o - a2 * o2;
            if (i >= pre_) { y_[i - pre_] = o2; sq2 += o2 * o2; }
        }
        // a fading note (a plucked string) flattened out first: the NSDF of a decaying wave leans its peak one way or
        // the other with where the window cuts it, a cent or two on a low B. The decay from the energy of each whole
        // period in the window (a straight line through their logs), undone. Any offset off first, over whole periods
        // (the window's mean isn't zero when it cuts a wave part-way, and flattened it would be a slope).
        int J = (int)(W / P);
        if (sq2 > 1e-30 && J >= 2) {
            int e = std::min(W, (int)std::lround(J * P));
            double dc = 0;
            for (int i = 0; i < e; ++i) dc += y_[i];
            dc /= e;
            for (int i = 0; i < W; ++i) y_[i] -= dc;
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int ok = 0;
            for (int j = 0; j < J; ++j) {
                int a = (int)std::lround(j * P), b = (int)std::lround((j + 1) * P);
                double e = 0;
                for (int i = a; i < b && i < W; ++i) e += y_[i] * y_[i];
                if (e <= 1e-30) continue;
                double cx = 0.5 * (a + b), ly = std::log(e);
                sx += cx; sy += ly; sxx += cx * cx; sxy += cx * ly;
                ++ok;
            }
            double den = ok * sxx - sx * sx;
            if (ok >= 2 && den > 0) {
                double g = (ok * sxy - sx * sy) / den;   // the energy's slope, per sample (log)
                g = std::max(-20.0 / W, std::min(20.0 / W, g));
                double a = -0.5 * g, c0 = -a * 0.5 * W;
                sq2 = 0;
                for (int i = 0; i < W; ++i) {
                    y_[i] *= std::exp(c0 + a * i);
                    sq2 += y_[i] * y_[i];
                }
            }
        }
        if (sq2 > 1e-30) {
            // the period near the first pass's (within 3 %)
            int lo = std::max(2, (int)std::floor(P * 0.97) - 1), hi = std::min(W - 3, (int)std::ceil(P * 1.03) + 1);
            nsdf(y_.data(), sq2, hi + 1);
            int bt = -1;
            double bv = -1;
            for (int tt = lo; tt <= hi; ++tt)
                if (n_[tt] > bv) { bv = n_[tt]; bt = tt; }
            if (bt > lo && bt < hi && bv > 0.5) {
                parabola(n_[bt - 1], n_[bt], n_[bt + 1], dt, pv);
                P = bt + dt;
                x = y_.data();
                sq = sq2;
            }
        }
    }

    // the period measured over as many of them as fit in half the window (the NSDF stays near 1 there for a steady
    // note): k periods at once, k times the precision
    double c1 = n_[(int)std::lround(P)];
    const double *d = d_.data();
    int lim = W / 2;
    for (int k = (int)std::floor(lim / P); k >= 2; --k) {
        int c0 = (int)std::lround(k * P);
        if (c0 + 5 >= W) continue;
        int lo = c0 - 3, hi = c0 + 3;
        // m at these lags from the running sum
        double mtt = 2 * sq;
        for (int q = 0; q < lo - 1; ++q) mtt -= x[q] * x[q] + x[W - 1 - q] * x[W - 1 - q];
        double nloc[16];
        for (int tt = lo - 1; tt <= hi + 1; ++tt) {
            nloc[tt - (lo - 1)] = mtt > 1e-20 ? 2 * d[tt] / mtt : 0;
            mtt -= x[tt] * x[tt] + x[W - 1 - tt] * x[W - 1 - tt];
        }
        double best2 = -1;
        int bt = -1;
        for (int tt = lo; tt <= hi; ++tt) {
            double v = nloc[tt - (lo - 1)];
            if (v > best2) { best2 = v; bt = tt; }
        }
        if (bt <= lo || bt >= hi || best2 < 0.85 * c1) continue;
        parabola(nloc[bt - 1 - (lo - 1)], nloc[bt - (lo - 1)], nloc[bt + 1 - (lo - 1)], dt, pv);
        P = (bt + dt) / k;
        break;
    }
    r.f = srd_ / P;
    return true;
}

} // namespace cue::tuner
