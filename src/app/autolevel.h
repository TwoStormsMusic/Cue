#pragma once

// Auto, as the Gain Stage does it, for an input's gain, an output's level
// or a block's output (a wire leaving it): it waits until it hears
// something, listens for a while, then says the level it heard, measured as
// asked (peak, RMS, or loudness as BS.1770 has it), and the gain that brings
// it to the target, or, matching, to the level the block was fed.

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
    // A block's output, before the wire's gains: to the target, or to match
    // what the block is fed.
    AutoRun(AutoSpec spec, double sampleRate, bool match);

    // The engine's bins, as they come. True once it has heard enough.
    bool add(const LevelBin& bin);
    // A stretch of what it measures: what it hears and, matching, what the
    // block was fed meanwhile. True once it has heard enough.
    bool add(uint32_t frames, const Measured& heard, const Measured& fed = {});
    State state() const { return state_; }
    bool input() const { return input_; }
    size_t index() const { return index_; }
    const AutoSpec& spec() const { return spec_; }
    double seconds() const { return frames_ / rate_; } // listened so far
    bool match() const { return match_; }
    // Once done: the level it heard, kNothingDb if nothing much; matching,
    // the level the block was fed; and the gain that brings it to the
    // target or that level (clamped to what a level can be).
    double levelDb() const;
    double fedDb() const;
    double gainDb() const;

private:
    struct Heard {
        float peak = 0;
        std::vector<double> bins; // each 10 ms bin's mean square (K-weighted for loudness)
        void add(const Measured& m, uint32_t frames, bool lufs);
        double levelDb(AutoSpec::Measure measure) const;
    };

    bool input_ = false;
    size_t index_ = 0;
    AutoSpec spec_;
    double rate_;
    bool block_ = false, match_ = false;
    State state_ = State::Waiting;
    double frames_ = 0;
    Heard heard_, fed_;
};

} // namespace cue
