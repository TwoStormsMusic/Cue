// Config parsing and the EurekaPROM MIDI map.

#include "check.h"

#include "app/config.h"
#include "midi/eurekaprom.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace cue;

namespace {
std::string writeTemp(const std::string& text) {
    char name[] = "/tmp/cue-config-XXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0)
        close(fd);
    std::ofstream(name) << text;
    return name;
}

bool parse(const std::string& text, Config& c, std::string& err) { return parseConfigText(text, "/cfg/test.ini", c, &err); }
} // namespace

TEST(config_reads_old_style_scenes_taps_and_settings) {
    const std::string path = writeTemp(R"(
[audio]
device = hw:M2
period = 64
input = 1
out1 = 1
out2 = 2      ; power amp
cpu = 7

[switching]
preroll_ms = 40
warm = none

[midi]
in = FCB
channel = 3

[scene Verse]
pedal = 2
plugin = clap:/x/Gain Stage.clap#com.twostorms.gainstage
param = 0:0.5
plugin = vst3:/x/Archetype Nolly X.vst3
tap = out2
plugin = builtin:delay 300 0.4 0.3
)");
    Config c;
    std::string err;
    CHECK_MSG(loadConfig(path, c, &err), "%s", err.c_str());
    std::remove(path.c_str());
    CHECK(c.audio.device == "hw:M2" && c.audio.period == 64 && c.audio.inputChannel == 0);
    CHECK(c.audio.outputChannels[1] == 1 && c.audio.cpu == 7 && !c.cpuAuto);
    CHECK(c.engine.prerollMs == 40.0f && c.warm == "none");
    CHECK(c.midi.in == "FCB" && c.midi.out == "FCB" && c.midi.channel == 2);
    CHECK(c.scenes.size() == 1);
    if (c.scenes.size() != 1 || c.scenes[0].chains.size() != 1)
        return;
    const SceneSpec& s = c.scenes[0];
    // An old [scene] is a scene with one chain and one preset of the same name.
    CHECK(s.name == "Verse" && s.pedal == 2 && s.chains[0].name == "Verse");
    CHECK(s.presets.size() == 1 && s.presets[0].chain == "Verse" && s.presets[0].pedal == 0);
    const auto plugins = chainPlugins(s.chains[0]);
    CHECK(plugins.size() == 3);
    CHECK(plugins[0]->kind == "clap" && plugins[0]->path == "/x/Gain Stage.clap" && plugins[0]->id == "com.twostorms.gainstage");
    CHECK(plugins[0]->params.size() == 1 && plugins[0]->params[0].second == 0.5);
    CHECK(plugins[1]->kind == "vst3" && plugins[1]->path == "/x/Archetype Nolly X.vst3");
    const ChainSpec& ch = s.chains[0];
    CHECK(ch.out[1].line == 0 && ch.out[1].after == 2); // out 2 before the delay
    CHECK(ch.out[0].line == 0 && ch.out[0].after == 3); // out 1 at the end
}

