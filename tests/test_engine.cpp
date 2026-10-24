// Offline tests of the chain engine: no audio hardware, no plug-ins.
// Each test drives Engine::process() block by block, exactly as the audio
// thread would, and inspects the output samples.

#include "check.h"

#include "core/builtin.h"
#include "core/engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using namespace cue;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 64;
constexpr float kPi = 3.14159265f;

using Slots = std::vector<std::unique_ptr<Processor>>;

template <typename... Ps>
Slots slots(Ps... ps) {
    Slots c;
    (c.push_back(std::unique_ptr<Processor>(ps)), ...);
    return c;
}

// Both outputs from the end of the chain unless told otherwise.
std::unique_ptr<Chain> chain(const char* name, Slots c, size_t out2After = SIZE_MAX) {
    const size_t n = c.size();
    std::vector<OutputTap> taps{{0, n, OutputMix::Sum}, {0, std::min(out2After, n), OutputMix::Sum}};
    return std::make_unique<Chain>(name, std::move(c), std::move(taps));
}

EngineSettings settings() {
    EngineSettings s;
    s.sampleRate = kRate;
    s.maxFrames = kBlock;
    s.numOutputs = 2;
    return s;
}

struct Sine {
    float freq = 110.0f, amp = 0.5f;
    double phase = 0.0;
    float operator()() {
        const float v = amp * std::sin(static_cast<float>(phase));
        phase += 2.0 * kPi * freq / kRate;
        return v;
    }
};

// Runs the engine and records both outputs.
struct Harness {
    Engine& engine;
    std::function<float()> input;
    std::vector<float> out[2] = {};

    void run(size_t blocks, const std::function<void(size_t)>& beforeBlock = nullptr) {
        std::vector<float> in(kBlock), o1(kBlock), o2(kBlock);
        float* outs[2] = {o1.data(), o2.data()};
        for (size_t b = 0; b < blocks; ++b) {
            if (beforeBlock)
                beforeBlock(b);
            for (auto& v : in)
                v = input ? input() : 0.0f;
            engine.process(in.data(), outs, kBlock);
            out[0].insert(out[0].end(), o1.begin(), o1.end());
            out[1].insert(out[1].end(), o2.begin(), o2.end());
        }
    }
    size_t blocksFor(double seconds) const { return static_cast<size_t>(seconds * kRate / kBlock); }
    void runSeconds(double seconds) { run(blocksFor(seconds)); }
    // Runs until the chain is active and its fade-in has finished.
    void settleOn(int index) {
        engine.switchTo(index);
        run(blocksFor(0.25));
    }
};

float maxStep(const std::vector<float>& x, size_t from = 1, size_t to = SIZE_MAX) {
    float m = 0.0f;
    to = std::min(to, x.size());
    for (size_t n = std::max<size_t>(from, 1); n < to; ++n)
        m = std::max(m, std::fabs(x[n] - x[n - 1]));
    return m;
}

float peak(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t n = from; n < std::min(to, x.size()); ++n)
        m = std::max(m, std::fabs(x[n]));
    return m;
}

size_t samples(double seconds) { return static_cast<size_t>(seconds * kRate); }

// Steady-state largest sample-to-sample step of a chain playing the sine alone.
float steadyStep(std::function<std::unique_ptr<Chain>()> make, int output = 0) {
    Engine e(settings());
    e.addChain(make());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    h.runSeconds(1.0);
    return maxStep(h.out[output], from);
}

} // namespace

TEST(idle_chains_are_never_processed) {
    CountingProcessor::Counts a, b, c;
    Engine e(settings());
    e.addChain(chain("A", slots(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), a))));
    e.addChain(chain("B", slots(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), b))));
    e.addChain(chain("C", slots(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), c))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    h.runSeconds(2.0);
    CHECK(a.process > 0);
    CHECK_MSG(b.process == 0 && c.process == 0, "B processed %llu blocks, C %llu", (unsigned long long)b.process.load(),
              (unsigned long long)c.process.load());
    CHECK(b.reset == 0 && c.reset == 0 && b.start == 0 && c.start == 0);
    CHECK(e.activeChain() == 0);
}

TEST(nothing_plays_before_the_first_chain) {
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.runSeconds(0.5);
    CHECK(peak(h.out[0], 0, h.out[0].size()) == 0.0f);
    CHECK(e.activeChain() == -1);
}

