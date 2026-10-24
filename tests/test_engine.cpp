// Offline tests of the scene engine: no audio hardware, no plug-ins.
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

using Chain = std::vector<std::unique_ptr<Processor>>;

template <typename... Ps>
Chain chain(Ps... ps) {
    Chain c;
    (c.push_back(std::unique_ptr<Processor>(ps)), ...);
    return c;
}

// Both outputs from the end of the chain unless told otherwise.
std::unique_ptr<Scene> scene(const char* name, Chain c, size_t out2After = SIZE_MAX) {
    const size_t n = c.size();
    std::vector<OutputTap> taps{{n, OutputMix::Sum}, {std::min(out2After, n), OutputMix::Sum}};
    return std::make_unique<Scene>(name, std::move(c), std::move(taps));
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
    // Runs until the scene is active and its fade-in has finished.
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

// Steady-state largest sample-to-sample step of a scene playing the sine alone.
float steadyStep(std::function<std::unique_ptr<Scene>()> make, int output = 0) {
    Engine e(settings());
    e.addScene(make());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const size_t from = h.out[0].size();
    h.runSeconds(1.0);
    return maxStep(h.out[output], from);
}

} // namespace

TEST(idle_scenes_are_never_processed) {
    CountingProcessor::Counts a, b, c;
    Engine e(settings());
    e.addScene(scene("A", chain(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), a))));
    e.addScene(scene("B", chain(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), b))));
    e.addScene(scene("C", chain(new CountingProcessor(std::make_unique<GainProcessor>(1.0f), c))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    h.runSeconds(2.0);
    CHECK(a.process > 0);
    CHECK_MSG(b.process == 0 && c.process == 0, "B processed %llu blocks, C %llu", (unsigned long long)b.process.load(),
              (unsigned long long)c.process.load());
    CHECK(b.reset == 0 && c.reset == 0 && b.start == 0 && c.start == 0);
    CHECK(e.activeScene() == 0);
}

TEST(nothing_plays_before_the_first_scene) {
    Engine e(settings());
    e.addScene(scene("A", chain(new GainProcessor(1.0f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.runSeconds(0.5);
    CHECK(peak(h.out[0], 0, h.out[0].size()) == 0.0f);
    CHECK(e.activeScene() == -1);
}

TEST(preroll_delays_the_new_scene_by_the_preroll_time) {
    EngineSettings s = settings();
    s.prerollMs = 50.0f;
    CountingProcessor::Counts b;
    Engine e(s);
    e.addScene(scene("A", chain(new GainProcessor(1.0f))));
    e.addScene(scene("B", chain(new CountingProcessor(std::make_unique<GainProcessor>(0.5f), b))));
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
    CHECK(e.activeScene() == 1);
}

TEST(switching_has_no_clicks) {
    // Three different sounds; switch every 0.5 s and look for steps larger
    // than the scenes themselves ever make.
    auto makeA = [] { return scene("A", chain(new DriveProcessor(3.0f), new LowpassProcessor(2000.0f))); };
    auto makeB = [] { return scene("B", chain(new GainProcessor(0.3f), new LowpassProcessor(800.0f))); };
    auto makeC = [] { return scene("C", chain(new DriveProcessor(6.0f), new DelayProcessor(300.0f, 0.5f, 0.5f)), 1); };
    const float steady = std::max({steadyStep(makeA), steadyStep(makeB), steadyStep(makeC)});

    Engine e(settings());
    e.addScene(makeA());
    e.addScene(makeB());
    e.addScene(makeC());
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
    // Two scenes overlap during a fade (three when a tail is ringing), so
    // allow their steps to add, but nothing like a cut.
    const float worst = maxStep(h.out[0], from);
    CHECK_MSG(worst <= 3.0f * steady, "largest step %.5f, steady scenes reach %.5f", worst, steady);
    const float worst2 = maxStep(h.out[1], from);
    CHECK_MSG(worst2 <= 3.0f * steady, "out 2 largest step %.5f, steady scenes reach %.5f", worst2, steady);
}

TEST(click_detector_catches_a_hard_cut) {
    // The same switch with no pre-roll and no crossfade must be flagged,
    // otherwise the test above proves nothing.
    auto makeA = [] { return scene("A", chain(new DriveProcessor(3.0f), new LowpassProcessor(2000.0f))); };
    auto makeB = [] { return scene("B", chain(new GainProcessor(0.3f), new LowpassProcessor(800.0f))); };
    const float steady = std::max(steadyStep(makeA), steadyStep(makeB));
    EngineSettings s = settings();
    s.prerollMs = 0.0f;
    s.crossfadeMs = 0.0f;
    Engine e(s);
    e.addScene(makeA());
    e.addScene(makeB());
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

TEST(tail_continues_after_switch_and_scene_goes_idle) {
    EngineSettings s = settings();
    s.tailCapMs = 20000.0f;
    CountingProcessor::Counts delayCounts;
    Engine e(s);
    e.addScene(scene("Delay", chain(new CountingProcessor(std::make_unique<DelayProcessor>(250.0f, 0.5f, 1.0f), delayCounts))));
    e.addScene(scene("Silent", chain(new GainProcessor(0.0f))));
    CHECK(e.activate());

    size_t t = 0;
    Sine sine;
    Harness h{e, [&] { return t < samples(0.35) ? (t++, sine()) : (t++, 0.0f); }};
    h.settleOn(0); // 0.25 s of playing, echoes are building
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(6.0);

    // Echoes of the old scene are still heard well after the 20 ms fade.
    const float echoes = peak(h.out[0], switchAt + samples(0.3), switchAt + samples(0.8));
    CHECK_MSG(echoes > 0.05f, "tail level after switch %.4f", echoes);
    CHECK(e.state(0) == SceneState::Idle);
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
    e.addScene(scene("Forever", chain(new DelayProcessor(200.0f, 1.0f, 1.0f))));
    e.addScene(scene("Silent", chain(new GainProcessor(0.0f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(3.0);
    CHECK(e.state(0) == SceneState::Idle);
    // The release starts after the new scene's 50 ms pre-roll; the cap
    // counts from there. Loud up to the cap, silent after the stop fade.
    CHECK(peak(h.out[0], switchAt + samples(1.85), switchAt + samples(2.0)) > 0.1f);
    CHECK(peak(h.out[0], switchAt + samples(2.15), h.out[0].size()) == 0.0f);
    const float stepAtStop = maxStep(h.out[0], switchAt + samples(1.95), switchAt + samples(2.2));
    const float stepBefore = maxStep(h.out[0], switchAt + samples(1.0), switchAt + samples(1.95));
    CHECK_MSG(stepAtStop <= stepBefore * 1.05f, "step at stop %.5f, before %.5f", stepAtStop, stepBefore);
}

TEST(warm_scene_switches_without_preroll) {
    CountingProcessor::Counts b;
    Engine e(settings());
    e.addScene(scene("A", chain(new GainProcessor(1.0f))));
    e.addScene(scene("B", chain(new CountingProcessor(std::make_unique<GainProcessor>(0.5f), b))));
    CHECK(e.activate());
    Harness h{e, [] { return 0.5f; }};
    h.settleOn(0);
    e.setWarm(1);
    h.runSeconds(1.0);
    CHECK(e.state(1) == SceneState::Warm);
    CHECK(b.frames >= samples(0.99));                          // processing in the background
    CHECK(std::fabs(h.out[0].back() - 0.5f) < 1e-6f);          // but not heard

    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(0.2);
    CHECK_MSG(std::fabs(h.out[0][switchAt + 8] - 0.5f) > 1e-5f, "warm switch did not start in the first block");
    CHECK(std::fabs(h.out[0].back() - 0.25f) < 1e-4f);
}

TEST(warm_scene_delay_is_cleared_on_switch) {
    CountingProcessor::Counts delay, amp;
    Engine e(settings());
    e.addScene(scene("A", chain(new GainProcessor(1.0f))));
    e.addScene(scene("B", chain(new CountingProcessor(std::make_unique<DriveProcessor>(2.0f), amp),
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
    e.addScene(scene("A", chain(new GainProcessor(1.0f))));
    e.addScene(scene("B", chain(new GainProcessor(0.5f))));
    e.addScene(scene("C", chain(new GainProcessor(0.25f))));
    CHECK(e.activate());
    Harness h{e, [] { return 0.5f; }};
    h.settleOn(0);
    e.switchTo(1);
    h.run(2);
    CHECK(e.pendingScene() == 1 && e.state(1) == SceneState::PreRoll);
    e.switchTo(2);
    h.run(1);
    CHECK(e.state(1) == SceneState::Idle);
    CHECK(e.pendingScene() == 2);
    CHECK(e.activeScene() == 0);
    h.runSeconds(0.3);
    CHECK(e.activeScene() == 2);
    CHECK(std::fabs(h.out[0].back() - 0.125f) < 1e-4f);
}

TEST(switching_back_during_a_tail_needs_no_reset) {
    CountingProcessor::Counts a;
    Engine e(settings());
    e.addScene(scene("A", chain(new CountingProcessor(std::make_unique<DelayProcessor>(400.0f, 0.7f, 0.5f), a))));
    e.addScene(scene("B", chain(new GainProcessor(0.5f))));
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    const uint64_t resets = a.reset;
    e.switchTo(1);
    h.runSeconds(0.3);
    CHECK(e.state(0) == SceneState::Releasing);
    const size_t backAt = h.out[0].size();
    e.switchTo(0);
    h.runSeconds(0.3);
    CHECK(e.activeScene() == 0);
    CHECK(a.reset == resets);
    CHECK(maxStep(h.out[0], backAt) < 0.05f);
}

TEST(out2_taps_before_the_end_of_the_chain) {
    // Out 2 after the first slot (like before the IR loader), out 1 at the end.
    Engine e(settings());
    e.addScene(scene("A", chain(new GainProcessor(0.5f), new GainProcessor(0.5f)), 1));
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
    e.addScene(scene("A", chain(new CountingProcessor(std::make_unique<DriveProcessor>(4.0f), counts[0]))));
    e.addScene(scene("B", chain(new CountingProcessor(std::make_unique<DelayProcessor>(120.0f, 0.6f, 0.5f), counts[1])), 0));
    e.addScene(scene("C", chain(new CountingProcessor(std::make_unique<LowpassProcessor>(500.0f), counts[2]))));
    e.addScene(scene("D", chain(new CountingProcessor(std::make_unique<DelayProcessor>(700.0f, 0.95f, 0.7f), counts[3]))));
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
            active += e.state(i) == SceneState::Active;
        if (active > 1)
            CHECK_MSG(false, "%d scenes active at block %zu", active, b);
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
    // The warm scene's delay is reset at the switch; its first echo, one
    // delay time later, must not start with a click.
    auto makeA = [] { return scene("A", chain(new GainProcessor(0.4f))); };
    auto makeB = [] { return scene("B", chain(new DriveProcessor(6.0f), new DelayProcessor(300.0f, 0.5f, 0.7f))); };
    const float steady = std::max(steadyStep(makeA), steadyStep(makeB));
    Engine e(settings());
    e.addScene(makeA());
    e.addScene(makeB());
    CHECK(e.activate());
    Harness h{e, Sine{}};
    h.settleOn(0);
    e.setWarm(1);
    h.runSeconds(1.0);
    e.switchTo(1);
    const size_t switchAt = h.out[0].size();
    h.runSeconds(1.5);
    const float worst = maxStep(h.out[0], switchAt);
    CHECK_MSG(worst <= 3.0f * steady, "largest step %.5f, steady scenes reach %.5f", worst, steady);
}
