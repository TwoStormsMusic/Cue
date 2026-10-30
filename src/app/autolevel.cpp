#include "app/autolevel.h"

#include <algorithm>
#include <cmath>

namespace cue {

AutoRun::AutoRun(bool input, size_t index, AutoSpec spec, double sampleRate)
    : input_(input), index_(index), spec_(spec), rate_(sampleRate) {
    spec_.seconds = std::clamp<float>(spec_.seconds, 0.1f, static_cast<float>(kMostSeconds));
}

bool AutoRun::add(const LevelBin& bin) {
    if (state_ == State::Done || index_ >= LevelBin::kMax || bin.frames == 0)
        return state_ == State::Done;
    const LevelBin::Channel& ch = input_ ? bin.in[index_] : bin.out[index_];
    if (state_ == State::Waiting) {
        if (ch.peak < kSignal)
            return false;
        state_ = State::Listening;
    }
    peak_ = std::max(peak_, ch.peak);
    const float e = spec_.measure == AutoSpec::Measure::Lufs ? ch.kEnergy : ch.energy;
    bins_.push_back(static_cast<double>(e) / bin.frames);
    frames_ += bin.frames;
    if (seconds() >= spec_.seconds)
        state_ = State::Done;
    return state_ == State::Done;
}

double AutoRun::levelDb() const {
    double lv = kNothingDb;
    const size_t nb = bins_.size();
    switch (spec_.measure) {
    case AutoSpec::Measure::Peak:
        if (peak_ > 0)
            lv = 20 * std::log10(static_cast<double>(peak_));
        break;
    case AutoSpec::Measure::Rms: {
        // Leaving out the 10 ms bins more than 20 dB below the loudest: the
        // gaps between notes.
        double most = 0;
        for (double e : bins_)
            most = std::max(most, e);
        const double threshold = std::max(most * 0.01, 1e-7);
        double sum = 0;
        size_t n = 0;
        for (double e : bins_)
            if (e >= threshold) {
                sum += e;
                ++n;
            }
        if (n > 0)
            lv = 10 * std::log10(sum / n);
        break;
    }
    case AutoSpec::Measure::Lufs: {
        // Integrated loudness: 400 ms blocks every 100 ms, gated at -70 LUFS
        // and then 10 LU below what is left.
        const size_t zones = nb / 10;
        std::vector<double> z(zones);
        for (size_t j = 0; j < zones; ++j) {
            double sum = 0;
            for (size_t i = j * 10; i < j * 10 + 10; ++i)
                sum += bins_[i];
            z[j] = sum / 10;
        }
        auto lufs = [](double w) { return -0.691 + 10 * std::log10(w); };
        if (zones < 4) {
            double sum = 0;
            for (double e : bins_)
                sum += e;
            if (nb > 0 && sum > 0)
                lv = lufs(sum / nb);
            break;
        }
        auto gated = [&](double floor, double& mean) {
            double sum = 0;
            size_t n = 0;
            for (size_t j = 0; j + 3 < zones; ++j) {
                const double w = (z[j] + z[j + 1] + z[j + 2] + z[j + 3]) / 4;
                if (w > 0 && lufs(w) > floor) {
                    sum += w;
                    ++n;
                }
            }
            mean = n > 0 ? sum / n : 0;
            return n > 0;
        };
        double mean = 0;
        if (gated(-70, mean) && gated(std::max(-70.0, lufs(mean) - 10), mean))
            lv = lufs(mean);
        break;
    }
    }
    return lv < -70 ? kNothingDb : lv;
}

double AutoRun::gainDb() const {
    const double lv = levelDb();
    if (lv <= kNothingDb)
        return 0;
    return std::clamp<double>(spec_.targetDb - lv, kLevelFloorDb + 1, kLevelMaxDb);
}

} // namespace cue