TEST(config_reads_scenes_with_chains_branches_and_presets) {
    Config c;
    std::string err;
    const bool ok = parse(R"(
[performance Friday]

[scene Covers]
display = CV

[chain Punk]
plugin = clap:gain.clap#com.twostorms.gainstage
name = Gain Stage
param = 54:1
plugin = clap:wah.clap
name = Auto Wah
bypass = yes
midi = no
plugin = vst3:~/.vst3/yabridge/Neural DSP/Darkglass Ultra.vst3#ABCDEF019182FAEB4E44535044524B55
state = states/punk-ultra.vststate
controller_state = states/punk-ultra.ctrl
branch = FOH
tap = out2
plugin = clap:delay.clap

[branch FOH]
plugin = clap:ir.clap
tap = out1

[chain Slap]
plugin = builtin:gain 1

[preset Punk]
display = PU
pedal = 6
do = Effect 1 = on

[preset Slap]
pedal = 7
)",
                          c, err);
    CHECK_MSG(ok, "%s", err.c_str());
    if (!ok)
        return;
    CHECK(c.performance == "Friday" && c.scenes.size() == 1);
    const SceneSpec& s = c.scenes[0];
    CHECK(s.display == "CV" && s.chains.size() == 2 && s.presets.size() == 2);
    const ChainSpec& punk = s.chains[0];
    CHECK(punk.lines.size() == 2);
    CHECK(punk.lines[0].plugins.size() == 4 && punk.lines[1].plugins.size() == 1);
    CHECK(punk.lines[1].name == "FOH" && punk.lines[1].parent == 0 && punk.lines[1].after == 3);
    CHECK(punk.out[0].line == 1 && punk.out[0].after == 1); // out 1 at the end of FOH
    CHECK(punk.out[1].line == 0 && punk.out[1].after == 3); // out 2 before the delay
    const PluginSpec& gain = punk.lines[0].plugins[0];
    CHECK(gain.label == "Gain Stage" && gain.path == "/cfg/gain.clap" && gain.params[0].first == 54);
    const PluginSpec& wah = punk.lines[0].plugins[1];
    CHECK(wah.label == "Auto Wah" && wah.bypassed && !wah.midi);
    const PluginSpec& ultra = punk.lines[0].plugins[2];
    CHECK(ultra.id == "ABCDEF019182FAEB4E44535044524B55" && ultra.stateFile == "/cfg/states/punk-ultra.vststate");
    CHECK(ultra.controllerStateFile == "/cfg/states/punk-ultra.ctrl");
    CHECK(s.presets[0].chain == "Punk" && s.presets[0].display == "PU" && s.presets[0].pedal == 6);
    CHECK(s.presets[0].actions.size() == 1 && s.presets[0].actions[0].text == "Effect 1 = on");
    CHECK(s.presets[0].actions[0].where == "/cfg/test.ini:32");
    CHECK(s.presets[1].chain == "Slap"); // the chain of the same name
    // Both outputs of Slap default to the end of its main line.
    CHECK(s.chains[1].out[0].after == 1 && s.chains[1].out[1].after == 1);
}

TEST(config_reads_controls_triggers_groups_and_fcb) {
    Config c;
    std::string err;
    const bool ok = parse(R"(
[chain Main]
plugin = builtin:gain 1

[control Wah]
values = Off, Pedal, Auto
default = Off
pedal = 4
enable = Auto Wah when Auto

[control Effect 1]
pedal = 1
default = on

[control Volume]
type = continuous
range = 0..10
step = 0.5
wrap = no
follows = expression 1
drive = Gain param 0 from 0 to 2

[control Tuner]
pedal = 5
mute = on

[trigger Reset]
on = pedal 3
on = cc 64 = 127
when = not Tuner
do = group Reset effects
do = preset next

[group Reset effects]
do = Effect 1 = on
do = Wah = Off

[fcb]
led 4 = Wah is not Off
display = TU when Tuner
)",
                          c, err);
    CHECK_MSG(ok, "%s", err.c_str());
    if (!ok)
        return;
    CHECK(c.controls.size() == 4 && c.triggers.size() == 1 && c.groups.size() == 1);
    const ControlSpec& wah = c.controls[0];
    CHECK(wah.type == ControlSpec::Type::Discrete && wah.min == 0 && wah.max == 2 && wah.labels.size() == 3);
    CHECK(wah.def == 0 && wah.pedal == 4 && wah.enables.size() == 1 && wah.wrap);
    const ControlSpec& fx = c.controls[1];
    CHECK(fx.type == ControlSpec::Type::Toggle && fx.def == 1 && fx.labels[1] == "on");
    const ControlSpec& vol = c.controls[2];
    CHECK(vol.type == ControlSpec::Type::Continuous && vol.min == 0 && vol.max == 10 && vol.step == 0.5 && !vol.wrap);
    CHECK(vol.follows.text == "expression 1" && vol.drives.size() == 1);
    CHECK(c.controls[3].mutes.size() == 1);
    const TriggerSpec& t = c.triggers[0];
    CHECK(t.inputs.size() == 2 && t.actions.size() == 2 && t.when.text == "not Tuner");
    CHECK(c.groups[0].name == "Reset effects" && c.groups[0].actions.size() == 2);
    CHECK(c.fcb.leds.size() == 1 && c.fcb.leds[0].first == 4 && c.fcb.displays.size() == 1);
    // A scene of its own for chains outside any [scene], with a preset per chain.
    CHECK(c.scenes.size() == 1 && c.scenes[0].presets.size() == 1 && c.scenes[0].presets[0].chain == "Main");
}

