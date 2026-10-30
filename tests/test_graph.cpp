// Offline tests of chains as graphs: blocks joined by wires that split and
// merge, what a wire does to its signal, and changes made while a chain
// plays. Like test_engine.cpp, each test drives Engine::process() block by
// block, as the audio thread would.

#include "check.h"

#include "core/builtin.h"
#include "core/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace cue;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 64;

EngineSettings settings() {
    EngineSettings s;
    s.sampleRate = kRate;
    s.maxFrames = kBlock;
    s.numOutputs = 2;
    return s;
}

// The left channel of its input on the left, silence on the right: a
// stereo source, which the chain's mono input is not.
class LeftOnly : public BuiltinProcessor {
public:
    LeftOnly() : BuiltinProcessor("Left only") {}
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        for (uint32_t n = 0; n < frames; ++n) {
            out[0][n] = in[0][n];
            out[1][n] = 0.0f;
        }
    }
};

// Plays its side-chain input instead of its main input.
class KeyOnly : public BuiltinProcessor {
public:
    KeyOnly() : BuiltinProcessor("Key") {}
    bool hasSidechain() const override { return true; }
    void setSidechain(const float* const* key) override {
        for (int c = 0; c < kChainChannels; ++c)
            key_[c] = key ? key[c] : nullptr;
    }
    void process(const float* const*, float* const* out, uint32_t frames) override {
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                out[c][n] = key_[c] ? key_[c][n] : 0.0f;
    }

private:
    const float* key_[kChainChannels] = {};
};

std::unique_ptr<Processor> gain(float g) { return std::make_unique<GainProcessor>(g); }

std::unique_ptr<Processor> counted(float g, CountingProcessor::Counts& counts) {
    return std::make_unique<CountingProcessor>(gain(g), counts);
}

// Plays the engine and keeps both outputs.
struct Run {
    Engine& engine;
    std::function<float()> input = [] { return 1.0f; };
    std::vector<float> out[2];

    void blocks(size_t count, const std::function<void(size_t)>& before = nullptr) {
        std::vector<float> in(kBlock), o1(kBlock), o2(kBlock);
        float* outs[2] = {o1.data(), o2.data()};
        for (size_t b = 0; b < count; ++b) {
            if (before)
                before(b);
            for (auto& v : in)
                v = input();
            engine.process(in.data(), outs, kBlock);
            out[0].insert(out[0].end(), o1.begin(), o1.end());
            out[1].insert(out[1].end(), o2.begin(), o2.end());
        }
    }
    void seconds(double s) { blocks(static_cast<size_t>(s * kRate / kBlock)); }
    float last(int o) const { return out[o].back(); }
};

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

float maxStep(const std::vector<float>& x, size_t from, size_t to = SIZE_MAX) {
    float m = 0.0f;
    for (size_t n = std::max<size_t>(from, 1); n < std::min(to, x.size()); ++n)
        m = std::max(m, std::fabs(x[n] - x[n - 1]));
    return m;
}

// The chain's slots kept as they are, in order.
std::vector<int> same(const Chain& c) {
    std::vector<int> s;
    for (size_t i = 0; i < c.numSlots(); ++i)
        s.push_back(static_cast<int>(i));
    return s;
}

} // namespace

TEST(wires_into_one_input_are_summed) {
    // Two blocks side by side, both into output 1; output 2 hears one.
    auto c = std::make_unique<Chain>("Merge", 2);
    c->addBlock(gain(0.5f));
    c->addBlock(gain(0.25f));
    CHECK(c->connect(Port::input(), Port::block(0)));
    CHECK(c->connect(Port::input(), Port::block(1)));
    CHECK(c->connect(Port::block(0), Port::output(0)));
    CHECK(c->connect(Port::block(1), Port::output(0)));
    CHECK(c->connect(Port::block(0), Port::output(1)));
    Engine e(settings());
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK_MSG(near(r.last(0), 0.75f) && near(r.last(1), 0.5f), "plays %.5f and %.5f, expected 0.75 and 0.5", r.last(0), r.last(1));
}

