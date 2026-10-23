// Tests of the VST3 host adapter against the test VST3 in plugins/test.

#include "check.h"

#include "core/engine.h"
#include "host/vst3_processor.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace sh;

namespace {

// The bundle directory and the shared object inside it; the packaged test
// run points these at the bundled plug-in.
const char* kTestVst3 = std::getenv("STAGEHOST_TEST_VST3") ? std::getenv("STAGEHOST_TEST_VST3") : STAGEHOST_TEST_VST3;
const char* kTestVst3So = std::getenv("STAGEHOST_TEST_VST3_SO") ? std::getenv("STAGEHOST_TEST_VST3_SO") : STAGEHOST_TEST_VST3_SO;
constexpr uint32_t kBlock = 64;

long vstat(const char* name) {
    void* h = dlopen(kTestVst3So, RTLD_NOW | RTLD_NOLOAD);
    auto fn = h ? reinterpret_cast<long (*)(const char*)>(dlsym(h, "stagehost_vst3_test_stat")) : nullptr;
    return fn ? fn(name) : -1;
}

std::unique_ptr<Vst3Processor> load() {
    std::string err;
    auto p = Vst3Processor::load(kTestVst3, "", &err);
    if (!p)
        std::printf("    load failed: %s\n", err.c_str());
    return p;
}

std::unique_ptr<Scene> sceneOf(std::unique_ptr<Processor> p) {
    std::vector<std::unique_ptr<Processor>> chain;
    chain.push_back(std::move(p));
    return std::make_unique<Scene>("vst3", std::move(chain), std::vector<OutputTap>{{1, OutputMix::Sum}, {1, OutputMix::Sum}});
}

std::unique_ptr<Scene> silentScene() {
    return std::make_unique<Scene>("silent", std::vector<std::unique_ptr<Processor>>{},
                                   std::vector<OutputTap>{{0, OutputMix::Sum}, {0, OutputMix::Sum}});
}

EngineSettings settings() {
    EngineSettings s;
    s.maxFrames = kBlock;
    return s;
}

void run(Engine& e, float value, size_t blocks, std::vector<float>* all = nullptr) {
    std::vector<float> in(kBlock, value), o1(kBlock), o2(kBlock);
    float* outs[2] = {o1.data(), o2.data()};
    for (size_t b = 0; b < blocks; ++b) {
        e.process(in.data(), outs, kBlock);
        if (all)
            all->insert(all->end(), o1.begin(), o1.end());
    }
}

} // namespace

TEST(vst3_module_lists_its_effect) {
    std::string err;
    auto list = scanVst3(kTestVst3, &err);
    CHECK_MSG(list.size() == 1, "found %zu (%s)", list.size(), err.c_str());
    if (!list.empty())
        CHECK(list[0].name == "Stagehost Test Delay");
}

TEST(vst3_delay_runs_rings_out_and_follows_lifecycle) {
    auto p = load();
    CHECK(p != nullptr);
    if (!p)
        return;
    Engine e(settings());
    e.addScene(sceneOf(std::move(p)));
    e.addScene(silentScene());
    CHECK(e.activate());
    CHECK(e.scene(0).slot(0).tailSamples() == 16 * 250 * 48);

    const long onBefore = vstat("processing_on");
    e.switchTo(0);
    std::vector<float> out;
    run(e, 0.25f, 400, &out); // DC in: dry 0.25 plus echoes at mix 0.5
    CHECK(vstat("processing_on") == onBefore + 1);
    CHECK(out.back() > 0.3f);

    e.switchTo(1);
    out.clear();
    run(e, 0.0f, 1500, &out);
    float echo = 0.0f;
    for (size_t n = 48000 / 4; n < 48000; ++n)
        echo = std::max(echo, std::fabs(out[n]));
    CHECK_MSG(echo > 0.05f, "echo after switch %.4f", echo);
    run(e, 0.0f, 3000);
    CHECK(e.state(0) == SceneState::Idle);
    CHECK(vstat("processing_off") >= 1);
    CHECK(vstat("process_while_off") == 0);
}

TEST(vst3_param_change_reaches_the_processor) {
    auto p = load();
    Vst3Processor* v = p.get();
    Engine e(settings());
    e.addScene(sceneOf(std::move(p)));
    CHECK(e.activate());
    e.switchTo(0);
    run(e, 0.0f, 100);
    v->setParam(0, 0.0); // mix 0: only the dry signal
    run(e, 0.25f, 600);
    std::vector<float> out;
    run(e, 0.25f, 10, &out);
    CHECK_MSG(std::fabs(out.back() - 0.25f) < 1e-5f, "output %.5f", out.back());
    v->pumpMainThread(); // controller follows
}

TEST(vst3_state_round_trips) {
    auto p = load();
    CHECK(p && p->activate(48000, kBlock));
    if (!p)
        return;
    std::vector<uint8_t> comp, ctrl;
    CHECK(p->saveState(comp, ctrl));
    CHECK(comp.size() == sizeof(double));
    double v = 0.125;
    std::vector<uint8_t> other(sizeof v);
    std::memcpy(other.data(), &v, sizeof v);
    CHECK(p->loadState(other, {}));
    CHECK(p->saveState(comp, ctrl) && std::memcmp(comp.data(), &v, sizeof v) == 0);
    p->deactivate();
}
