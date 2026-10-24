// The show: presets, controls, triggers, groups and FCB feedback, against a
// recording output instead of an engine.

#include "check.h"

#include "app/config.h"
#include "control/show.h"
#include "midi/eurekaprom.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace cue;

namespace {

struct Recorder : ShowOutput {
    std::vector<int> switches;
    int warm = -2;
    std::map<std::pair<int, size_t>, bool> bypass; // latest per plug-in
    int bypassCalls = 0;
    std::vector<std::tuple<int, size_t, uint32_t, double>> params;
    std::vector<bool> mutes;
    std::vector<MidiMessage> midi;
    std::vector<std::pair<std::pair<int, size_t>, MidiMessage>> forwarded;

    void switchTo(int chain) override { switches.push_back(chain); }
    void setWarm(int chain) override { warm = chain; }
    void setBypass(int chain, size_t slot, bool b) override {
        bypass[{chain, slot}] = b;
        ++bypassCalls;
    }
    void setParam(int chain, size_t slot, uint32_t id, double v) override { params.emplace_back(chain, slot, id, v); }
    void setMute(bool m) override { mutes.push_back(m); }
    void sendMidi(const MidiMessage& m) override { midi.push_back(m); }
    void forwardMidi(int chain, size_t slot, const MidiMessage& m) override { forwarded.push_back({{chain, slot}, m}); }

    // -1 never set, else 1 bypassed, 0 running.
    int by(int chain, size_t slot) const {
        auto it = bypass.find({chain, slot});
        return it == bypass.end() ? -1 : it->second ? 1 : 0;
    }

    // The LED state the FCB would show, from every message sent.
    bool ledLit(int pedal) const {
        bool on = false;
        for (const auto& m : midi)
            if (m.data2 == eurekaprom::ledValueForPedal(pedal) && (m.data1 == eurekaprom::kLedOnCC || m.data1 == eurekaprom::kLedOffCC))
                on = m.data1 == eurekaprom::kLedOnCC;
        return on;
    }
};

// Every plug-in of the config, labelled as configured (or "P<slot>").
std::vector<ShowPlugin> pluginsOf(const Config& c) {
    std::vector<ShowPlugin> out;
    int chain = 0;
    for (const auto& s : c.scenes)
        for (const auto& ch : s.chains) {
            size_t slot = 0;
            for (const PluginSpec* p : chainPlugins(ch)) {
                ShowPlugin sp;
                sp.chain = chain;
                sp.slot = slot;
                sp.label = p->label.empty() ? "P" + std::to_string(slot) : p->label;
                sp.midi = p->midi && p->label.find("Wah") != std::string::npos;
                sp.bypassed = p->bypassed;
                out.push_back(sp);
                ++slot;
            }
            ++chain;
        }
    return out;
}

// What the FCB sends: pedals 1-9 as themselves, 10 as 0, up as 10, down as 11.
MidiMessage press(int pedal) {
    const int v = pedal == 10 ? 0 : pedal == eurekaprom::kUp ? 10 : pedal == eurekaprom::kDown ? 11 : pedal;
    return MidiMessage::cc(0, eurekaprom::kPressCC, static_cast<uint8_t>(v));
}

// Adam's template, in miniature: five chains selected by pedals 6-10, with
// a Wah cycle on pedal 4, three effect switches and a tuner.
const char* kTemplate = R"(
[midi]
channel = 1

[scene Template]

[chain Punk]
plugin = builtin:gain 1
name = Gain Stage
plugin = builtin:gain 1
name = Auto Wah
bypass = yes
plugin = builtin:gain 1
name = Pedal Wah
bypass = yes
[chain Slap]
plugin = builtin:gain 1
name = Gain Stage
plugin = builtin:gain 1
name = Auto Wah
bypass = yes
plugin = builtin:gain 1
name = Pedal Wah
bypass = yes
[chain Djent]
plugin = builtin:gain 1
name = Gain Stage

[preset Punk]
pedal = 6
display = PU
do = group Reset effects
[preset Slap]
pedal = 7
display = SL
do = group Reset effects
[preset Djent]
pedal = 10
do = group Reset effects

[control Wah]
values = Off, Pedal, Auto
pedal = 4
enable = Auto Wah when Auto
enable = Pedal Wah when Pedal

[control Effect 1]
pedal = 1
[control Effect 2]
pedal = 2
[control Effect 3]
pedal = 3

[control Tuner]
pedal = 5
mute = on

[group Reset effects]
do = Effect 1 = on
do = Effect 2 = off
do = Effect 3 = off
do = Wah = Off

[fcb]
display = TU when Tuner
)";