TEST(a_wire_sets_level_then_width_then_balance) {
    // A stereo signal, left only, to two outputs: one takes the left, the other the right.
    auto c = std::make_unique<Chain>("Wire", 2);
    c->addBlock(std::make_unique<LeftOnly>());
    c->setOutputMix(0, OutputMix::Left);
    c->setOutputMix(1, OutputMix::Right);
    CHECK(c->connect(Port::input(), Port::block(0)));
    CHECK(c->connect(Port::block(0), Port::output(0)));
    CHECK(c->connect(Port::block(0), Port::output(1)));
    Engine e(settings());
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(near(r.last(0), 1.0f) && near(r.last(1), 0.0f));
    auto both = [&](WireGains g) {
        CHECK(e.setWire(0, Port::block(0), Port::output(0), g));
        CHECK(e.setWire(0, Port::block(0), Port::output(1), g));
        r.seconds(0.1);
    };
    both({-6.0206f, 1.0f, 0.0f});
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 0.0f), "-6 dB plays %.5f / %.5f", r.last(0), r.last(1));
    both({0.0f, 0.0f, 0.0f}); // mono: the left shared by both sides
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 0.5f), "width 0 plays %.5f / %.5f", r.last(0), r.last(1));
    both({0.0f, 0.0f, -1.0f}); // then to the left only
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 0.0f), "pan left plays %.5f / %.5f", r.last(0), r.last(1));
    both({0.0f, 0.0f, 0.5f}); // half right: the left side down by half
    CHECK_MSG(near(r.last(0), 0.25f) && near(r.last(1), 0.5f), "pan half right plays %.5f / %.5f", r.last(0), r.last(1));
    both({0.0f, 2.0f, 0.0f}); // twice as wide
    CHECK_MSG(near(r.last(0), 1.5f) && near(r.last(1), -0.5f), "width 2 plays %.5f / %.5f", r.last(0), r.last(1));
    both({kWireOffDb, 1.0f, 0.0f});
    CHECK(r.last(0) == 0.0f && r.last(1) == 0.0f);
    CHECK(!e.setWire(0, Port::input(), Port::output(0), {})); // no such wire
}

TEST(wire_gains_move_without_clicks) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(1.0f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    const size_t from = r.out[0].size();
    e.setWire(0, Port::block(0), Port::output(0), {-20.0f, 1.0f, 0.0f});
    r.seconds(0.1);
    CHECK(near(r.last(0), 0.1f));
    // 0.9 over the 10 ms fade: about 0.0019 a sample.
    CHECK_MSG(maxStep(r.out[0], from) < 0.0025f, "step %.5f", maxStep(r.out[0], from));
}

TEST(each_wire_has_a_meter_after_its_gains) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(0.5f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::output(0), {-6.0206f, 1.0f, 0.0f});
    c->connect(Port::block(0), Port::output(1), {0.0f, 1.0f, 0.0f, true});
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    Chain& chain = e.chain(0);
    uint32_t in = 0, foh = 0, amp = 0;
    for (const Wire& w : chain.wires())
        (w.from.kind == Port::Kind::Input ? in : w.to.index == 0 ? foh : amp) = w.id;
    CHECK(std::fabs(chain.takeWirePeak(in) - 1.0f) < 1e-4f);
    CHECK(std::fabs(chain.takeWirePeak(foh) - 0.25f) < 1e-3f);
    CHECK(chain.takeWirePeak(amp) == 0.0f); // muted
    // Taken: nothing until it plays again.
    CHECK(chain.takeWirePeak(foh) == 0.0f);
    r.seconds(0.01);
    CHECK(chain.takeWirePeak(foh) > 0.2f);
}