TEST(config_includes_files_relative_to_the_including_file) {
    const std::string chains = writeTemp("[chain A]\nplugin = clap:a.clap\n[chain B]\nplugin = builtin:gain 1\n");
    const std::string main = writeTemp("[scene S]\ninclude = " + chains + "\n[preset One]\nchain = B\n");
    Config c;
    std::string err;
    CHECK_MSG(loadConfig(main, c, &err), "%s", err.c_str());
    std::remove(chains.c_str());
    std::remove(main.c_str());
    CHECK(c.scenes.size() == 1 && c.scenes[0].chains.size() == 2);
    if (c.scenes.size() == 1 && c.scenes[0].chains.size() == 2) {
        CHECK(c.scenes[0].chains[0].lines[0].plugins[0].path == "/tmp/a.clap");
        CHECK(c.scenes[0].presets.size() == 1 && c.scenes[0].presets[0].chain == "B");
    }
}

TEST(config_reports_the_line_of_a_mistake) {
    const std::string path = writeTemp("[audio]\nperiod = sixty-four\n");
    Config c;
    std::string err;
    CHECK(!loadConfig(path, c, &err));
    CHECK_MSG(err.find(":2:") != std::string::npos, "%s", err.c_str());
    std::remove(path.c_str());

    Config d;
    CHECK(!parse("[chain A]\nplugin = builtin:gain 1\n[branch FOH]\nplugin = builtin:gain 1\n", d, err));
    CHECK_MSG(err.find("test.ini:3") != std::string::npos && err.find("branch = FOH") != std::string::npos, "%s", err.c_str());
    Config e;
    CHECK(!parse("[chain A]\nplugin = builtin:gain 1\n[preset P]\nchain = Nope\n", e, err));
    CHECK_MSG(err.find("no chain named Nope") != std::string::npos, "%s", err.c_str());
    Config f;
    CHECK(!parse("[chain A]\nplugin = builtin:gain 1\n[control Wah]\nvalues = Off, On\ndefault = Maybe\n", f, err));
    CHECK_MSG(err.find("Maybe") != std::string::npos, "%s", err.c_str());

    // A preset that plays the first chain only because it names none is
    // probably a mistake when the scene has several.
    Config g;
    CHECK(parse("[chain A]\nplugin = builtin:gain 1\n[chain B]\nplugin = builtin:gain 1\n[preset B]\n[preset Loud]\n", g, err));
    CHECK_MSG(g.warnings.size() == 1 && g.warnings[0].find("preset 'Loud'") != std::string::npos && g.warnings[0].find("test.ini:6") != std::string::npos,
              "%s", g.warnings.empty() ? "no warning" : g.warnings[0].c_str());
}

TEST(eurekaprom_decodes_pedals_and_expression) {
    using namespace eurekaprom;
    CHECK(decode(MidiMessage::cc(0, 104, 3)).kind == Input::Kind::Press && decode(MidiMessage::cc(0, 104, 3)).pedal == 3);
    CHECK(decode(MidiMessage::cc(0, 104, 0)).pedal == 10);  // pedal 10 sends value 0
    CHECK(decode(MidiMessage::cc(0, 104, 10)).pedal == kUp);
    CHECK(decode(MidiMessage::cc(0, 105, 11)).kind == Input::Kind::Release && decode(MidiMessage::cc(0, 105, 11)).pedal == kDown);
    const auto e = decode(MidiMessage::cc(5, 103, 90));
    CHECK(e.kind == Input::Kind::Expression && e.expression == 2 && e.value == 90);
    CHECK(decode(MidiMessage::cc(0, 7, 100)).kind == Input::Kind::None);
}

TEST(eurekaprom_drives_leds_and_display) {
    using namespace eurekaprom;
    const auto on = led(0, 10, true), off = led(0, 4, false);
    CHECK(on.status == 0xB0 && on.data1 == 106 && on.data2 == 0);
    CHECK(off.data1 == 107 && off.data2 == 4);
    CHECK(displayNumber(0, 12).data1 == 108 && displayNumber(0, 12).data2 == 12);
    CHECK(displayLetter(0, true, 'C').data1 == 109 && displayLetter(0, true, 'C').data2 == 2);
    CHECK(displayLetter(0, false, ' ').data2 == 26); // blank
    // "TU": leading digit blank, then two letters.
    const auto tu = displayText(0, "TU");
    CHECK(tu.size() == 3 && tu[0].data1 == 112 && tu[0].data2 == 5);
    CHECK(tu[1].data1 == 109 && tu[1].data2 == 19 && tu[2].data1 == 110 && tu[2].data2 == 20);
    // Digits go on the hex controllers; a leading '-' on the leading digit.
    const auto m12 = displayText(0, "-12");
    CHECK(m12[0].data2 == 2 && m12[1].data1 == 113 && m12[1].data2 == 1 && m12[2].data1 == 114 && m12[2].data2 == 2);
}
