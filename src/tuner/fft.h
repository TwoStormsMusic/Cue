// Ported from the TwoStorms Tuner (github.com/TwoStormsMusic/Plugins, src/core/fft.h), unchanged but
// for its namespace, so the two can be compared line by line.
// A complex FFT for the convolution engines (the EQ's linear and natural phase): radix 2, in place, natural order in
// and out, data interleaved (re, im). Like EEL2's fft / ifft, neither direction scales: ifft(fft(x)) = n x.
// (EEL2 leaves its output permuted and wants its input to ifft permuted; every use here is either elementwise or
// round trip, so natural order on both sides gives the same numbers, up to rounding.)
#pragma once

namespace cue::tuner::fft {

constexpr int MAXN = 32768;
// n: a power of two, 2 .. MAXN
void forward(double *d, int n);
void inverse(double *d, int n);
// real data (EEL2's fft_real / ifft_real, natural order): m reals <-> m / 2 complex bins, twice the DFT, bin 0 holding
// 2 DC and 2 Nyquist; realInverse(realForward(x)) = 2m x. m: a power of two, 4 .. MAXN
void realForward(double *d, int m);
void realInverse(double *d, int m);

} // namespace cue::tuner::fft