TEST(the_probe_measures_what_a_block_is_fed_and_passes_on) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(0.5f));
    c->connect(Port::input(), Port::block(0), {-6.0206f, 1.0f, 0.0f});
    c->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.05);
    Chain& chain = e.chain(0);
    Chain::ProbeBin bin;
    CHECK(!chain.nextProbeBin(bin)); // nothing until asked
    auto measure = [&](int slot, float& fed, float& out, float& fedPeak, float& outPeak) {
        chain.probe(slot);
        r.seconds(0.2);
        uint32_t frames = 0;
        int bins = 0;
        double fe = 0, oe = 0;
        fedPeak = outPeak = 0;
        while (chain.nextProbeBin(bin)) {
            CHECK(bin.slot == slot);
            frames += bin.frames;
            fe += bin.fed.energy;
            oe += bin.out.energy;
            fedPeak = std::max(fedPeak, bin.fed.peak);
            outPeak = std::max(outPeak, bin.out.peak);
            CHECK(bin.frames >= 480 && bin.frames < 480 + 64);
            ++bins;
        }
        CHECK(bins >= 15);
        fed = static_cast<float>(fe / frames);
        out = static_cast<float>(oe / frames);
    };
    float fed = 0, out = 0, fedPeak = 0, outPeak = 0;
    measure(0, fed, out, fedPeak, outPeak);
    CHECK(std::fabs(fedPeak - 0.5f) < 1e-3f && std::fabs(outPeak - 0.25f) < 1e-3f);
    CHECK(std::fabs(fed - 0.25f) < 1e-3f && std::fabs(out - 0.0625f) < 1e-3f);
    // The chain's input: what it is fed is what it passes on.
    measure(Chain::kProbeInput, fed, out, fedPeak, outPeak);
    CHECK(std::fabs(fedPeak - 1.0f) < 1e-4f && fed == out && fedPeak == outPeak);
    chain.probe(Chain::kProbeNone);
    r.seconds(0.05);
    CHECK(!chain.nextProbeBin(bin));
}

TEST(blocks_nothing_reaches_or_nobody_hears_are_not_processed) {
    CountingProcessor::Counts a, loose, deadEnd;
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(counted(0.5f, a));
    c->addBlock(counted(1.0f, loose));   // no wires at all
    c->addBlock(counted(1.0f, deadEnd)); // fed, but feeds nothing
    CHECK(c->connect(Port::input(), Port::block(0)));
    CHECK(c->connect(Port::block(0), Port::output(0)));
    CHECK(c->connect(Port::block(0), Port::block(2)));
    Engine e(settings());
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(a.process > 0 && loose.process == 0 && deadEnd.process == 0);
    CHECK(loose.start == 0 && deadEnd.start == 0);
    CHECK(near(r.last(0), 0.5f) && r.last(1) == 0.0f); // nothing feeds output 2
}

TEST(wires_that_do_not_fit_and_loops_are_refused) {
    Chain c("A", 2);
    c.addBlock(gain(1.0f));
    c.addBlock(gain(1.0f));
    c.addBlock(std::make_unique<KeyOnly>());
    std::string why;
    CHECK(c.connect(Port::input(), Port::block(0)));
    CHECK(c.connect(Port::block(0), Port::block(1)));
    CHECK(!c.connect(Port::block(1), Port::block(0), {}, &why) && why.find("loop") != std::string::npos);
    CHECK(!c.connect(Port::block(1), Port::block(1), {}, &why));
    CHECK(!c.connect(Port::block(0), Port::block(1), {}, &why) && why.find("already") != std::string::npos);
    CHECK(!c.connect(Port::block(0), Port::input(), {}, &why));
    CHECK(!c.connect(Port::output(0), Port::block(1), {}, &why));
    CHECK(!c.connect(Port::block(0), Port::output(2), {}, &why));
    CHECK(!c.connect(Port::block(5), Port::output(0), {}, &why));
    CHECK(!c.connect(Port::block(0), Port::block(1, true), {}, &why) && why.find("side-chain") != std::string::npos);
    CHECK(c.connect(Port::block(1), Port::block(2, true)));
    // A key counts for loops too.
    CHECK(c.connect(Port::block(2), Port::output(0)));
    CHECK(!c.connect(Port::block(2), Port::block(0), {}, &why));
    CHECK(c.wires().size() == 4);
}

