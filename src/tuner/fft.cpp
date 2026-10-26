// Ported from the TwoStorms Tuner (github.com/TwoStormsMusic/Plugins, src/core/fft.cpp), unchanged but
// for its namespace, so the two can be compared line by line.
#include "tuner/fft.h"
#include <cmath>
#include <utility>
#include <vector>

namespace cue::tuner::fft {

// cos and sin of 2 pi k / MAXN for k < MAXN / 2
static const std::vector<double> &table()
{
    static const std::vector<double> t = [] {
        std::vector<double> v(MAXN);
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < MAXN / 2; ++k) {
            v[2 * k] = std::cos(2 * pi * k / MAXN);
            v[2 * k + 1] = std::sin(2 * pi * k / MAXN);
        }
        return v;
    }();
    return t;
}

static void run(double *d, int n, double sgn)
{
    const std::vector<double> &T = table();
    // bit reversal
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(d[2 * i], d[2 * j]); std::swap(d[2 * i + 1], d[2 * j + 1]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, stride = MAXN / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                double wr = T[2 * k * stride], wi = sgn * T[2 * k * stride + 1];
                double *a = d + 2 * (i + k), *b = d + 2 * (i + k + half);
                double xr = b[0] * wr - b[1] * wi, xi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - xr;
                b[1] = a[1] - xi;
                a[0] += xr;
                a[1] += xi;
            }
        }
    }
}

void forward(double *d, int n) { run(d, n, -1); }
void inverse(double *d, int n) { run(d, n, 1); }

// m reals -> twice their DFT's bins 0..m/2 - 1, bin 0 holding 2 DC and 2 Nyquist (EEL2's fft_real + fft_permute)
void realForward(double *d, int m)
{
    const std::vector<double> &T = table();
    const int n = m / 2, st = MAXN / m;
    run(d, n, -1);
    // X[k] = E + W^k O, E = (Z[k] + conj Z[n-k]) / 2, O = (Z[k] - conj Z[n-k]) / 2i; out 2 X
    double z0r = d[0], z0i = d[1];
    d[0] = 2 * (z0r + z0i);   // 2 X[0] = 2 (Re Z0 + Im Z0)
    d[1] = 2 * (z0r - z0i);   // 2 X[n]
    for (int k = 1; k <= n / 2; ++k) {
        int j = n - k;
        double ar = d[2 * k], ai = d[2 * k + 1], br = d[2 * j], bi = d[2 * j + 1];
        // for k: Zk = a, conj Z[n-k] = (br, -bi)
        double er = ar + br, ei = ai - bi;          // 2E
        double orr = ai + bi, oi = br - ar;         // 2O = (a - conj b) / i
        double wr = T[2 * k * st], wi = -T[2 * k * st + 1];
        double xr = er + (orr * wr - oi * wi), xi = ei + (orr * wi + oi * wr);
        // for j = n - k: Zj = b, conj Z[n-j] = conj a
        double er2 = br + ar, ei2 = bi - ai;
        double or2 = bi + ai, oi2 = ar - br;
        double wr2 = T[2 * j * st], wi2 = -T[2 * j * st + 1];
        double yr = er2 + (or2 * wr2 - oi2 * wi2), yi = ei2 + (or2 * wi2 + oi2 * wr2);
        d[2 * k] = xr; d[2 * k + 1] = xi;
        d[2 * j] = yr; d[2 * j + 1] = yi;
    }
}

// the inverse: y[t] = H0 + HN (-1)^t + 2 Re sum H[k] e^(2 pi i k t / m) (EEL2's fft_ipermute + ifft_real: 2m x back)
void realInverse(double *d, int m)
{
    const std::vector<double> &T = table();
    const int n = m / 2, st = MAXN / m;
    double h0 = d[0], hn = d[1];
    d[0] = h0 + hn;   // E0 + i O0
    d[1] = h0 - hn;
    for (int k = 1; k <= n / 2; ++k) {
        int j = n - k;
        double ar = d[2 * k], ai = d[2 * k + 1], br = d[2 * j], bi = d[2 * j + 1];
        // E_k = H_k + conj H_{n-k}, O_k = (H_k - conj H_{n-k}) e^(+2 pi i k / m), Z_k = E_k + i O_k
        double er = ar + br, ei = ai - bi, tr = ar - br, ti = ai + bi;
        double wr = T[2 * k * st], wi = T[2 * k * st + 1];
        double orr = tr * wr - ti * wi, oi = tr * wi + ti * wr;
        double er2 = br + ar, ei2 = bi - ai, tr2 = br - ar, ti2 = bi + ai;
        double wr2 = T[2 * j * st], wi2 = T[2 * j * st + 1];
        double or2 = tr2 * wr2 - ti2 * wi2, oi2 = tr2 * wi2 + ti2 * wr2;
        d[2 * k] = er - oi; d[2 * k + 1] = ei + orr;
        d[2 * j] = er2 - oi2; d[2 * j + 1] = ei2 + or2;
    }
    run(d, n, 1);
}

} // namespace cue::tuner::fft
