#pragma once

// Auto, as the Gain Stage does it, for an input's gain or an output's
// level: it waits until it hears something, listens for a while, then
// says the level it heard, measured as asked (peak, RMS, or loudness as
// BS.1770 has it), and the gain that brings it to the target.

#include "app/config.h"
#include "core/engine.h"

#include <vector>

namespace cue {

class AutoRun {
public:
    enum class State { Waiting, Listening, Done };
    static constexpr double kSignal = 0.00316; // -50 dB: what counts as hearing something
    static constexpr double kNothingDb = -200; // what it heard, if it was below -70 dB
    static constexpr double kMostSeconds = 60;

    // An input (as it arrives) or an output (before its level), by number.
    AutoRun(bool input, size_t index, AutoSpec spec, double sampleRate);

    // The engine's bins, as they come. True once it has heard enough.
    bool add(const LevelBin& bin);
    State state() const { return state_; }
    bool input() const { return input_; }
    size_t index() const { return index_; }
    const AutoSpec& spec() const { return spec_; }
    double seconds() const { return frames_ / rate_; } // listened so far
    // Once done: the level it heard, kNothingDb if nothing much, and the
    // gain that brings it to the target (clamped to what a level can be).
    double levelDb() const;
    double gainDb() const;

private:
    bool input_;
    size_t index_;
    AutoSpec spec_;
    double rate_;
    State state_ = State::Waiting;
    double frames_ = 0;
    float peak_ = 0;
    std::vector<double> bins_; // each 10 ms bin's mean square (K-weighted for loudness)
};

} // namespace cue