TEST(a_feedback_wire_is_heard_a_block_late_and_cannot_run_away) {
    Chain c("A", 2);
    c.addBlock(gain(1.0f));
    c.addBlock(gain(1.0f));
    std::string why;
    CHECK(c.connect(Port::input(), Port::block(0)));
    CHECK(c.connect(Port::block(0), Port::block(1)));
    // Back to where it came from, and into itself: only as feedback.
    CHECK(c.connect(Port::block(1), Port::block(0), {}, &why, true));
    CHECK(c.connect(Port::block(1), Port::block(1), {}, &why, true));
    CHECK(!c.connect(Port::input(), Port::block(1), {}, &why, true) && why.find("feedback") != std::string::npos);
    CHECK(!c.connect(Port::block(1), Port::output(0), {}, &why, true));

    // One plug-in into itself at half its level: 1 + 1/2 + 1/4 ... = 2.
    auto play = [](float levelDb) {
        auto ch = std::make_unique<Chain>("Echo", 2);
        ch->addBlock(gain(1.0f));
        CHECK(ch->connect(Port::input(), Port::block(0)));
        CHECK(ch->connect(Port::block(0), Port::output(0)));
        WireGains g;
        g.levelDb = levelDb;
        std::string err;
        CHECK_MSG(ch->connect(Port::block(0), Port::block(0), g, &err, true), "%s", err.c_str());
        auto e = std::make_unique<Engine>(settings());
        e->addChain(std::move(ch));
        CHECK(e->activate());
        e->switchTo(0);
        return e;
    };
    {
        auto e = play(20 * std::log10(0.5f));
        Run r{*e};
        r.seconds(0.5);
        CHECK_MSG(near(r.last(0), 2.0f), "plays %.5f, expected 2", r.last(0));
        // A block late: it climbs in steps of a block, never within one.
        Run steps{*e};
        steps.input = [] { return 0.0f; };
        steps.blocks(2);
        CHECK_MSG(near(steps.out[0][0], 1.0f) && near(steps.out[0][kBlock - 1], 1.0f) && near(steps.out[0][kBlock], 0.5f),
                  "dies away as %.5f, %.5f, %.5f", steps.out[0][0], steps.out[0][kBlock - 1], steps.out[0][kBlock]);
    }
    {
        // Louder than it came in: held, not growing for ever.
        auto e = play(6.0f);
        Run r{*e};
        r.seconds(1.0);
        const float most = *std::max_element(r.out[0].begin(), r.out[0].end());
        CHECK_MSG(std::isfinite(most) && most <= 1.0f + kFeedbackLimit * 2.0f, "reaches %.3f", most);
    }
}

TEST(a_key_wire_feeds_the_side_chain_input) {
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(0.5f));
    c->addBlock(std::make_unique<KeyOnly>());
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::input(), Port::block(1));
    c->connect(Port::block(1), Port::output(0));
    Engine e(settings());
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(r.last(0) == 0.0f); // no key yet: silence
    // The gain is played now that its output keys a block that is heard.
    ChainEdit edit;
    edit.slots = {0, 1};
    edit.wires = e.chain(0).wires();
    edit.wires.push_back({Port::block(0), Port::block(1, true), {}});
    std::string why;
    CHECK_MSG(e.edit(0, std::move(edit), &why), "%s", why.c_str());
    r.seconds(0.1);
    CHECK_MSG(near(r.last(0), 0.5f), "keyed block plays %.5f, expected 0.5", r.last(0));
}

TEST(editing_a_playing_chain_fades_out_and_back_in) {
    CountingProcessor::Counts added;
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(1.0f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::output(0));
    c->connect(Port::block(0), Port::output(1));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(near(r.last(0), 1.0f));
    const size_t from = r.out[0].size();

    // A gain of 0.5 after the first, before output 1 only.
    ChainEdit edit;
    edit.slots = {0, -1};
    edit.added.push_back({counted(0.5f, added)});
    CHECK(edit.added[0].processor->activate(kRate, kBlock));
    edit.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::block(1)}, {Port::block(1), Port::output(0)}, {Port::block(0), Port::output(1)}};
    std::string why;
    CHECK_MSG(e.edit(0, std::move(edit), &why), "%s", why.c_str());
    CHECK(e.chain(0).numSlots() == 2 && e.chain(0).wires().size() == 4);
    const uint64_t primed = added.process;
    CHECK_MSG(primed > 0 && added.start == 1 && added.stop == 1, "primed with %llu blocks", (unsigned long long)primed);
    r.seconds(0.2);
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 1.0f), "plays %.5f / %.5f after the edit", r.last(0), r.last(1));
    CHECK(added.process > primed && added.start == 2 && added.processWhileStopped == 0);
    // Down to silence and up again, never at once.
    const float lowest = *std::min_element(r.out[0].begin() + from, r.out[0].end());
    CHECK_MSG(lowest < 0.01f, "the output only went down to %.4f", lowest);
    CHECK_MSG(maxStep(r.out[0], from) < 0.02f && maxStep(r.out[1], from) < 0.02f, "steps %.5f / %.5f", maxStep(r.out[0], from),
              maxStep(r.out[1], from));
    e.collect();
}

