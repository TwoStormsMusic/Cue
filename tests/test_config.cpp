// Config parsing and the EurekaPROM MIDI map.

#include "check.h"

#include "app/config.h"
#include "midi/eurekaprom.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace sh;

namespace {
std::string writeTemp(const std::string& text) {
    char name[] = "/tmp/stagehost-config-XXXXXX";
    const int fd = mkstemp(name);
    if (fd >= 0)
        close(fd);
    std::ofstream(name) << text;
    return name;
}
} // namespace

TEST(config_reads_scenes_taps_and_settings) {
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
    if (c.scenes.size() != 1)
        return;
    const SceneSpec& s = c.scenes[0];
    CHECK(s.name == "Verse" && s.pedal == 2 && s.chain.size() == 3);
    CHECK(s.chain[0].kind == "clap" && s.chain[0].path == "/x/Gain Stage.clap" && s.chain[0].id == "com.twostorms.gainstage");
    CHECK(s.chain[0].params.size() == 1 && s.chain[0].params[0].second == 0.5);
    CHECK(s.chain[1].kind == "vst3" && s.chain[1].path == "/x/Archetype Nolly X.vst3");
    CHECK(s.out2After == 2 && s.out1After == SIZE_MAX); // out 2 before the delay, out 1 at the end
}

TEST(config_reports_the_line_of_a_mistake) {
    const std::string path = writeTemp("[audio]\nperiod = sixty-four\n");
    Config c;
    std::string err;
    CHECK(!loadConfig(path, c, &err));
    CHECK_MSG(err.find(":2:") != std::string::npos, "%s", err.c_str());
    std::remove(path.c_str());
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
}
