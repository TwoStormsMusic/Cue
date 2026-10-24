// Tests of the CLAP host adapter against the test plug-ins in plugins/test.

#include "check.h"

#include "core/engine.h"
#include "host/clap_processor.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
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
    CHECK_MSG(list.size() == 4, "found %zu plug-ins (%s)", list.size(), err.c_str());
    if (list.size() == 4)
        CHECK(list[0].id == "cue.test.gain" && list[1].name == "Test Delay");
}

TEST(clap_gain_runs_in_a_scene) {
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
    CHECK(stat("resets") == resets + 1); // gain scene reset before its pre-roll; the delay was not
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