TEST(editing_a_playing_chain_has_no_clicks) {
    auto make = [] {
        auto c = std::make_unique<Chain>("A", 2);
        c->addBlock(std::make_unique<DriveProcessor>(3.0f));
        c->addBlock(std::make_unique<DelayProcessor>(120.0f, 0.4f, 0.5f));
        c->connect(Port::input(), Port::block(0));
        c->connect(Port::block(0), Port::block(1));
        c->connect(Port::block(1), Port::output(0));
        c->connect(Port::block(1), Port::output(1));
        return c;
    };
    Engine e(settings());
    e.addChain(make());
    CHECK(e.activate());
    double phase = 0;
    Run r{e, [&] {
              phase += 2 * 3.14159265 * 110 / kRate;
              return 0.4f * static_cast<float>(std::sin(phase));
          }};
    e.switchTo(0);
    r.seconds(0.5);
    const size_t steadyFrom = r.out[0].size();
    r.seconds(0.5);
    const float steady = maxStep(r.out[0], steadyFrom);
    const size_t from = r.out[0].size();
    // Take the delay out, then put a lowpass in its place, then wire the
    // drive straight to output 1 beside it: each while it plays.
    std::string why;
    ChainEdit out;
    out.slots = {0};
    out.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::output(0)}, {Port::block(0), Port::output(1)}};
    CHECK(e.edit(0, std::move(out), &why));
    r.seconds(0.3);
    ChainEdit in;
    in.slots = {0, -1};
    in.added.push_back({std::make_unique<LowpassProcessor>(800.0f)});
    CHECK(in.added[0].processor->activate(kRate, kBlock));
    in.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::block(1)}, {Port::block(1), Port::output(0)}, {Port::block(1), Port::output(1)}};
    CHECK(e.edit(0, std::move(in), &why));
    r.seconds(0.3);
    ChainEdit beside;
    beside.slots = {0, 1};
    beside.wires = e.chain(0).wires();
    beside.wires.push_back({Port::block(0), Port::output(0), {-6.0f, 1.0f, 0.0f}});
    CHECK(e.edit(0, std::move(beside), &why));
    r.seconds(0.5);
    const float worst = std::max(maxStep(r.out[0], from), maxStep(r.out[1], from));
    CHECK_MSG(worst <= 2.0f * steady, "largest step %.5f, steady reaches %.5f", worst, steady);
    e.collect();
}

TEST(editing_an_idle_chain_takes_effect_at_once) {
    Engine e(settings());
    auto a = std::make_unique<Chain>("A", 2);
    a->addBlock(gain(1.0f));
    a->connect(Port::input(), Port::block(0));
    a->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(a));
    auto b = std::make_unique<Chain>("B", 2);
    b->addBlock(gain(0.5f));
    b->connect(Port::input(), Port::block(0));
    b->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(b));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    // B, idle: its gain to output 2 as well.
    ChainEdit edit;
    edit.slots = {0};
    edit.wires = e.chain(1).wires();
    edit.wires.push_back({Port::block(0), Port::output(1)});
    std::string why;
    CHECK(e.edit(1, std::move(edit), &why));
    r.blocks(1);
    CHECK(!e.chain(1).hasPending());
    e.switchTo(1);
    r.seconds(0.3);
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 0.5f), "B plays %.5f / %.5f", r.last(0), r.last(1));
    e.collect();
}

TEST(removed_blocks_stop_and_are_freed_on_the_main_thread) {
    CountingProcessor::Counts gone;
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(0.5f));
    c->addBlock(counted(0.5f, gone));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::block(1));
    c->connect(Port::block(1), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(near(r.last(0), 0.25f) && gone.start == 1);
    ChainEdit edit;
    edit.slots = {0};
    edit.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::output(0)}};
    std::string why;
    CHECK(e.edit(0, std::move(edit), &why));
    CHECK(e.chain(0).numSlots() == 1);
    r.seconds(0.1);
    CHECK(near(r.last(0), 0.5f));
    CHECK_MSG(gone.stop == 1 && gone.deactivate == 0 && gone.destroyed == 0, "stopped %llu, deactivated %llu", (unsigned long long)gone.stop.load(),
              (unsigned long long)gone.deactivate.load());
    const uint64_t blocks = gone.process;
    r.seconds(0.1);
    CHECK(gone.process == blocks);
    e.collect();
    CHECK(gone.deactivate == 1 && gone.destroyed == 1);
}

