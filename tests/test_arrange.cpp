// Chains and scenes added, copied, moved, set and taken out while the
// engine plays: on a thread of its own, as the sound card would play it,
// with built-in gains so what comes out says which chain is heard.

#include "check.h"

#include "app/arrange.h"
#include "app/build.h"
#include "app/cuefile.h"
#include "app/edit.h"
#include "host/clap_processor.h"
#include "midi/eurekaprom.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cue;

namespace {

// Scene A plays the input at 0.25 (One) or 0.125 (Two), scene B as it is.
const char* kPerformance = R"(
[audio]
period = 64

[performance Test]
inputs = Bass
outputs = FOH, Amp

[control Effect]

[scene A]

[chain One]
plugin = builtin:gain 0.5
name = Level

[chain Two]
plugin = builtin:gain 0.25
name = Level

[scene B]

[chain Three]
plugin = builtin:gain 1
name = Level

[trigger Hold]
on = pedal 9
scene = B
do = Effect = on
)";

// The performance loaded and playing, as cue run has it.
struct Live {
    Config config;
    Rig rig;
    std::unique_ptr<RigOutput> out;
    std::unique_ptr<Show> show;
    std::unique_ptr<LiveRig> live;
    std::string err;

    Live() {
        CHECK_MSG(parseConfigText(kPerformance, "/cfg/t.ini", config, &err), "%s", err.c_str());
        config.workers = 0;
        CHECK_MSG(buildRig(config, rig, &err, 4), "%s", err.c_str());
        out = std::make_unique<RigOutput>(rig, nullptr);
        show = std::make_unique<Show>(*out);
        CHECK_MSG(show->load(config, rig.showPlugins(), &err), "%s", err.c_str());
        live = std::make_unique<LiveRig>(LiveRig{config, rig, *show, nullptr});
        show->start();
    }
    const SceneSpec& scene(int s) const { return config.scenes.at(s); }
    int playing() const { return show->presetChain(show->preset()); }
    // A chain's preset, by its id.
    int presetOf(int id) const {
        for (int i = 0; i < static_cast<int>(show->numPresets()); ++i)
            if (show->presetChain(i) == id)
                return i;
        return -1;
    }
};

// The engine played a block at a time on a thread of its own, fed a steady
// 0.5, with what the last block put out.
struct Player {
    Engine& engine;
    std::atomic<bool> running{true};
    std::atomic<float> level{0.0f};
    std::thread thread;

