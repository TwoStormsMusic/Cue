// .cue performance files and rig profiles.

#include "check.h"

#include "app/config.h"
#include "app/cuefile.h"
#include "miniz.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace cue;

namespace {

std::string tempDir() {
    char name[] = "/tmp/cue-cuefile-XXXXXX";
    return mkdtemp(name) ? name : "/tmp";
}

void writeFile(const std::string& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }

std::string bytes(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

// A performance with every kind of thing a .cue holds.
std::string kPerformance(const std::string& dir) {
    return R"(
[audio]
device = hw:Test
period = 128
periods = 3
input = 2
out1 = 4
out2 = 3
cpu = none
workers = 1
mix = left

[switching]
preroll_ms = 40
crossfade_ms = 15
bypass_mode = mute

[midi]
in = FCB
channel = 3

[performance Friday]
inputs = Bass
outputs = FOH, Amp

[scene Covers]
display = CV
pedal = 2

[chain Punk]
plugin = clap:)" + dir + R"(/Gain Stage.clap#com.twostorms.gainstage
name = Gain Stage
param = 54:1
param = 3:-6.5
state = )" + dir + R"(/gain.state
plugin = vst3:)" + dir + R"(/Amp.vst3
state = )" + dir + R"(/amp.vst3state
controller_state = )" + dir + R"(/amp.vst3ctrl
bypass_mode = thru
branch = FOH
tap = out2
plugin = builtin:delay 300 0.4 0.3
name = Echo
bypass = yes
midi = no
plugin = builtin:gain 1
name = Gate
sidechain = Gain Stage

[branch FOH]
plugin = builtin:lowpass 4000
name = IR
tap = out1

[chain Slap]
plugin = builtin:gain 0.5

[preset Punk]
pedal = 6
display = PU
program = 4
do = group Start
do = Wah = Off
warm = Slap

[preset Slap]
pedal = 7
warm = none

[scene Originals]
[chain Djent]
plugin = builtin:drive 3

[control Wah]
values = Off, Auto, Pedal
default = Auto
pedal = 4
enable = Echo when Auto
[control Effect 1]
pedal = 1
[control Level]
type = continuous
range = -12..6
step = 1.5
default = 0
follows = expression 2
drive = Gate 0 = value
[control Tuner]
pedal = 5
mute = on

[group Start]
do = Effect 1 = on

[trigger Next song]
scene = Covers
when = Tuner is off
on = cc 102 = 14
do = scene next
do = midi B0 6D 0F

[fcb]
led value 1 = Wah is not Off
led 3 = Tuner
display = TU when Tuner
)";
}

void makeStates(const std::string& dir) {
    writeFile(dir + "/gain.state", "TWOSTORMS gainstage 1\np 54 1\n");
    writeFile(dir + "/amp.vst3state", std::string("\x00\x01\x02 amp", 7));
    writeFile(dir + "/amp.vst3ctrl", "ctrl");
}

} // namespace

