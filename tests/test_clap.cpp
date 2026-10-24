// Tests of the CLAP host adapter against the test plug-ins in plugins/test.

#include "check.h"

#include "core/engine.h"
#include "host/clap_processor.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace cue;

namespace {

// The packaged test run points these at the bundled plug-ins.
const char* kTestClap = std::getenv("CUE_TEST_CLAP") ? std::getenv("CUE_TEST_CLAP") : CUE_TEST_CLAP;
constexpr uint32_t kBlock = 64;

long stat(const char* name) {
    static auto fn = [] {
        void* h = dlopen(kTestClap, RTLD_NOW | RTLD_NOLOAD);
        return h ? reinterpret_cast<long (*)(const char*)>(dlsym(h, "cue_test_stat")) : nullptr;
    }();
    return fn ? fn(name) : -1;
}

std::unique_ptr<ClapProcessor> load(const char* id) {
    std::string err;
    auto p = ClapProcessor::load(kTestClap, id, &err);
    if (!p)
        std::printf("    load %s failed: %s\n", id, err.c_str());
    return p;
}

std::unique_ptr<Chain> chainOf(std::vector<std::unique_ptr<Processor>> chain) {
    const size_t n = chain.size();
    return std::make_unique<Chain>("clap", std::move(chain), std::vector<OutputTap>{{0, n, OutputMix::Sum}, {0, n, OutputMix::Sum}});
}

EngineSettings settings() {
    EngineSettings s;
    s.maxFrames = kBlock;
    s.numOutputs = 2;
    return s;
}

// Runs blocks of a constant input and returns the last output sample.
float runConstant(Engine& e, float value, size_t blocks, std::vector<float>* all = nullptr) {
    std::vector<float> in(kBlock, value), o1(kBlock), o2(kBlock);
    float* outs[2] = {o1.data(), o2.data()};
    for (size_t b = 0; b < blocks; ++b) {
        e.process(in.data(), outs, kBlock);
        if (all)
            all->insert(all->end(), o1.begin(), o1.end());
    }
    return o1.back();
}

} // namespace

TEST(clap_file_lists_its_plugins) {
    std::string err;
    auto list = scanClapFile(kTestClap, &err);
    CHECK_MSG(list.size() == 6, "found %zu plug-ins (%s)", list.size(), err.c_str());
    if (list.size() == 6)
        CHECK(list[0].id == "cue.test.gain" && list[1].name == "Test Delay" && list[5].id == "cue.test.key");
}

TEST(clap_gain_runs_in_a_chain) {
    markClapAudioThread();
    Engine e(settings());
    std::vector<std::unique_ptr<Processor>> chain;
    chain.push_back(load("cue.test.gain"));
    CHECK(chain.back() != nullptr);
    e.addChain(chainOf(std::move(chain)));
    CHECK(e.activate());
    e.switchTo(0);
    CHECK(std::fabs(runConstant(e, 0.5f, 200) - 0.5f) < 1e-6f);
}

TEST(clap_param_change_reaches_the_plugin) {
    Engine e(settings());
    auto gain = load("cue.test.gain");
    ClapProcessor* g = gain.get();
    std::vector<std::unique_ptr<Processor>> chain;
    chain.push_back(std::move(gain));
    e.addChain(chainOf(std::move(chain)));
    CHECK(e.activate());
    e.switchTo(0);
    runConstant(e, 0.5f, 100);
    CHECK(g->setParam(0, 0.5));
    CHECK(std::fabs(runConstant(e, 0.5f, 2) - 0.25f) < 1e-6f);
}

TEST(clap_idle_plugin_gets_params_without_processing) {
    Engine e(settings());
    auto gainA = load("cue.test.gain");
    auto gainB = load("cue.test.gain");
    ClapProcessor* b = gainB.get();
    std::vector<std::unique_ptr<Processor>> ca, cb;
    ca.push_back(std::move(gainA));
    cb.push_back(std::move(gainB));
    e.addChain(chainOf(std::move(ca)));
    e.addChain(chainOf(std::move(cb)));
    CHECK(e.activate());
    e.switchTo(0);
    runConstant(e, 0.5f, 100);
    const long stoppedBefore = stat("process_while_stopped");
    b->setParam(0, 2.0);
    runConstant(e, 0.5f, 20); // scene B idle: flushed, not processed
    e.switchTo(1);
    CHECK(std::fabs(runConstant(e, 0.5f, 300) - 1.0f) < 1e-5f);
    CHECK(stat("process_while_stopped") == stoppedBefore);
}

TEST(clap_delay_reports_tail_and_rings_out) {
    Engine e(settings());
    std::vector<std::unique_ptr<Processor>> ca, cb;
    ca.push_back(load("cue.test.delay"));
    cb.push_back(load("cue.test.gain"));
    const uint32_t tail = ca.back()->tailSamples();
    e.addChain(chainOf(std::move(ca)));
    e.addChain(chainOf(std::move(cb)));
    CHECK(e.activate());
    CHECK_MSG(e.chain(0).slot(0).tailSamples() > 48000, "tail %u", e.chain(0).slot(0).tailSamples());
    (void)tail;
    e.switchTo(0);
    runConstant(e, 0.3f, 400);
    const long resets = stat("resets");
    e.switchTo(1);
    std::vector<float> out;
    runConstant(e, 0.0f, 1500, &out); // silence in: only the delay's echoes remain
    float echo = 0.0f;
    for (size_t n = 48000 / 2; n < 48000; ++n)
        echo = std::max(echo, std::fabs(out[n]));
    CHECK_MSG(echo > 0.05f, "echo level %.4f", echo);
    CHECK(stat("resets") == resets); // the gain chain never played, so it needs no reset; the delay rings on
}