TEST(bypass_after_an_edit_finds_the_block_by_its_new_number) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    for (float g : {0.5f, 0.25f, 0.125f})
        c->addBlock(gain(g));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::block(1));
    c->connect(Port::block(1), Port::block(2));
    c->connect(Port::block(2), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    CHECK(near(r.last(0), 0.015625f));
    // The first goes, so the 0.25 is slot 0 now; bypassed in the same breath.
    ChainEdit edit;
    edit.slots = {1, 2};
    edit.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::block(1)}, {Port::block(1), Port::output(0)}};
    std::string why;
    CHECK(e.edit(0, std::move(edit), &why));
    e.setBypass(0, 0, true);
    r.seconds(0.2);
    CHECK_MSG(near(r.last(0), 0.125f), "plays %.5f, expected 0.125 with the 0.25 bypassed", r.last(0));
    e.collect();
}

TEST(an_edit_that_does_not_fit_changes_nothing) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(0.5f));
    c->addBlock(gain(0.5f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::block(1));
    c->connect(Port::block(1), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    const std::vector<Wire> before = e.chain(0).wires();
    ChainEdit loop;
    loop.slots = same(e.chain(0));
    loop.wires = before;
    loop.wires.push_back({Port::block(1), Port::block(0)});
    std::string why;
    CHECK(!e.edit(0, std::move(loop), &why) && why.find("loop") != std::string::npos);
    ChainEdit twice;
    twice.slots = {0, 0};
    CHECK(!e.edit(0, std::move(twice), &why));
    CHECK(e.chain(0).wires().size() == before.size() && !e.chain(0).hasPending());
    r.seconds(0.1);
    CHECK(near(r.last(0), 0.25f));
}

TEST(a_wire_keeps_its_id_and_gains_through_an_edit) {
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(1.0f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    CHECK(e.setWire(0, Port::input(), Port::block(0), {-6.0206f, 1.0f, 0.0f}));
    r.seconds(0.2);
    CHECK(near(r.last(0), 0.5f));
    const uint32_t id = e.chain(0).wires()[0].id;
    // A gain in front of it: the block the wire went into is slot 1 now.
    ChainEdit edit;
    edit.slots = {-1, 0};
    edit.added.push_back({gain(0.5f)});
    edit.wires = {{Port::input(), Port::block(1), {-6.0206f, 1.0f, 0.0f}}, {Port::block(1), Port::block(0)}, {Port::block(0), Port::output(0)}};
    std::string why;
    CHECK_MSG(e.edit(0, std::move(edit), &why), "%s", why.c_str());
    CHECK(e.chain(0).wires()[0].id == id);
    r.seconds(0.2);
    CHECK_MSG(near(r.last(0), 0.25f), "plays %.5f, expected 0.25", r.last(0)); // played second, though slot 0
    e.collect();
}

TEST(a_newer_edit_replaces_one_waiting_for_its_fade) {
    CountingProcessor::Counts brief;
    Engine e(settings());
    auto c = std::make_unique<Chain>("A", 2);
    c->addBlock(gain(1.0f));
    c->connect(Port::input(), Port::block(0));
    c->connect(Port::block(0), Port::output(0));
    e.addChain(std::move(c));
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    r.seconds(0.2);
    ChainEdit add;
    add.slots = {0, -1};
    add.added.push_back({counted(0.5f, brief)});
    CHECK(add.added[0].processor->activate(kRate, kBlock));
    add.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::block(1)}, {Port::block(1), Port::output(0)}};
    std::string why;
    CHECK(e.edit(0, std::move(add), &why));
    r.blocks(2); // fading out for it
    const uint64_t primed = brief.process;
    ChainEdit undo;
    undo.slots = {0};
    undo.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::output(0)}};
    CHECK(e.edit(0, std::move(undo), &why));
    r.seconds(0.2);
    CHECK(near(r.last(0), 1.0f));
    CHECK_MSG(brief.process == primed, "the replaced edit's gain played %llu blocks", (unsigned long long)(brief.process - primed));
    e.collect();
    CHECK(brief.destroyed == 1);
}