TEST(cue_file_round_trips_a_whole_performance) {
    const std::string dir = tempDir();
    makeStates(dir);
    Config ini;
    std::string err;
    CHECK_MSG(parseConfigText(kPerformance(dir), dir + "/perf.ini", ini, &err), "%s", err.c_str());
    const std::string path = dir + "/perf.cue";
    std::vector<std::string> warnings;
    // The parameters the app picked for the amp, in its order.
    ini.scenes[0].chains[0].lines[0].plugins[1].shown = {7, 2, 40};
    ini.scenes[0].chains[0].lines[0].plugins[1].shownSet = true;
    CHECK_MSG(saveCue(ini, path, &err, &warnings), "%s", err.c_str());
    CHECK(warnings.empty());
    // The state files are inside it: removing them changes nothing.
    std::remove((dir + "/gain.state").c_str());
    std::remove((dir + "/amp.vst3state").c_str());

    Config c;
    CHECK_MSG(loadCue(path, c, &err), "%s", err.c_str());
    CHECK(c.performance == "Friday");
    CHECK(c.inputs == std::vector<std::string>{"Bass"});
    CHECK((c.outputs == std::vector<std::string>{"FOH", "Amp"}));
    CHECK(c.mix == OutputMix::Left);
    CHECK(c.engine.prerollMs == 40.0f && c.engine.crossfadeMs == 15.0f);
    CHECK(c.midi.channel == 2 && c.midi.profile == "eurekaprom");
    // Machine settings are not in the file.
    CHECK(c.audio.device != "hw:Test" && c.midi.in == "any");

    CHECK(c.scenes.size() == 2);
    const SceneSpec& s = c.scenes[0];
    CHECK(s.name == "Covers" && s.display == "CV" && s.pedal == 2);
    CHECK(s.chains.size() == 2 && s.presets.size() == 2);
    const ChainSpec& punk = s.chains[0];
    CHECK(punk.name == "Punk" && punk.lines.size() == 2);
    CHECK(punk.lines[1].name == "FOH" && punk.lines[1].parent == 0 && punk.lines[1].after == 2);
    CHECK(punk.out[0].line == 1 && punk.out[0].after == 1);
    CHECK(punk.out[1].line == 0 && punk.out[1].after == 2);
    const PluginSpec& gain = punk.lines[0].plugins[0];
    CHECK(gain.kind == "clap" && gain.id == "com.twostorms.gainstage" && gain.path == dir + "/Gain Stage.clap");
    CHECK(gain.label == "Gain Stage" && gain.params.size() == 2 && gain.params[1].first == 3 && gain.params[1].second == -6.5);
    CHECK(gain.embeddedState && bytes(gain.state) == "TWOSTORMS gainstage 1\np 54 1\n");
    CHECK(gain.muteOnBypass); // the [switching] default, written out
    const PluginSpec& amp = punk.lines[0].plugins[1];
    CHECK(amp.kind == "vst3" && amp.state.size() == 7 && bytes(amp.controllerState) == "ctrl" && !amp.muteOnBypass);
    CHECK(amp.shownSet && amp.shown == (std::vector<uint32_t>{7, 2, 40}));
    CHECK(!gain.shownSet && gain.shown.empty());
    const PluginSpec& echo = punk.lines[0].plugins[2];
    CHECK(echo.kind == "builtin" && echo.args == (std::vector<std::string>{"delay", "300", "0.4", "0.3"}));
    CHECK(echo.bypassed && !echo.midi && echo.label == "Echo");
    CHECK(punk.lines[0].plugins[3].sidechain == "Gain Stage");

    const PresetSpec& pp = s.presets[0];
    CHECK(pp.name == "Punk" && pp.chain == "Punk" && pp.pedal == 6 && pp.display == "PU" && pp.program == 4);
    CHECK(pp.actions.size() == 2 && pp.actions[0].text == "group Start" && pp.warm == "Slap");
    CHECK(s.presets[1].warm == "none" && s.presets[1].pedal == 7);
    CHECK(c.scenes[1].presets[0].warm == "next");

    CHECK(c.controls.size() == 4);
    const ControlSpec& wah = c.controls[0];
    CHECK(wah.type == ControlSpec::Type::Discrete && wah.labels.size() == 3 && wah.def == 1 && wah.pedal == 4);
    CHECK(wah.enables.size() == 1 && wah.enables[0].text == "Echo when Auto");
    CHECK(c.controls[1].type == ControlSpec::Type::Toggle && c.controls[1].labels.size() == 2);
    const ControlSpec& level = c.controls[2];
    CHECK(level.type == ControlSpec::Type::Continuous && level.min == -12 && level.max == 6 && level.step == 1.5);
    CHECK(level.follows.text == "expression 2" && level.drives.size() == 1);
    CHECK(c.controls[3].mutes.size() == 1);
    CHECK(c.groups.size() == 1 && c.groups[0].actions[0].text == "Effect 1 = on");
    CHECK(c.triggers.size() == 1 && c.triggers[0].scene == "Covers" && c.triggers[0].when.text == "Tuner is off");
    CHECK(c.triggers[0].inputs[0].text == "cc 102 = 14" && c.triggers[0].actions[1].text == "midi B0 6D 0F");
    CHECK(c.fcb.leds.size() == 2 && c.fcb.leds[0].first == 18 && c.fcb.leds[1].first == 3);
    CHECK(c.fcb.displays.size() == 1 && c.fcb.displays[0].text == "TU when Tuner");

    // Saved again, it is the same file.
    const std::string again = dir + "/again.cue";
    CHECK_MSG(saveCue(c, again, &err), "%s", err.c_str());
    Config c2;
    CHECK_MSG(loadCue(again, c2, &err), "%s", err.c_str());
    mz_zip_archive a{}, b{};
    size_t na = 0, nb = 0;
    CHECK(mz_zip_reader_init_file(&a, path.c_str(), 0) && mz_zip_reader_init_file(&b, again.c_str(), 0));
    char* ja = static_cast<char*>(mz_zip_reader_extract_file_to_heap(&a, "performance.json", &na, 0));
    char* jb = static_cast<char*>(mz_zip_reader_extract_file_to_heap(&b, "performance.json", &nb, 0));
    CHECK(ja && jb && std::string(ja, na) == std::string(jb, nb));
    CHECK(mz_zip_reader_get_num_files(&a) == mz_zip_reader_get_num_files(&b));
    CHECK(mz_zip_reader_get_num_files(&a) == 4); // performance.json and three states
    mz_free(ja);
    mz_free(jb);
    mz_zip_reader_end(&a);
    mz_zip_reader_end(&b);
}