    explicit Player(Engine& e) : engine(e) {
        thread = std::thread([this] {
            markClapAudioThread();
            std::vector<float> in(64, 0.5f), foh(64), amp(64);
            const float* ins[1] = {in.data()};
            float* outs[2] = {foh.data(), amp.data()};
            while (running) {
                engine.process(ins, outs, 64);
                level.store(foh[63]);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
    }
    ~Player() {
        running = false;
        thread.join();
    }
    // Whether the output comes to want (within a hair) in time.
    bool hears(float want) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            if (std::abs(level.load() - want) < 1e-4f)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

MidiMessage press(int pedal) { return MidiMessage::cc(0, eurekaprom::kPressCC, static_cast<uint8_t>(pedal == 10 ? 0 : pedal)); }

} // namespace

TEST(arrange_adds_copies_moves_and_takes_out_chains_while_it_plays) {
    Live t;
    Engine& engine = *t.rig.engine;
    CHECK(engine.numChains() == 3 && engine.capacity() == 7);
    Player play(engine);
    CHECK(play.hears(0.25f));

    // A copy of Two before One: the first spare chain, named apart.
    NewChain copy;
    copy.scene = 0;
    copy.at = 0;
    copy.copy = 1;
    int id = -1;
    CHECK_MSG(addChain(*t.live, copy, &id, &t.err), "%s", t.err.c_str());
    CHECK(id == 3 && engine.inUse(3) && engine.numChains() == 4);
    CHECK(t.scene(0).chains.size() == 3 && t.scene(0).chains[0].name == "Two 2" && t.scene(0).chains[0].id == 3);
    CHECK(t.rig.find(3, 0) >= 0 && t.rig.chainNames[3] == "Two 2");
    // One still plays, though it is the second chain of its scene now.
    CHECK(t.playing() == 0 && t.show->preset() == 1);
    CHECK(play.hears(0.25f));
    t.show->selectPreset(t.presetOf(3));
    CHECK(play.hears(0.125f));

    // Not while it plays; then it goes, and its engine chain is spare.
    RemovedChain removed;
    CHECK(!removeChain(*t.live, 3, &removed, &t.err) && t.err.find("playing") != std::string::npos);
    t.show->selectPreset(t.presetOf(0));
    CHECK(play.hears(0.25f));
    CHECK_MSG(removeChain(*t.live, 3, &removed, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).chains.size() == 2 && !engine.inUse(3) && engine.numChains() == 3);
    CHECK(t.rig.find(3, 0) < 0 && t.rig.chainNames[3].empty() && t.presetOf(3) < 0);
    // Back where it was, under the same number.
    CHECK_MSG(restoreChain(*t.live, removed, &id, &t.err), "%s", t.err.c_str());
    CHECK(id == 3 && t.scene(0).chains[0].name == "Two 2" && t.rig.find(3, 0) >= 0);

    // An empty chain plays its input as it is.
    NewChain empty;
    empty.scene = 1;
    CHECK_MSG(addChain(*t.live, empty, &id, &t.err), "%s", t.err.c_str());
    CHECK(id == 4 && t.scene(1).chains.size() == 2 && t.scene(1).chains[1].name == "Chain 2" && t.scene(1).chains[1].wires.size() == 2);
    t.show->selectPreset(t.presetOf(4));
    CHECK(play.hears(0.5f) && t.show->scene() == 1);
    // Moved into scene A, first: it plays on, and the show's scene follows it.
    CHECK_MSG(moveChain(*t.live, 4, 0, 0, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).chains[0].id == 4 && t.scene(1).chains.size() == 1);
    CHECK(t.playing() == 4 && t.show->preset() == 0 && t.show->scene() == 0);
    CHECK(play.hears(0.5f));
    // ...and within it, last.
    CHECK(moveChain(*t.live, 4, 0, -1, &t.err) && t.scene(0).chains.back().id == 4 && t.playing() == 4);

    // A scene keeps a chain; names are the scene's own.
    CHECK(!removeChain(*t.live, 2, nullptr, &t.err) && t.err.find("at least one chain") != std::string::npos);
    CHECK(!moveChain(*t.live, 2, 0, 0, &t.err));
    NewChain twin;
    twin.scene = 0;
    twin.name = "one";
    CHECK(!addChain(*t.live, twin, &id, &t.err) && t.err.find("already has") != std::string::npos);
    twin.name = "A/B";
    CHECK(!addChain(*t.live, twin, &id, &t.err));
    twin.scene = 1;
    twin.name = "One";
    CHECK_MSG(addChain(*t.live, twin, &id, &t.err), "%s", t.err.c_str());

    // Every spare taken: refused, with nothing changed.
    CHECK(id == 5 && addChain(*t.live, empty, &id, &t.err) && id == 6);
    const size_t chains = t.scene(1).chains.size();
    CHECK(!addChain(*t.live, empty, &id, &t.err) && t.err.find("no more chains") != std::string::npos);
    CHECK(t.scene(1).chains.size() == chains);
    CHECK(play.hears(0.5f));
}

TEST(arrange_sets_a_chains_pedal_display_actions_and_warm_chain) {
    Live t;
    Engine& engine = *t.rig.engine;
    Player play(engine);
    CHECK(play.hears(0.25f));

    // A pedal moves from the chain that had it.
    ChainSettings pedal;
    pedal.pedal = 7;
    CHECK_MSG(setChain(*t.live, 1, pedal, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).presets[1].pedal == 7);
    CHECK(setChain(*t.live, 0, pedal, &t.err) && t.scene(0).presets[0].pedal == 7 && t.scene(0).presets[1].pedal == 0);
    pedal.pedal = 4;
    CHECK(setChain(*t.live, 1, pedal, &t.err));
    t.show->onMidi(press(4));
    CHECK(t.playing() == 1 && play.hears(0.125f));
    t.show->onMidi(press(7));
    CHECK(t.playing() == 0 && play.hears(0.25f));
    pedal.pedal = 11;
    CHECK(!setChain(*t.live, 0, pedal, &t.err));

    // The display: two letters, as the FCB shows them (three with a leading 1, - or +).
    ChainSettings display;
    display.display = "ON";
    CHECK(setChain(*t.live, 0, display, &t.err) && t.scene(0).presets[0].display == "ON");
    display.display = "-12";
    CHECK(setChain(*t.live, 0, display, &t.err) && t.scene(0).presets[0].display == "-12");
    display.display = "ONE";
    CHECK(!setChain(*t.live, 0, display, &t.err) && t.scene(0).presets[0].display == "-12");
    display.display = "ON";
    CHECK(setChain(*t.live, 0, display, &t.err));

    // What selecting it does; one the show cannot follow is refused.
    ChainSettings actions;
    actions.onSelect = std::vector<std::string>{"Effect = on", " "};
    CHECK_MSG(setChain(*t.live, 1, actions, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).presets[1].actions.size() == 1);
    t.show->selectPreset(t.presetOf(1));
    CHECK(t.show->value("Effect") == 1.0);
    actions.onSelect = std::vector<std::string>{"Nothing = on"};
    CHECK(!setChain(*t.live, 1, actions, &t.err) && t.err.find("Nothing") != std::string::npos);
    CHECK(t.scene(0).presets[1].actions.size() == 1 && t.scene(0).presets[1].actions[0].text == "Effect = on");

    // Kept warm while it plays: the next chain, none, or one named.
    t.show->selectPreset(t.presetOf(0));
    auto warm = [&](int chain) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            if (static_cast<ChainState>(engine.stats(chain).state.load()) == ChainState::Warm)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    CHECK(warm(1));
    NewChain empty;
    empty.scene = 0;
    int id = -1;
    CHECK(addChain(*t.live, empty, &id, &t.err) && id == 3);
    ChainSettings keep;
    keep.warm = "chain 3";
    CHECK_MSG(setChain(*t.live, 0, keep, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).presets[0].warm == "Chain 3" && warm(3));
    keep.warm = "Three";
    CHECK(!setChain(*t.live, 0, keep, &t.err));
    // Renamed: what keeps it warm follows the name.
    ChainSettings name;
    name.name = "Empty";
    CHECK_MSG(setChain(*t.live, 3, name, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).presets[0].warm == "Empty" && t.rig.chainNames[3] == "Empty" && t.scene(0).presets[2].name == "Empty");
    name.name = "One";
    CHECK(!setChain(*t.live, 3, name, &t.err));
    // Taken out, the next chain is kept warm instead; put back, it is again.
    RemovedChain removed;
    CHECK_MSG(removeChain(*t.live, 3, &removed, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(0).presets[0].warm == "next" && warm(1));
    CHECK(restoreChain(*t.live, removed, &id, &t.err) && t.scene(0).presets[0].warm == "Empty" && warm(3));
    keep.warm = "none";
    CHECK(setChain(*t.live, 0, keep, &t.err) && t.scene(0).presets[0].warm == "none");
    CHECK(play.hears(0.25f));
}

TEST(arrange_adds_copies_and_takes_out_scenes) {
    Live t;
    Engine& engine = *t.rig.engine;
    Player play(engine);
    CHECK(play.hears(0.25f));

    // A copy of B: its chains, and the trigger that only works in it.
    NewScene copy;
    copy.copy = 1;
    int index = -1;
    std::vector<int> ids;
    CHECK_MSG(addScene(*t.live, copy, &index, &ids, &t.err), "%s", t.err.c_str());
    CHECK(index == 2 && t.scene(2).name == "B 2" && ids.size() == 1 && ids[0] == 3 && t.scene(2).chains[0].name == "Three");
    CHECK(t.config.triggers.size() == 2 && t.config.triggers[1].scene == "B 2");
    t.show->selectPreset(t.presetOf(3));
    CHECK(play.hears(0.5f) && t.show->scene() == 2);
    t.show->onMidi(press(9));
    CHECK(t.show->value("Effect") == 1.0);

    // Not while it plays; then it goes, chains, trigger and all.
    RemovedScene removed;
    CHECK(!removeScene(*t.live, 2, &removed, &t.err) && t.err.find("playing") != std::string::npos);
    t.show->selectPreset(t.presetOf(0));
    CHECK(play.hears(0.25f));
    CHECK_MSG(removeScene(*t.live, 2, &removed, &t.err), "%s", t.err.c_str());
    CHECK(t.config.scenes.size() == 2 && t.config.triggers.size() == 1 && !engine.inUse(3));
    CHECK_MSG(restoreScene(*t.live, removed, &t.err), "%s", t.err.c_str());
    CHECK(t.config.scenes.size() == 3 && t.scene(2).chains[0].id == 3 && t.config.triggers.size() == 2 && engine.inUse(3));

    // A new one has one empty chain.
    NewScene fresh;
    fresh.at = 0;
    CHECK_MSG(addScene(*t.live, fresh, &index, &ids, &t.err), "%s", t.err.c_str());
    CHECK(index == 0 && t.scene(0).name == "Scene 4" && t.scene(0).chains.size() == 1 && t.scene(0).chains[0].name == "Chain 1");
    // The scene playing is the one with the chain playing, wherever it is.
    CHECK(t.show->scene() == 1 && t.playing() == 0);

    // Renamed, its triggers follow; a pedal moves from the scene that had it.
    SceneSettings set;
    set.name = "Bee";
    CHECK_MSG(setScene(*t.live, 2, set, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(2).name == "Bee" && t.config.triggers[0].scene == "Bee");
    set = {};
    set.pedal = 2;
    CHECK(setScene(*t.live, 2, set, &t.err) && setScene(*t.live, 3, set, &t.err) && t.scene(2).pedal == 0 && t.scene(3).pedal == 2);
    set = {};
    set.name = "a";
    CHECK(!setScene(*t.live, 2, set, &t.err));

    // Moved: the show keeps playing the same chain.
    CHECK_MSG(moveScene(*t.live, 1, 3, &t.err), "%s", t.err.c_str());
    CHECK(t.scene(3).name == "A" && t.show->scene() == 3 && t.playing() == 0);
    CHECK(!removeScene(*t.live, 3, nullptr, &t.err));
    CHECK(play.hears(0.25f));
}

TEST(arrange_renames_and_places_the_inputs_and_outputs) {
    Live t;
    ChainSpec& one = t.config.scenes.at(0).chains.at(0);
    // A name of the performance's own: its chains, wires and tuner follow.
    CHECK_MSG(renamePort(*t.live, false, 0, " Front ", &t.err), "%s", t.err.c_str());
    CHECK(t.config.outputs == (std::vector<std::string>{"Front", "Amp"}));
    CHECK_MSG(renamePort(*t.live, true, 0, "Bass DI", &t.err), "%s", t.err.c_str());
    CHECK(t.config.inputs.at(0) == "Bass DI" && one.input == "Bass DI" && t.config.tuner.input == "Bass DI");
    // Not empty, not another's, not how requests name another.
    CHECK(!renamePort(*t.live, false, 0, "  ", &t.err));
    CHECK(!renamePort(*t.live, false, 0, "amp", &t.err));
    CHECK(!renamePort(*t.live, false, 0, "out2", &t.err));
    CHECK(!renamePort(*t.live, true, 0, "in", &t.err));
    CHECK(!renamePort(*t.live, false, 2, "Monitor", &t.err));
    CHECK(renamePort(*t.live, false, 0, "out1", &t.err) && renamePort(*t.live, false, 0, "Front", &t.err));
    CHECK(t.config.outputs.at(0) == "Front");

    // Where the board draws the input and an output.
    CHECK_MSG(placePort(*t.live, one.id, Port::input(), -20, 40.25, &t.err), "%s", t.err.c_str());
    CHECK_MSG(placePort(*t.live, one.id, Port::output(1), 300, -40, &t.err), "%s", t.err.c_str());
    CHECK(!placePort(*t.live, one.id, Port::output(2), 0, 0, &t.err));
    CHECK(one.inputAt.placed && one.inputAt.x == -20 && one.outputsAt.size() == 2 && !one.outputsAt[0].placed && one.outputsAt[1].y == -40);

    // Saved and read back, with the new names; on a rig that knows only
    // the old ones, what it does not know takes a channel none of the rest has.
    char name[] = "/tmp/cue-arrange-XXXXXX";
    const std::string dir = mkdtemp(name) ? name : "/tmp";
    CHECK_MSG(saveCue(t.config, dir + "/renamed.cue", &t.err), "%s", t.err.c_str());
    std::ofstream(dir + "/rig.json") << R"({"cue_rig": 1, "inputs": {"Bass": 1}, "outputs": {"FOH": 2, "Amp": 1}})";
    Config c;
    CHECK_MSG(loadPerformance(dir + "/renamed.cue", dir + "/rig.json", c, &t.err), "%s", t.err.c_str());
    CHECK(c.inputs == std::vector<std::string>{"Bass DI"} && c.outputs == (std::vector<std::string>{"Front", "Amp"}));
    CHECK(c.tuner.input == "Bass DI");
    CHECK(c.audio.outputChannels == (std::vector<unsigned>{1, 0}));
    const ChainSpec& back = c.scenes.at(0).chains.at(0);
    CHECK(back.inputAt.placed && back.inputAt.x == -20 && back.inputAt.y == 40.3);
    CHECK(back.outputsAt.size() == 2 && !back.outputsAt[0].placed && back.outputsAt[1].placed && back.outputsAt[1].x == 300);
    CHECK_MSG(wireList(back, c.outputs) == "in>0 0>Front 0>Amp", "%s", wireList(back, c.outputs).c_str());
}

TEST(outputs_split_a_plug_ins_signal_each_at_a_level_of_its_own) {
    Live t;
    Engine& engine = *t.rig.engine;
    // An edit puts in a new config: the chain as it is now, each time.
    auto one = [&]() -> const ChainSpec& { return t.config.scenes.at(0).chains.at(0); };
    const int id = one().id;
    Player play(engine);
    CHECK(play.hears(0.25f));
    const float half = 20.0f * std::log10(0.5f);

    // Its one output, at half: both wires leaving it follow.
    WireGains g;
    g.levelDb = half;
    CHECK_MSG(setOutput(*t.live, id, Port::block(0), g, &t.err), "%s", t.err.c_str());
    CHECK(play.hears(0.125f));
    CHECK(one().blocks[0].outs.size() == 1 && one().blocks[0].outs[0].levelDb == half);

    // A second output, as it was, wired to FOH too: FOH hears both.
    CHECK_MSG(setOutputs(*t.live, id, Port::block(0), {g, WireGains{}}, &t.err), "%s", t.err.c_str());
    CHECK(play.hears(0.125f));
    CHECK(!setWire(*t.live, id, {Port::blockOut(0, 2), Port::output(0), ""}, &t.err));
    CHECK_MSG(setWire(*t.live, id, {Port::blockOut(0, 1), Port::output(0), ""}, &t.err), "%s", t.err.c_str());
    CHECK(play.hears(0.375f));
    CHECK_MSG(wireList(one(), t.config.outputs) == "in>0 0>FOH(-6.0206dB w100 p0) 0>Amp(-6.0206dB w100 p0) 0@1>FOH",
              "%s", wireList(one(), t.config.outputs).c_str());

    // The first goes, with its wires; the second is the first now.
    CHECK_MSG(removeOutput(*t.live, id, Port::block(0), &t.err), "%s", t.err.c_str());
    CHECK(play.hears(0.25f));
    CHECK_MSG(wireList(one(), t.config.outputs) == "in>0 0>FOH", "%s", wireList(one(), t.config.outputs).c_str());
    CHECK(!removeOutput(*t.live, id, Port::block(0), &t.err) && t.err.find("one output") != std::string::npos);

    // Fewer outputs: the wires of those it no longer has go.
    CHECK(setOutputs(*t.live, id, Port::block(0), {WireGains{}, WireGains{}}, &t.err));
    CHECK(setWire(*t.live, id, {Port::blockOut(0, 1), Port::output(1), ""}, &t.err));
    CHECK(setOutputs(*t.live, id, Port::block(0), {WireGains{}}, &t.err));
    CHECK_MSG(wireList(one(), t.config.outputs) == "in>0 0>FOH", "%s", wireList(one(), t.config.outputs).c_str());
    CHECK(!setOutputs(*t.live, id, Port::block(0), {}, &t.err));
    CHECK(!setOutputs(*t.live, id, Port::block(0), std::vector<WireGains>(kMaxOuts + 1), &t.err));

    // The chain's input has outputs too: muted, nothing comes through.
    WireGains off;
    off.muted = true;
    CHECK_MSG(setOutput(*t.live, id, Port::input(), off, &t.err), "%s", t.err.c_str());
    CHECK(play.hears(0.0f));
    CHECK(one().inputOuts.size() == 1 && one().inputOuts[0].muted);
    CHECK(setOutput(*t.live, id, Port::input(), WireGains{}, &t.err));
    CHECK(play.hears(0.25f));
    CHECK(!setOutput(*t.live, id, Port::output(0), WireGains{}, &t.err));
}