bool loadShow(Show& show, const std::string& text, Config& config, std::string& err) {
    if (!parseConfigText(text, "/cfg/t.ini", config, &err))
        return false;
    return show.load(config, pluginsOf(config), &err);
}

} // namespace

TEST(show_pedals_select_presets_and_warm_the_next_chain) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    CHECK_MSG(loadShow(show, kTemplate, c, err), "%s", err.c_str());
    show.start();
    CHECK(show.preset() == 0 && out.switches.size() == 1 && out.switches[0] == 0);
    CHECK(out.warm == 1); // Slap is next
    show.onMidi(press(7));
    CHECK(show.preset() == 1 && out.switches.back() == 1 && out.warm == 2);
    show.onMidi(press(10));
    CHECK(show.preset() == 2 && out.switches.back() == 2 && out.warm == 0);
    // The same preset again does not switch chains again.
    const size_t before = out.switches.size();
    show.onMidi(press(10));
    CHECK(out.switches.size() == before);
    // Up and down step through the presets.
    show.onMidi(press(eurekaprom::kUp));
    CHECK(show.preset() == 0);
    show.onMidi(press(eurekaprom::kDown));
    CHECK(show.preset() == 2);
}

TEST(show_wah_cycles_wraps_and_switches_the_right_plugins_in_every_chain) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    CHECK_MSG(loadShow(show, kTemplate, c, err), "%s", err.c_str());
    show.start();
    CHECK(show.valueText("Wah") == "Off");
    CHECK(out.bypassCalls == 0); // the wahs start bypassed, as the engine already has them
    show.onMidi(press(4));
    CHECK(show.valueText("Wah") == "Pedal");
    CHECK(out.by(0, 2) == 0 && out.by(1, 2) == 0); // Pedal Wah on in Punk and Slap
    CHECK(out.by(0, 1) == -1);                               // Auto Wah untouched
    CHECK(out.ledLit(4));
    show.onMidi(press(4));
    CHECK(show.valueText("Wah") == "Auto");
    CHECK(out.by(0, 1) == 0 && out.by(0, 2) == 1);
    show.onMidi(press(4)); // past the end: back to the start
    CHECK(show.valueText("Wah") == "Off");
    CHECK(out.by(0, 1) == 1 && out.by(1, 1) == 1 && out.by(0, 2) == 1);
    CHECK(!out.ledLit(4));
}

TEST(show_selecting_a_preset_runs_its_group) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    CHECK_MSG(loadShow(show, kTemplate, c, err), "%s", err.c_str());
    show.start();
    CHECK(show.valueText("Effect 1") == "on" && out.ledLit(1)); // the group ran on start
    show.onMidi(press(1));
    show.onMidi(press(2));
    show.onMidi(press(4));
    CHECK(show.valueText("Effect 1") == "off" && show.valueText("Effect 2") == "on" && show.valueText("Wah") == "Pedal");
    CHECK(!out.ledLit(1) && out.ledLit(2));
    show.onMidi(press(7)); // Slap: effects back to how the preset starts
    CHECK(show.valueText("Effect 1") == "on" && show.valueText("Effect 2") == "off" && show.valueText("Wah") == "Off");
    CHECK(out.ledLit(1) && !out.ledLit(2) && out.ledLit(7) && !out.ledLit(6));
}

