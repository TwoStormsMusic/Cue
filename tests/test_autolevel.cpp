// Auto: waiting for signal, listening, and the level it heard.

#include "check.h"

#include "app/autolevel.h"

#include <cmath>

using namespace cue;

namespace {

constexpr double kRate = 48000.0;

// 10 ms of a steady signal on input 0: its peak and mean square.
LevelBin binOf(float peak, float meanSquare, float kMeanSquare = -1) {
    LevelBin b;
    b.frames = 480;
    b.in[0] = {peak, meanSquare * 480, (kMeanSquare < 0 ? meanSquare : kMeanSquare) * 480};
    return b;
}

} // namespace

TEST(auto_waits_for_signal_then_listens_for_its_time) {
    AutoRun run(true, 0, {AutoSpec::Measure::Peak, -12.0f, 1.0f}, kRate);
    for (int i = 0; i < 50; ++i)
        CHECK(!run.add(binOf(0.001f, 1e-7f))); // below -50 dB: still waiting
    CHECK(run.state() == AutoRun::State::Waiting && run.seconds() == 0);
    int bins = 0;
    while (!run.add(binOf(bins % 2 ? 0.5f : 0.25f, 0.05f)))
        ++bins;
    CHECK_MSG(bins == 99, "%d bins", bins);
    CHECK(run.state() == AutoRun::State::Done);
    CHECK(std::fabs(run.levelDb() - 20 * std::log10(0.5)) < 0.01);
    // -6 dB heard, -12 wanted.
    CHECK(std::fabs(run.gainDb() - (-12 - 20 * std::log10(0.5))) < 0.01);
}

TEST(auto_rms_leaves_out_the_gaps_between_notes) {
    AutoRun run(true, 0, {AutoSpec::Measure::Rms, -18.0f, 2.0f}, kRate);
    // Notes at -20 dB RMS, every other 100 ms, with near silence between.
    for (int i = 0; run.state() != AutoRun::State::Done; ++i)
        run.add((i / 10) % 2 == 0 ? binOf(0.2f, 0.01f) : binOf(0.004f, 1e-6f));
    CHECK_MSG(std::fabs(run.levelDb() - (-20)) < 0.01, "%.2f", run.levelDb());
    CHECK(std::fabs(run.gainDb() - 2) < 0.01);
}

TEST(auto_loudness_gates_the_quiet_blocks) {
    AutoRun run(false, 0, {AutoSpec::Measure::Lufs, -14.0f, 4.0f}, kRate);
    // An output at -23 LUFS (K-weighted mean square) for 2 s, then quiet.
    const double k23 = std::pow(10.0, (-23 + 0.691) / 10);
    for (int i = 0; run.state() != AutoRun::State::Done; ++i) {
        LevelBin b;
        b.frames = 480;
        const bool loud = i < 200;
        b.out[0] = {loud ? 0.3f : 0.004f, static_cast<float>((loud ? k23 : 1e-9) * 480), static_cast<float>((loud ? k23 : 1e-9) * 480)};
        run.add(b);
    }
    // The quiet blocks are left out, the three that end the notes (3/4, 1/2
    // and 1/4 loud) kept: 18.5 blocks' worth over 20.
    const double heard = -23 + 10 * std::log10(18.5 / 20);
    CHECK_MSG(std::fabs(run.levelDb() - heard) < 0.01, "%.2f, not %.2f", run.levelDb(), heard);
    CHECK(std::fabs(run.gainDb() - (-14 - heard)) < 0.01);
}

TEST(auto_that_heard_almost_nothing_says_so) {
    AutoRun run(true, 0, {AutoSpec::Measure::Rms, -18.0f, 1.0f}, kRate);
    while (!run.add(binOf(0.004f, 1e-8f))) {
    }
    CHECK(run.levelDb() == AutoRun::kNothingDb);
    CHECK(run.gainDb() == 0);
}