TEST(clap_lifecycle_follows_the_rules) {
    // Any process() call outside start/stop processing is a host bug.
    CHECK(stat("process_while_stopped") == 0);
    CHECK(stat("starts") >= stat("stops"));
}

TEST(clap_state_round_trips) {
    auto g = load("cue.test.gain");
    CHECK(g != nullptr);
    if (!g)
        return;
    CHECK(g->activate(48000, kBlock));
    std::vector<uint8_t> state;
    CHECK(g->saveState(state));
    CHECK(state.size() == sizeof(double));
    double v = 1.75;
    std::vector<uint8_t> other(sizeof v);
    std::memcpy(other.data(), &v, sizeof v);
    CHECK(g->loadState(other));
    CHECK(g->saveState(state) && std::memcmp(state.data(), &v, sizeof v) == 0);
    g->deactivate();
}

TEST(clap_midi_reaches_a_plugin_with_a_midi_input) {
    markClapAudioThread();
    auto pedal = load("cue.test.pedal");
    auto gain = load("cue.test.gain");
    CHECK(pedal && gain);
    if (!pedal || !gain)
        return;
    CHECK(pedal->acceptsMidi());
    CHECK(!gain->acceptsMidi());
    ClapProcessor* pp = pedal.get();
    Engine e(settings());
    std::vector<std::unique_ptr<Processor>> a, b;
    a.push_back(std::move(pedal));
    b.push_back(std::move(gain));
    e.addChain(chainOf(std::move(a)));
    e.addChain(chainOf(std::move(b)));
    CHECK(e.activate());
    e.switchTo(0);
    CHECK(std::fabs(runConstant(e, 1.0f, 200) - 1.0f) < 1e-4f);
    pp->sendMidi(0xB0, 11, 64); // the pedal halfway
    const float half = runConstant(e, 1.0f, 2);
    CHECK_MSG(std::fabs(half - 64.0f / 127.0f) < 1e-4f, "output %.4f after CC 11 = 64", half);
    CHECK(stat("last_cc") == 11 && stat("last_cc_value") == 64);
}

TEST(clap_idle_plugin_gets_the_latest_pedal_position_when_its_chain_starts) {
    markClapAudioThread();
    auto pedal = load("cue.test.pedal");
    auto gain = load("cue.test.gain");
    if (!pedal || !gain)
        return;
    ClapProcessor* pp = pedal.get();
    Engine e(settings());
    std::vector<std::unique_ptr<Processor>> a, b;
    a.push_back(std::move(gain));
    b.push_back(std::move(pedal));
    e.addChain(chainOf(std::move(a)));
    e.addChain(chainOf(std::move(b)));
    CHECK(e.activate());
    e.switchTo(0);
    runConstant(e, 1.0f, 50);
    // The pedal moves 100 times while its chain is idle.
    const long before = stat("midi_events");
    for (int v = 0; v < 100; ++v)
        pp->sendMidi(0xB0, 11, static_cast<uint8_t>(v));
    pp->sendMidi(0xB0, 11, 32);
    runConstant(e, 1.0f, 50);
    CHECK(stat("midi_events") == before); // not processed, nothing delivered yet
    e.switchTo(1);
    const float out = runConstant(e, 1.0f, 400);
    CHECK_MSG(std::fabs(out - 32.0f / 127.0f) < 1e-4f, "output %.4f, expected the last position 32/127", out);
    CHECK_MSG(stat("midi_events") - before == 1, "%ld events delivered, expected only the latest", stat("midi_events") - before);
}

TEST(clap_sidechain_feeds_the_second_input_port) {
    markClapAudioThread();
    auto a = load("cue.test.gain");
    auto b = load("cue.test.gain");
    auto key = load("cue.test.key");
    if (!a || !b || !key)
        return;
    CHECK(key->hasSidechain() && !a->hasSidechain());
    a->setParam(0, 0.5);
    b->setParam(0, 0.5);
    std::vector<std::unique_ptr<Processor>> slots;
    slots.push_back(std::move(a));
    slots.push_back(std::move(b));
    slots.push_back(std::move(key));
    auto chain = chainOf(std::move(slots));
    CHECK(chain->setSidechain(2, 0));
    Engine e(settings());
    e.addChain(std::move(chain));
    CHECK(e.activate());
    e.switchTo(0);
    // The key plug-in plays its side-chain: the first gain's 0.5, not the 0.25 on its main input.
    const float out = runConstant(e, 1.0f, 400);
    CHECK_MSG(std::fabs(out - 0.5f) < 1e-5f, "keyed output %.5f, expected 0.5", out);
}

TEST(clap_timers_run_from_the_main_thread_pump) {
    auto key = load("cue.test.key");
    if (!key)
        return;
    CHECK(key->activate(48000.0, kBlock));
    const long before = stat("timer_calls");
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < end) {
        key->pumpMainThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const long calls = stat("timer_calls") - before;
    CHECK_MSG(calls >= 4 && calls <= 9, "%ld timer calls in 150 ms of a 20 ms timer", calls);
    key->deactivate();
}