TEST(editing_a_warm_chain_keeps_it_warm) {
    Engine e(settings());
    for (const char* name : {"A", "B"}) {
        auto c = std::make_unique<Chain>(name, 2);
        c->addBlock(gain(name[0] == 'A' ? 1.0f : 0.5f));
        c->connect(Port::input(), Port::block(0));
        c->connect(Port::block(0), Port::output(0));
        e.addChain(std::move(c));
    }
    CHECK(e.activate());
    Run r{e};
    e.switchTo(0);
    e.setWarm(1);
    r.seconds(0.2);
    CHECK(e.state(1) == ChainState::Warm);
    ChainEdit edit;
    edit.slots = {0};
    edit.wires = e.chain(1).wires();
    edit.wires.push_back({Port::block(0), Port::output(1)});
    std::string why;
    CHECK(e.edit(1, std::move(edit), &why));
    r.seconds(0.1);
    CHECK(e.state(1) == ChainState::Warm && !e.chain(1).hasPending());
    CHECK(near(r.last(0), 1.0f) && r.last(1) == 0.0f); // still A alone
    e.switchTo(1);
    r.seconds(0.2);
    CHECK_MSG(near(r.last(0), 0.5f) && near(r.last(1), 0.5f), "B plays %.5f / %.5f", r.last(0), r.last(1));
    e.collect();
}

// The audio thread plays on, with helper threads, while the main thread
// changes both chains over and over: what an edit adds is primed and
// swapped in, what it takes out is freed on the main thread, and nothing
// is left behind. Run under ThreadSanitizer, this is where a race shows.
TEST(edits_while_the_audio_thread_plays) {
    // One count each: started and stopped are each gain's own.
    std::deque<CountingProcessor::Counts> counts;
    auto destroyed = [&] {
        uint64_t n = 0;
        for (const auto& c : counts)
            n += c.destroyed;
        return n;
    };
    int made = 0;
    {
        EngineSettings s = settings();
        s.workers = 2;
        Engine e(s);
        for (const char* name : {"A", "B"}) {
            auto c = std::make_unique<Chain>(name, 2);
            c->addBlock(gain(0.5f));
            c->connect(Port::input(), Port::block(0));
            c->connect(Port::block(0), Port::output(0));
            c->connect(Port::block(0), Port::output(1));
            e.addChain(std::move(c));
        }
        CHECK(e.activate());
        e.switchTo(0);
        e.setWarm(1);
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            std::vector<float> in(kBlock, 0.5f), o1(kBlock), o2(kBlock);
            float* outs[2] = {o1.data(), o2.data()};
            while (!stop.load()) {
                e.process(in.data(), outs, kBlock);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        int playing = 0;
        for (int i = 0; i < 40; ++i) {
            const int chain = i % 2;
            ChainEdit edit;
            if (e.chain(chain).numSlots() == 1) {
                // A gain after the first, on the way to output 0.
                edit.slots = {0, -1};
                edit.added.push_back({counted(0.5f, counts.emplace_back())});
                CHECK(edit.added[0].processor->activate(kRate, kBlock));
                ++made;
                edit.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::block(1)}, {Port::block(1), Port::output(0)},
                              {Port::block(0), Port::output(1)}};
            } else {
                edit.slots = {0};
                edit.wires = {{Port::input(), Port::block(0)}, {Port::block(0), Port::output(0)}, {Port::block(0), Port::output(1)}};
            }
            std::string why;
            CHECK_MSG(e.edit(chain, std::move(edit), &why), "%s", why.c_str());
            CHECK(e.setWire(chain, Port::block(0), Port::output(1), {-6.0f, 0.5f, 0.25f}));
            e.collect();
            if (i % 8 == 7) {
                playing = 1 - playing;
                e.switchTo(playing);
                e.setWarm(1 - playing);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
        audio.join();
        e.collect();
        CHECK_MSG(destroyed() == static_cast<uint64_t>(made) - (e.chain(0).numSlots() - 1) - (e.chain(1).numSlots() - 1),
                  "%llu of %d freed while playing", (unsigned long long)destroyed(), made);
        for (const auto& c : counts)
            CHECK(c.processWhileStopped == 0 && c.process > 0);
        e.deactivate();
    }
    CHECK_MSG(destroyed() == static_cast<uint64_t>(made), "%llu of %d freed", (unsigned long long)destroyed(), made);
}