TEST(cue_file_gives_each_chain_its_own_preset) {
    Config c;
    std::string err;
    CHECK_MSG(parseConfigText(R"(
[scene S]
[chain A]
plugin = builtin:gain 1
[chain B]
plugin = builtin:gain 1
[chain Unused]
plugin = builtin:gain 1
[preset Second]
chain = B
pedal = 7
warm = A
[preset First]
chain = A
pedal = 6
)", "/cfg/p.ini", c, &err), "%s", err.c_str());
    CHECK_MSG(presetsToChains(c, &err), "%s", err.c_str());
    const SceneSpec& s = c.scenes[0];
    CHECK(s.chains.size() == 3 && s.presets.size() == 3);
    // Chains in the presets' order, named after them; the unused chain last.
    CHECK(s.chains[0].name == "Second" && s.chains[1].name == "First" && s.chains[2].name == "Unused");
    CHECK(s.presets[0].chain == "Second" && s.presets[0].pedal == 7);
    CHECK(s.presets[0].warm == "First"); // was chain A, now named after its preset
    CHECK(s.presets[1].warm == "next" && s.presets[2].name == "Unused");
    CHECK(c.warnings.size() == 3); // two renames and the chain without a preset

    Config twice;
    CHECK(parseConfigText("[scene S]\n[chain A]\nplugin = builtin:gain 1\n[preset X]\nchain = A\n[preset Y]\nchain = A\n", "/cfg/p.ini", twice, &err));
    CHECK(!presetsToChains(twice, &err));
    CHECK(err.find("played by presets 'X' and 'Y'") != std::string::npos);
}

TEST(cue_file_rig_profile_names_the_channels) {
    const std::string dir = tempDir();
    makeStates(dir);
    Config ini;
    std::string err;
    CHECK_MSG(parseConfigText(kPerformance(dir), dir + "/perf.ini", ini, &err), "%s", err.c_str());
    CHECK_MSG(saveCue(ini, dir + "/perf.cue", &err), "%s", err.c_str());
    CHECK_MSG(saveRig(ini, dir + "/rig.json", &err), "%s", err.c_str());

    Config c;
    CHECK_MSG(loadPerformance(dir + "/perf.cue", dir + "/rig.json", c, &err), "%s", err.c_str());
    CHECK(c.audio.device == "hw:Test" && c.audio.period == 128 && c.audio.periods == 3);
    CHECK(!c.cpuAuto && c.audio.cpu == -1 && c.workers == 1);
    CHECK(c.audio.inputChannels == std::vector<unsigned>{1});
    CHECK(c.audio.outputChannels.size() == 2 && c.audio.outputChannels[0] == 3 && c.audio.outputChannels[1] == 2);
    CHECK(c.midi.in == "FCB" && c.midi.out == "FCB");
    CHECK(c.warnings.empty());

    // On a machine whose rig calls the outputs something else: by position, with a warning.
    writeFile(dir + "/other.json", R"({"cue_rig": 1, "audio": {"device": "hw:Other"}, "outputs": {"Amp": 5}})");
    Config o;
    CHECK_MSG(loadPerformance(dir + "/perf.cue", dir + "/other.json", o, &err), "%s", err.c_str());
    CHECK(o.audio.device == "hw:Other" && o.audio.outputChannels[0] == 0 && o.audio.outputChannels[1] == 4);
    CHECK(o.warnings.size() == 2); // the input Bass and the output FOH

    // A missing rig given by name is an error.
    CHECK(!loadPerformance(dir + "/perf.cue", dir + "/none.json", o, &err));
}

TEST(cue_file_reports_what_it_cannot_read) {
    const std::string dir = tempDir();
    Config c;
    std::string err;
    CHECK(!loadCue(dir + "/missing.cue", c, &err) && err.find("cannot open") != std::string::npos);
    writeFile(dir + "/text.cue", "not a zip");
    CHECK(!loadCue(dir + "/text.cue", c, &err) && err.find("is not a .cue file") != std::string::npos);

    auto zipWith = [&](const std::string& name, const std::string& json) {
        mz_zip_archive z{};
        mz_zip_writer_init_file(&z, (dir + "/" + name).c_str(), 0);
        mz_zip_writer_add_mem(&z, "performance.json", json.data(), json.size(), MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&z);
        mz_zip_writer_end(&z);
        return dir + "/" + name;
    };
    CHECK(!loadCue(zipWith("newer.cue", R"({"cue": 99, "saved_by": "Cue 9.0"})"), c, &err) && err.find("newer Cue (Cue 9.0)") != std::string::npos);
    CHECK(!loadCue(zipWith("broken.cue", R"({"cue": 1, "scenes": [)"), c, &err) && err.find("broken.cue") != std::string::npos);
    const std::string noState = R"({"cue": 1, "scenes": [{"name": "S", "chains": [{"name": "A", "lines": [{"plugins": [
        {"format": "clap", "path": "/x.clap", "state": "states/x.clapstate"}]}]}]}]})";
    CHECK(!loadCue(zipWith("nostate.cue", noState), c, &err) && err.find("states/x.clapstate is missing") != std::string::npos);
    const std::string badTap = R"({"cue": 1, "scenes": [{"name": "S", "chains": [{"name": "A", "lines": [{"plugins": []}],
        "outputs": {"Out 1": {"line": "FOH"}}}]}]})";
    CHECK(!loadCue(zipWith("badtap.cue", badTap), c, &err) && err.find("line 'FOH'") != std::string::npos);
    // The smallest performance: one scene, one chain, defaults for the rest.
    CHECK_MSG(loadCue(zipWith("small.cue", R"({"cue": 1, "scenes": [{"name": "S", "chains": [{"name": "A", "lines": [{"plugins": []}]}]}]})"), c, &err),
              "%s", err.c_str());
    CHECK(c.scenes[0].presets.size() == 1 && c.outputs.size() == 2 && c.scenes[0].chains[0].out[1].after == 0);
}