TEST(show_tuner_mutes_and_shows_tu) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    CHECK_MSG(loadShow(show, kTemplate, c, err), "%s", err.c_str());
    show.start();
    CHECK(show.display() == "PU" && !show.muted());
    show.onMidi(press(5));
    CHECK(show.muted() && !out.mutes.empty() && out.mutes.back());
    CHECK(show.display() == "TU" && out.ledLit(5));
    show.onMidi(press(5));
    CHECK(!show.muted() && !out.mutes.back() && show.display() == "PU");
    show.onMidi(press(10));
    CHECK(show.display() == "#3"); // no display text: the preset's number
}

TEST(show_one_input_runs_many_actions_and_groups) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    const std::string text = std::string(kTemplate) + R"(
[trigger Everything]
on = cc 64 = 127
on = key x
do = Effect 3 = on
do = Wah = Auto
do = preset Djent
do = mute on

[trigger Undo]
on = cc 64 = 0
do = group Reset effects
do = mute off
)";
    CHECK_MSG(loadShow(show, text, c, err), "%s", err.c_str());
    show.start();
    show.onMidi(MidiMessage::cc(0, 64, 127));
    // The preset ran its own reset after Effect 3 and Wah were set; then mute.
    CHECK(show.preset() == 2 && show.muted());
    CHECK(show.valueText("Effect 3") == "off" && show.valueText("Wah") == "Off");
    show.onMidi(MidiMessage::cc(0, 64, 0));
    CHECK(!show.muted());
    show.run("Effect 3 = on", &err);
    CHECK(show.onKey('x') && show.preset() == 2);
    CHECK(!show.onKey('z'));
}

TEST(show_continuous_controls_step_reach_the_end_and_wrap) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    const std::string text = R"(
[chain A]
plugin = builtin:gain 1
name = Delay
[control Mix]
type = continuous
range = 0..1
step = 0.3
drive = Delay param 2 from 0 to 100
[control Clamp]
type = discrete
range = 1..3
wrap = no
[trigger]
on = pedal 1
do = Mix = next
do = Clamp = up 1
[trigger]
on = pedal 2
do = Mix = down 0.5
)";
    CHECK_MSG(loadShow(show, text, c, err), "%s", err.c_str());
    show.start();
    std::vector<double> seen;
    for (int i = 0; i < 6; ++i) {
        show.onMidi(press(1));
        seen.push_back(show.value("Mix"));
    }
    const double want[] = {0.3, 0.6, 0.9, 1.0, 0.0, 0.3};
    for (int i = 0; i < 6; ++i)
        CHECK_MSG(std::fabs(seen[i] - want[i]) < 1e-9, "step %d: %.3f, expected %.3f", i, seen[i], want[i]);
    CHECK(show.value("Clamp") == 3); // no wrap: stays at the end
    show.onMidi(press(2));
    CHECK(std::fabs(show.value("Mix") - 0.0) < 1e-9); // 0.3 - 0.5 reaches the start first
    show.onMidi(press(2));
    CHECK(std::fabs(show.value("Mix") - 1.0) < 1e-9); // then wraps to the end
    // The drive followed: Mix 1.0 = param 100.
    CHECK(!out.params.empty() && std::get<2>(out.params.back()) == 2 && std::fabs(std::get<3>(out.params.back()) - 100.0) < 1e-9);
}

TEST(show_expression_pedal_follows_and_reaches_midi_plugins) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    const std::string text = std::string(kTemplate) + R"(
[control Volume]
type = continuous
range = 0..2
follows = expression 1
)";
    CHECK_MSG(loadShow(show, text, c, err), "%s", err.c_str());
    show.start();
    show.onMidi(MidiMessage::cc(0, eurekaprom::kLeftExprCC, 127));
    CHECK(std::fabs(show.value("Volume") - 2.0) < 1e-9);
    // The wahs take MIDI: the expression pedal reaches all four, in idle chains too.
    CHECK(out.forwarded.size() == 4);
    // Pedal presses are switches, not passed on.
    out.forwarded.clear();
    show.onMidi(press(4));
    CHECK(out.forwarded.empty());
}