TEST(preroll_delays_the_new_chain_by_the_preroll_time) {
    EngineSettings s = settings();
    s.prerollMs = 50.0f;
    CountingProcessor::Counts b;
    Engine e(s);
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    e.addChain(chain("B", slots(new CountingProcessor(std::make_unique<GainProcessor>(0.5f), b))));
    CHECK(e.activate());
    Harness h{e, [] { return 0.5f; }}; // DC input: A gives 0.5, B gives 0.25
    h.settleOn(0);

    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(0.5);

    size_t firstChange = SIZE_MAX;
    for (size_t n = switchAt; n < h.out[0].size(); ++n)
        if (std::fabs(h.out[0][n] - 0.5f) > 1e-6f) {
            firstChange = n - switchAt;
            break;
        }
    const size_t preroll = samples(0.050);
    CHECK_MSG(firstChange >= preroll && firstChange <= preroll + kBlock, "new sound began %zu samples after the switch, pre-roll is %zu",
              firstChange, preroll);
    CHECK(b.reset == 1);                   // reset once before pre-roll
    CHECK(b.frames >= preroll);            // processed live input while muted
    CHECK(std::fabs(h.out[0].back() - 0.25f) < 1e-4f);
    CHECK(e.activeChain() == 1);
}

TEST(switching_has_no_clicks) {
    // Three different sounds; switch every 0.5 s and look for steps larger
    // than the chains themselves ever make.
    auto makeA = [] { return chain("A", slots(new DriveProcessor(3.0f), new LowpassProcessor(2000.0f))); };
    auto makeB = [] { return chain("B", slots(new GainProcessor(0.3f), new LowpassProcessor(800.0f))); };
    auto makeC = [] { return chain("C", slots(new DriveProcessor(6.0f), new DelayProcessor(300.0f, 0.5f, 0.5f)), 1); };
    const float steady = std::max({steadyStep(makeA), steadyStep(makeB), steadyStep(makeC)});

    Engine e(settings());
    e.addChain(makeA());
    e.addChain(makeB());
    e.addChain(makeC());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    int next = 1;
    h.run(h.blocksFor(10.0), [&](size_t b) {
        if (b % h.blocksFor(0.5) == 0) {
            e.switchTo(next);
            next = (next + 1) % 3;
        }
    });
    // Two chains overlap during a fade (three when a tail is ringing), so
    // allow their steps to add, but nothing like a cut.
    const float worst = maxStep(h.out[0], from);
    CHECK_MSG(worst <= 3.0f * steady, "largest step %.5f, steady chains reach %.5f", worst, steady);
    const float worst2 = maxStep(h.out[1], from);
    CHECK_MSG(worst2 <= 3.0f * steady, "out 2 largest step %.5f, steady chains reach %.5f", worst2, steady);
}

TEST(click_detector_catches_a_hard_cut) {
    // The same switch with no pre-roll and no crossfade must be flagged,
    // otherwise the test above proves nothing.
    auto makeA = [] { return chain("A", slots(new DriveProcessor(3.0f), new LowpassProcessor(2000.0f))); };
    auto makeB = [] { return chain("B", slots(new GainProcessor(0.3f), new LowpassProcessor(800.0f))); };
    const float steady = std::max(steadyStep(makeA), steadyStep(makeB));
    EngineSettings s = settings();
    s.prerollMs = 0.0f;
    s.crossfadeMs = 0.0f;
    Engine e(s);
    e.addChain(makeA());
    e.addChain(makeB());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    h.run(h.blocksFor(4.0), [&](size_t b) {
        if (b % h.blocksFor(0.5) == 7)
            e.switchTo(static_cast<int>((b / h.blocksFor(0.5) + 1) % 2));
    });
    CHECK_MSG(maxStep(h.out[0], from) > 3.0f * steady, "hard cut not detected: %.5f vs %.5f", maxStep(h.out[0], from), steady);
}

TEST(tail_continues_after_switch_and_chain_goes_idle) {
    EngineSettings s = settings();
    s.tailCapMs = 20000.0f;
    CountingProcessor::Counts delayCounts;
    Engine e(s);
    e.addChain(chain("Delay", slots(new CountingProcessor(std::make_unique<DelayProcessor>(250.0f, 0.5f, 1.0f), delayCounts))));
    e.addChain(chain("Silent", slots(new GainProcessor(0.0f))));
    CHECK(e.activate());

    size_t t = 0;
    Sine sine;
    Harness h{e, [&] { return t < samples(0.35) ? (t++, sine()) : (t++, 0.0f); }};
    h.settleOn(0); // 0.25 s of playing, echoes are building
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(6.0);

    // Echoes of the old chain are still heard well after the 20 ms fade.
    const float echoes = peak(h.out[0], switchAt + samples(0.3), switchAt + samples(0.8));
    CHECK_MSG(echoes > 0.05f, "tail level after switch %.4f", echoes);
    CHECK(e.state(0) == ChainState::Idle);
    CHECK(delayCounts.stop == 1);
    // It stopped when quiet, not at the cap.
    const uint64_t processedAfter = delayCounts.frames - samples(0.25);
    CHECK_MSG(processedAfter < samples(5.0), "tail processed for %.2f s", processedAfter / kRate);
    CHECK(maxStep(h.out[0], switchAt) < 0.05f);
}