TEST(cue_file_keeps_every_input_and_output) {
    const std::string dir = tempDir();
    Config ini;
    std::string err;
    CHECK_MSG(parseConfigText(R"(
[performance Duo]
inputs = Bass, Synth
outputs = FOH, Amp, In-ear
[audio]
device = hw:Test
in2 = 4
out1 = 3
out2 = 4
out3 = 7
[chain Clean]
plugin = builtin:gain 1
tap = In-ear
plugin = builtin:gain 0.5
[chain Keys]
input = Synth
plugin = builtin:gain 1
branch = Wet
[branch Wet]
plugin = builtin:gain 0.3
tap = out2
)", dir + "/duo.ini", ini, &err), "%s", err.c_str());
    CHECK_MSG(saveCue(ini, dir + "/duo.cue", &err), "%s", err.c_str());
    CHECK_MSG(saveRig(ini, dir + "/rig.json", &err), "%s", err.c_str());

    Config c;
    CHECK_MSG(loadPerformance(dir + "/duo.cue", dir + "/rig.json", c, &err), "%s", err.c_str());
    CHECK((c.inputs == std::vector<std::string>{"Bass", "Synth"}));
    CHECK((c.outputs == std::vector<std::string>{"FOH", "Amp", "In-ear"}));
    CHECK(c.audio.inputChannels == (std::vector<unsigned>{0, 3}));
    CHECK(c.audio.outputChannels == (std::vector<unsigned>{2, 3, 6}));
    CHECK(c.warnings.empty());
    const auto& chains = c.scenes.at(0).chains;
    CHECK(chains.size() == 2);
    if (chains.size() != 2)
        return;
    CHECK(chains[0].inputIndex == 0 && chains[1].inputIndex == 1);
    CHECK(chains[0].out.size() == 3 && chains[0].out[2].after == 1 && chains[0].out[0].after == 2);
    CHECK(chains[1].out.size() == 3 && chains[1].out[1].line == 1 && chains[1].out[1].after == 1 && chains[1].out[0].line == 0);

    // A rig made for a performance with fewer outputs: the rest by position, with a warning.
    writeFile(dir + "/small.json", R"({"cue_rig": 1, "inputs": {"Bass": 2, "Synth": 1}, "outputs": {"FOH": 2, "Amp": 1}})");
    Config s;
    CHECK_MSG(loadPerformance(dir + "/duo.cue", dir + "/small.json", s, &err), "%s", err.c_str());
    CHECK(s.audio.inputChannels == (std::vector<unsigned>{1, 0}));
    CHECK(s.audio.outputChannels == (std::vector<unsigned>{1, 0, 2}));
    CHECK(s.warnings.size() == 1 && s.warnings[0].find("In-ear") != std::string::npos);
}