TEST(show_conditions_leds_and_scoped_triggers) {
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    const std::string text = std::string(kTemplate) + R"(
[trigger Only in Djent]
on = pedal 8
when = preset is Djent and not Tuner
do = Effect 3 = toggle

[fcb]
led 9 = Wah is Auto or (Effect 2 and Effect 3)
)";
    CHECK_MSG(loadShow(show, text, c, err), "%s", err.c_str());
    show.start();
    show.onMidi(press(8));
    CHECK(show.valueText("Effect 3") == "off"); // not in Djent
    show.onMidi(press(10));
    show.onMidi(press(8));
    CHECK(show.valueText("Effect 3") == "on");
    CHECK(!out.ledLit(9));
    show.onMidi(press(2));
    CHECK(out.ledLit(9)); // Effect 2 and Effect 3
    show.onMidi(press(2));
    CHECK(!out.ledLit(9));
    show.onMidi(press(4));
    show.onMidi(press(4));
    CHECK(out.ledLit(9)); // Wah is Auto
}

TEST(show_reports_mistakes_with_their_line) {
    Recorder out;
    std::string err;
    auto fails = [&](const std::string& extra, const char* expect) {
        Show show(out);
        Config c;
        const bool ok = loadShow(show, std::string(kTemplate) + extra, c, err);
        CHECK_MSG(!ok && err.find(expect) != std::string::npos, "expected '%s' in: %s", expect, err.c_str());
    };
    fails("[trigger T]\non = pedal 3\ndo = Wha = next\n", "Wha = next");
    fails("[trigger T]\non = pedal 3\ndo = Wah = Sometimes\n", "its values: Off, Pedal, Auto");
    fails("[trigger T]\non = pedal 3\ndo = group Nope\n", "no group named 'Nope'");
    fails("[trigger T]\non = pedal 33\ndo = Wah = next\n", "pedal is 1 to 10");
    fails("[control X]\nenable = Flanger when on\n", "no plug-in named 'Flanger'");
    fails("[fcb]\nled 3 = Wah is Purple\n", "not a value of Wah");
}

TEST(show_template_mapping_does_what_reaLearn_did) {
    // The shipped mapping, included after five chains as an import makes them.
    std::string text = "[scene Cue Template]\n";
    for (const char* name : {"Punk", "Slap", "2000s", "Motown", "Djent"})
        text += std::string("[chain ") + name +
                "]\nplugin = builtin:gain 1\nname = TwoStorms Gain Stage\n"
                "plugin = builtin:gain 1\nname = TwoStorms Wah\nbypass = yes\n"
                "plugin = builtin:gain 1\nname = TwoStorms Wah\nbypass = yes\n";
    const char* configs = std::getenv("CUE_CONFIGS") ? std::getenv("CUE_CONFIGS") : CUE_SOURCE_DIR "/configs";
    text += std::string("include = ") + configs + "/cue-template-mapping.ini\n";
    Recorder out;
    Show show(out);
    Config c;
    std::string err;
    CHECK_MSG(loadShow(show, text, c, err), "%s", err.c_str());
    show.start();
    CHECK(show.numPresets() == 5 && show.presetName(0) == "Punk" && show.display() == "PU");
    CHECK(show.valueText("Effect 1") == "on" && out.ledLit(1) && out.ledLit(6));
    show.onMidi(press(9));
    CHECK(show.presetName(show.preset()) == "Motown" && out.switches.back() == 3 && show.display() == "MO");
    show.onMidi(press(4)); // auto wah: the first wah, in every chain
    CHECK(show.valueText("Wah") == "Auto");
    for (int chain = 0; chain < 5; ++chain)
        CHECK(out.by(chain, 1) == 0 && out.by(chain, 2) == -1);
    show.onMidi(press(4)); // pedal wah: the second
    CHECK(show.valueText("Wah") == "Pedal" && out.by(3, 1) == 1 && out.by(3, 2) == 0);
    show.onMidi(press(5));
    CHECK(show.muted() && show.display() == "TU");
    show.onMidi(press(5));
    show.onMidi(press(8)); // 2000s: the wah resets
    CHECK(show.valueText("Wah") == "Off" && out.by(3, 2) == 1 && show.display() == "OO" && !show.muted());
}