TEST(infinite_tail_is_capped_with_a_fade) {
    EngineSettings s = settings();
    s.tailCapMs = 2000.0f;
    Engine e(s);
    e.addChain(chain("Forever", slots(new DelayProcessor(200.0f, 1.0f, 1.0f))));
    e.addChain(chain("Silent", slots(new GainProcessor(0.0f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(3.0);
    CHECK(e.state(0) == ChainState::Idle);
    // The release starts after the new chain's 50 ms pre-roll; the cap
    // counts from there. Loud up to the cap, silent after the stop fade.
    CHECK(peak(h.out[0], switchAt + samples(1.85), switchAt + samples(2.0)) > 0.1f);
    CHECK(peak(h.out[0], switchAt + samples(2.15), h.out[0].size()) == 0.0f);
    const float stepAtStop = maxStep(h.out[0], switchAt + samples(1.95), switchAt + samples(2.2));
    const float stepBefore = maxStep(h.out[0], switchAt + samples(1.0), switchAt + samples(1.95));
    CHECK_MSG(stepAtStop <= stepBefore * 1.05f, "step at stop %.5f, before %.5f", stepAtStop, stepBefore);
}

TEST(warm_chain_switches_without_preroll) {
    CountingProcessor::Counts b;
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    e.addChain(chain("B", slots(new CountingProcessor(std::make_unique<GainProcessor>(0.5f), b))));
    CHECK(e.activate());
    Harness h{e, [] { return 0.5f; }};
    h.settleOn(0);
    e.setWarm(1);
    h.runSeconds(1.0);
    CHECK(e.state(1) == ChainState::Warm);
    CHECK(b.frames >= samples(0.99));                          // processing in the background
    CHECK(std::fabs(h.out[0].back() - 0.5f) < 1e-6f);          // but not heard

    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(0.2);
    CHECK_MSG(std::fabs(h.out[0][switchAt + 8] - 0.5f) > 1e-5f, "warm switch did not start in the first block");
    CHECK(std::fabs(h.out[0].back() - 0.25f) < 1e-4f);
}

TEST(warm_chain_delay_is_cleared_on_switch) {
    CountingProcessor::Counts delay, amp;
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    e.addChain(chain("B", slots(new CountingProcessor(std::make_unique<DriveProcessor>(2.0f), amp),
                                new CountingProcessor(std::make_unique<DelayProcessor>(300.0f, 0.6f, 0.5f), delay))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    e.setWarm(1);
    h.runSeconds(1.0);
    const uint64_t delayResets = delay.reset, ampResets = amp.reset;
    e.switchTo(1);
    h.runSeconds(0.1);
    CHECK(delay.reset == delayResets + 1); // the delay starts empty
    CHECK(amp.reset == ampResets);         // the amp stays settled
}

TEST(newer_request_cancels_a_preroll) {
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    e.addChain(chain("B", slots(new GainProcessor(0.5f))));
    e.addChain(chain("C", slots(new GainProcessor(0.25f))));
    CHECK(e.activate());
    Harness h{e, [] { return 0.5f; }};
    h.settleOn(0);
    e.switchTo(1);
    h.run(2);
    CHECK(e.pendingChain() == 1 && e.state(1) == ChainState::PreRoll);
    e.switchTo(2);
    h.run(1);
    CHECK(e.state(1) == ChainState::Idle);
    CHECK(e.pendingChain() == 2);
    CHECK(e.activeChain() == 0);
    h.runSeconds(0.3);
    CHECK(e.activeChain() == 2);
    CHECK(std::fabs(h.out[0].back() - 0.125f) < 1e-4f);
}

TEST(switching_back_during_a_tail_needs_no_reset) {
    CountingProcessor::Counts a;
    Engine e(settings());
    e.addChain(chain("A", slots(new CountingProcessor(std::make_unique<DelayProcessor>(400.0f, 0.7f, 0.5f), a))));
    e.addChain(chain("B", slots(new GainProcessor(0.5f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const uint64_t resets = a.reset;
    e.switchTo(1);
    h.runSeconds(0.3);
    CHECK(e.state(0) == ChainState::Releasing);
    const size_t backAt = h.out[0].size();
    e.switchTo(0);
    h.runSeconds(0.3);
    CHECK(e.activeChain() == 0);
    CHECK(a.reset == resets);
    CHECK(maxStep(h.out[0], backAt) < 0.05f);
}

TEST(out2_taps_before_the_end_of_the_chain) {
    // Out 2 after the first slot (like before the IR loader), out 1 at the end.
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(0.5f), new GainProcessor(0.5f)), 1));
    CHECK(e.activate());
    Harness h{e, [] { return 1.0f; }};
    h.settleOn(0);
    CHECK(std::fabs(h.out[0].back() - 0.25f) < 1e-6f);
    CHECK(std::fabs(h.out[1].back() - 0.5f) < 1e-6f);
}

TEST(random_switching_stays_sane) {
    std::mt19937 rng(1234);
    CountingProcessor::Counts counts[4];
    Engine e(settings());
    e.addChain(chain("A", slots(new CountingProcessor(std::make_unique<DriveProcessor>(4.0f), counts[0]))));
    e.addChain(chain("B", slots(new CountingProcessor(std::make_unique<DelayProcessor>(120.0f, 0.6f, 0.5f), counts[1])), 0));
    e.addChain(chain("C", slots(new CountingProcessor(std::make_unique<LowpassProcessor>(500.0f), counts[2]))));
    e.addChain(chain("D", slots(new CountingProcessor(std::make_unique<DelayProcessor>(700.0f, 0.95f, 0.7f), counts[3]))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    size_t nextSwitch = 0;
    h.run(h.blocksFor(60.0), [&](size_t b) {
        if (b == nextSwitch) {
            if (rng() % 5 == 0)
                e.setWarm(static_cast<int>(rng() % 5) - 1);
            else
                e.switchTo(static_cast<int>(rng() % 4));
            nextSwitch = b + 1 + rng() % 30;
        }
        int active = 0;
        for (size_t i = 0; i < 4; ++i)
            active += e.state(i) == ChainState::Active;
        if (active > 1)
            CHECK_MSG(false, "%d chains active at block %zu", active, b);
    });
    for (auto& out : h.out) {
        bool finite = true;
        for (float v : out)
            finite = finite && std::isfinite(v);
        CHECK(finite);
        CHECK(peak(out, 0, out.size()) < 6.0f);
    }
    for (auto& c : counts)
        CHECK(c.processWhileStopped == 0);
}

TEST(warm_switch_into_a_delay_has_no_late_click) {
    // The warm chain's delay is reset at the switch; its first echo, one
    // delay time later, must not start with a click.
    auto makeA = [] { return chain("A", slots(new GainProcessor(0.4f))); };
    auto makeB = [] { return chain("B", slots(new DriveProcessor(6.0f), new DelayProcessor(300.0f, 0.5f, 0.7f))); };
    const float steady = std::max(steadyStep(makeA), steadyStep(makeB));
    Engine e(settings());
    e.addChain(makeA());
    e.addChain(makeB());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    e.setWarm(1);
    h.runSeconds(1.0);
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(1.5);
    const float worst = maxStep(h.out[0], switchAt);
    CHECK_MSG(worst <= 3.0f * steady, "largest step %.5f, steady chains reach %.5f", worst, steady);
}

TEST(branch_feeds_only_its_own_output) {
    // Main line: gain 0.5 then gain 0.5 again, heard on out 2. A branch off
    // the main line after its first slot holds gain 0.2, heard on out 1:
    // like an IR loader that only FOH gets.
    std::vector<Line> lines(2);
    lines[0].slots.push_back(std::make_unique<GainProcessor>(0.5f));
    lines[0].slots.push_back(std::make_unique<GainProcessor>(0.5f));
    lines[1].parent = 0;
    lines[1].afterSlots = 1;
    lines[1].slots.push_back(std::make_unique<GainProcessor>(0.2f));
    std::vector<OutputTap> taps{{1, SIZE_MAX, OutputMix::Sum}, {0, SIZE_MAX, OutputMix::Sum}};
    Engine e(settings());
    e.addChain(std::make_unique<Chain>("Branch", std::move(lines), std::move(taps)));
    CHECK(e.activate());
    Harness h{e, [] { return 1.0f; }};
    h.settleOn(0);
    CHECK_MSG(std::fabs(h.out[0].back() - 0.1f) < 1e-5f, "out 1 %.5f, expected 0.1 (0.5 x 0.2)", h.out[0].back());
    CHECK_MSG(std::fabs(h.out[1].back() - 0.25f) < 1e-5f, "out 2 %.5f, expected 0.25 (0.5 x 0.5)", h.out[1].back());
}

TEST(bypass_is_not_processed_and_crossfades_without_clicks) {
    CountingProcessor::Counts drive;
    auto make = [&](CountingProcessor::Counts& counts) {
        return chain("A", slots(new GainProcessor(0.8f), new CountingProcessor(std::make_unique<DriveProcessor>(6.0f), counts),
                                new LowpassProcessor(3000.0f)));
    };
    CountingProcessor::Counts unused;
    const float steady = std::max(steadyStep([&] { return make(unused); }),
                                  steadyStep([] { return chain("dry", slots(new GainProcessor(0.8f), new LowpassProcessor(3000.0f))); }));
    Engine e(settings());
    e.addChain(make(drive));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    // Bypass and bring back the drive every 0.3 s.
    h.run(h.blocksFor(3.0), [&](size_t b) {
        if (b % h.blocksFor(0.3) == 0)
            e.setBypass(0, 1, (b / h.blocksFor(0.3)) % 2 == 0);
    });
    const float worst = maxStep(h.out[0], from);
    CHECK_MSG(worst <= 2.0f * steady, "largest step %.5f, steady reaches %.5f", worst, steady);

    // Fully bypassed: no more process calls.
    e.setBypass(0, 1, true);
    h.runSeconds(0.1);
    const uint64_t before = drive.process.load();
    h.runSeconds(0.5);
    CHECK_MSG(drive.process.load() == before, "bypassed slot processed %llu more blocks",
              (unsigned long long)(drive.process.load() - before));
    // The dry signal passes: gain 0.8 then the lowpass, no drive.
    const float p = peak(h.out[0], h.out[0].size() - samples(0.1), h.out[0].size());
    CHECK_MSG(p > 0.35f && p < 0.41f, "bypassed peak %.3f, expected about 0.4 (0.5 x 0.8)", p);
}

TEST(bypass_on_an_idle_chain_applies_at_once) {
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    e.addChain(chain("B", slots(new GainProcessor(0.5f), new GainProcessor(0.1f))));
    CHECK(e.activate());
    Harness h{e, [] { return 1.0f; }};
    h.settleOn(0);
    e.setBypass(1, 1, true); // B is idle: the 0.1 gain is bypassed before B is ever heard
    h.settleOn(1);
    CHECK_MSG(std::fabs(h.out[0].back() - 0.5f) < 1e-5f, "B plays %.5f, expected 0.5 with its second slot bypassed", h.out[0].back());
}

TEST(mute_fades_every_output_and_comes_back) {
    Engine e(settings());
    e.addChain(chain("A", slots(new GainProcessor(1.0f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    e.setMute(true);
    h.runSeconds(0.1);
    CHECK(peak(h.out[0], h.out[0].size() - samples(0.05), h.out[0].size()) == 0.0f);
    CHECK(peak(h.out[1], h.out[1].size() - samples(0.05), h.out[1].size()) == 0.0f);
    e.setMute(false);
    h.runSeconds(0.1);
    CHECK(peak(h.out[0], h.out[0].size() - samples(0.05), h.out[0].size()) > 0.45f);
    // 10 ms fades on a 110 Hz sine: no step much bigger than the sine's own.
    const float sineStep = 0.5f * 2.0f * kPi * 110.0f / static_cast<float>(kRate);
    CHECK_MSG(maxStep(h.out[0], from) <= 1.5f * sineStep, "largest step %.5f, sine %.5f", maxStep(h.out[0], from), sineStep);
}

TEST(bypass_click_detector_catches_a_hard_cut) {
    // Without the crossfade the same bypassing must be flagged.
    auto make = [] { return chain("A", slots(new GainProcessor(0.8f), new DriveProcessor(6.0f), new LowpassProcessor(3000.0f))); };
    const float steady = std::max(steadyStep(make),
                                  steadyStep([] { return chain("dry", slots(new GainProcessor(0.8f), new LowpassProcessor(3000.0f))); }));
    EngineSettings s = settings();
    s.bypassFadeMs = 0.0f;
    Engine e(s);
    e.addChain(make());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    h.run(h.blocksFor(3.0), [&](size_t b) {
        if (b % h.blocksFor(0.3) == 3)
            e.setBypass(0, 1, (b / h.blocksFor(0.3)) % 2 == 0);
    });
    CHECK_MSG(maxStep(h.out[0], from) > 2.0f * steady, "hard bypass not detected: %.5f vs %.5f", maxStep(h.out[0], from), steady);
}
