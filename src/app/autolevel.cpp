#include "app/autolevel.h"

#include <algorithm>
#include <cmath>

namespace cue {

AutoRun::AutoRun(bool input, size_t index, AutoSpec spec, double sampleRate)
    : input_(input), index_(index), spec_(spec), rate_(sampleRate) {
    spec_.seconds = std::clamp<float>(spec_.seconds, 0.1f, static_cast<float>(kMostSeconds));
}

AutoRun::AutoRun(AutoSpec spec, double sampleRate, bool match) : spec_(spec), rate_(sampleRate), block_(true), match_(match) {
    spec_.seconds = std::clamp<float>(spec_.seconds, 0.1f, static_cast<float>(kMostSeconds));
}

bool AutoRun::add(const LevelBin& bin) {
    if (block_ || index_ >= LevelBin::kMax)
        return state_ == State::Done;
    return add(bin.frames, input_ ? bin.in[index_] : bin.out[index_]);
}

bool AutoRun::add(uint32_t frames, const Measured& heard, const Measured& fed) {
    if (state_ == State::Done || frames == 0)
        return state_ == State::Done;
    if (state_ == State::Waiting) {
        // Matching, it starts once the block is fed: what it plays may be quiet.
        if ((match_ ? fed.peak : heard.peak) < kSignal)
            return false;
        state_ = State::Listening;
    }
    const bool lufs = spec_.measure == AutoSpec::Measure::Lufs;
    heard_.add(heard, frames, lufs);
    if (match_)
        fed_.add(fed, frames, lufs);
    frames_ += frames;
    if (seconds() >= spec_.seconds)
        state_ = State::Done;
    return state_ == State::Done;
}

void AutoRun::Heard::add(const Measured& m, uint32_t frames, bool lufs) {
    peak = std::max(peak, m.peak);
    bins.push_back(static_cast<double>(lufs ? m.kEnergy : m.energy) / frames);
}

double AutoRun::levelDb() const { return heard_.levelDb(spec_.measure); }

double AutoRun::fedDb() const { return match_ ? fed_.levelDb(spec_.measure) : kNothingDb; }

double AutoRun::Heard::levelDb(AutoSpec::Measure measure) const {
    double lv = kNothingDb;
    const size_t nb = bins.size();
    switch (measure) {
    case AutoSpec::Measure::Peak:
        if (peak > 0)
            lv = 20 * std::log10(static_cast<double>(peak));
        break;
    case AutoSpec::Measure::Rms: {
        // Leaving out the 10 ms bins more than 20 dB below the loudest: the
        // gaps between notes.
        double most = 0;
        for (double e : bins)
            most = std::max(most, e);
        const double threshold = std::max(most * 0.01, 1e-7);
        double sum = 0;
        size_t n = 0;
        for (double e : bins)
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
                sum += bins[i];
            z[j] = sum / 10;
        }
        auto lufs = [](double w) { return -0.691 + 10 * std::log10(w); };
        if (zones < 4) {
            double sum = 0;
            for (double e : bins)
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
    const double lv = levelDb(), to = match_ ? fedDb() : spec_.targetDb;
    if (lv <= kNothingDb || to <= kNothingDb)
        return 0;
    // A wire's level goes from silence to +24 dB; it is not a calibration.
    if (block_)
        return std::clamp<double>(to - lv, -60, 24);
    return std::clamp<double>(to - lv, kLevelFloorDb + 1, kLevelMaxDb);
}

} // namespace cue
