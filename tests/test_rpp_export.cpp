// REAPER project export: a performance written as a project and read back
// by the import must keep its chains, lines, plug-ins, states and outputs.

#include "check.h"

#include "app/config.h"
#include "app/convert.h"
#include "app/cuefile.h"
#include "app/rpp.h"
#include "app/rpp_export.h"
#include "app/realearn.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace cue;

namespace {

std::string tempDir() {
    char name[] = "/tmp/cue-rppexport-XXXXXX";
    return mkdtemp(name) ? name : "/tmp";
}

void writeText(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

std::string text(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::string b64lines(const std::vector<uint8_t>& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t n = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        b += t[n >> 18 & 63];
        b += t[n >> 12 & 63];
        b += i + 1 < data.size() ? t[n >> 6 & 63] : '=';
        b += i + 2 < data.size() ? t[n & 63] : '=';
    }
    std::string out;
    for (size_t i = 0; i < b.size(); i += 128)
        out += "        " + b.substr(i, 128) + "\n";
    return out;
}

struct PathLocator : PluginLocator {
    std::string findClap(const std::string& id, const std::string&) override { return "/plugins/" + id + ".clap"; }
    std::string findVst3(const std::string& file) override { return "/vst3/" + file; }
};

// A chain like the imported template: a main line to the amp, a branch to
// FOH with the IR, a keyed gate and a VST3.
std::string performanceText(const std::string& dir) {
    writeText(dir + "/gain.clapstate", "TWOSTORMS 0 1\np 54 1\n");
    writeText(dir + "/ir.clapstate", "TWOSTORMS 0 1\nx ir0name Cab\n");
    writeText(dir + "/ultra.vst3state", "component-state");
    writeText(dir + "/ultra.vst3ctrl", "controller-state");
    return R"ini(
[performance]
outputs = FOH, Amp

[scene Covers]

[chain Punk]
plugin = clap:/plugins/com.twostorms.gainstage.clap#com.twostorms.gainstage
name = Gain Stage
state = gain.clapstate
plugin = clap:/plugins/com.twostorms.gate.clap#com.twostorms.gate
name = Gate
plugin = clap:/plugins/com.twostorms.gate.clap#com.twostorms.gate
name = Keyed Gate
sidechain = Gate
plugin = vst3:/vst3/Darkglass Ultra.vst3#ABCDEF019182FAEB4E44535044524B55
name = Darkglass Ultra
state = ultra.vst3state
controller_state = ultra.vst3ctrl
bypass = yes
branch = FOH line
plugin = clap:/plugins/com.twostorms.chorus.clap#com.twostorms.chorus
name = Chorus
tap = Amp

[branch FOH line]
plugin = clap:/plugins/com.twostorms.irloader.clap#com.twostorms.irloader
name = IR Loader
state = ir.clapstate
plugin = clap:/plugins/com.twostorms.chorus.clap#com.twostorms.chorus
name = Chorus
tap = FOH

[chain Slap]
plugin = clap:/plugins/com.twostorms.eq.clap#com.twostorms.eq
name = EQ
)ini";
}

} // namespace

TEST(rpp_export_round_trips_through_the_import) {
    const std::string dir = tempDir();
    Config c;
    std::string err;
    const bool loaded = parseConfigText(performanceText(dir), dir + "/show.ini", c, &err) && presetsToChains(c, &err);
    CHECK_MSG(loaded, "%s", err.c_str());
    if (!loaded)
        return;
    RppExport exp;
    CHECK_MSG(exportRpp(c, exp, &err, dir + "/no-reaper"), "%s", err.c_str());
    CHECK(exp.tracks == 3 && exp.plugins == 8 && exp.skipped == 0);
    // Only the first chain plays in REAPER.
    CHECK(exp.text.find("MUTESOLO 0 0 0") != std::string::npos && exp.text.find("MUTESOLO 1 0 0") != std::string::npos);

    PathLocator loc;
    Import imp;
    CHECK_MSG(convertRpp(exp.text, "Covers", loc, "", imp, &err), "%s", err.c_str());
    CHECK(imp.chains == 2 && imp.plugins == 8 && imp.skipped == 0 && imp.missing == 0);
    for (const auto& f : imp.files) {
        std::filesystem::create_directories(std::filesystem::path(dir + "/back/" + f.path).parent_path());
        std::ofstream(dir + "/back/" + f.path, std::ios::binary).write(reinterpret_cast<const char*>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
    }
    Config back;
    const bool ok = parseConfigText(imp.config, dir + "/back/Covers.ini", back, &err);
    CHECK_MSG(ok, "%s\n%s", err.c_str(), imp.config.c_str());
    if (!ok)
        return;
    CHECK(back.scenes.size() == 1 && back.scenes[0].chains.size() == 2);
    const ChainSpec& punk = back.scenes[0].chains[0];
    CHECK(punk.name == "Punk" && punk.lines.size() == 2);
    if (punk.lines.size() != 2)
        return;
    const auto& main = punk.lines[0].plugins;
    const auto& foh = punk.lines[1].plugins;
    CHECK(main.size() == 5 && foh.size() == 2 && punk.lines[1].after == 4);
    if (main.size() != 5 || foh.size() != 2)
        return;
    CHECK(main[0].label == "Gain Stage" && main[1].label == "Gate" && main[2].label == "Keyed Gate" && main[3].label == "Darkglass Ultra" &&
          main[4].label == "Chorus");
    CHECK(foh[0].label == "IR Loader" && foh[1].label == "Chorus");
    CHECK(main[2].sidechain == "Gate");
    CHECK(main[3].kind == "vst3" && main[3].bypassed && main[3].id == "ABCDEF019182FAEB4E44535044524B55" && main[3].path == "/vst3/Darkglass Ultra.vst3");
    CHECK(!main[0].bypassed && main[0].path == "/plugins/com.twostorms.gainstage.clap");
    // FOH takes the branch, the amp the end of the main line.
    CHECK(punk.out.size() == 2 && punk.out[0].line == 1 && punk.out[1].line == 0);

    std::vector<uint8_t> data;
    auto read = [&](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    };
    CHECK(read(main[0].stateFile) == "TWOSTORMS 0 1\np 54 1\n");
    CHECK(read(foh[0].stateFile) == "TWOSTORMS 0 1\nx ir0name Cab\n");
    CHECK(read(main[3].stateFile) == "component-state" && read(main[3].controllerStateFile) == "controller-state");
    std::filesystem::remove_all(dir);
}

TEST(rpp_export_puts_scenes_in_folders_and_lists_what_it_leaves_out) {
    const std::string dir = tempDir();
    Config c;
    std::string err;
    const bool loaded = parseConfigText(R"ini(
[scene One]
[chain A]
plugin = builtin:gain -6
[scene Two]
[chain B]
plugin = clap:/plugins/x.clap#com.example.x
name = X
bypass_mode = mute
)ini",
                                        dir + "/two.ini", c, &err) &&
                        presetsToChains(c, &err);
    CHECK_MSG(loaded, "%s", err.c_str());
    if (!loaded)
        return;
    RppExport exp;
    CHECK(exportRpp(c, exp, &err, dir));
    CHECK(exp.tracks == 5 && exp.plugins == 1 && exp.skipped == 1);
    CHECK(exp.text.find("NAME One\n    ISBUS 1 1") != std::string::npos && exp.text.find("ISBUS 2 -1") != std::string::npos);
    bool builtin = false, mute = false;
    for (const auto& n : exp.notes) {
        builtin = builtin || n.find("built-in") != std::string::npos;
        mute = mute || n.find("bypass_mode mute") != std::string::npos;
    }
    CHECK(builtin && mute);
    // The import makes each folder a scene again.
    struct NoLocator : PluginLocator {
        std::string findClap(const std::string&, const std::string&) override { return "/x.clap"; }
        std::string findVst3(const std::string&) override { return ""; }
    } loc;
    Import imp;
    CHECK(convertRpp(exp.text, "Show", loc, "", imp, &err));
    Config back;
    CHECK_MSG(parseConfigText(imp.config, dir + "/back.ini", back, &err), "%s\n%s", err.c_str(), imp.config.c_str());
    CHECK(back.scenes.size() == 2 && back.scenes[0].name == "One" && back.scenes[1].name == "Two" && back.scenes[1].chains.size() == 1);
    std::filesystem::remove_all(dir);
}

TEST(rpp_export_converts_files_both_ways) {
    const std::string dir = tempDir();
    writeText(dir + "/show.ini", performanceText(dir));
    ConvertOptions o;
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1); // no rig profile
    o.reaperDir = dir;
    o.scanClap = false;
    ConvertResult r;
    std::string err;
    // INI -> .cue (Cue's own convert), .cue -> .RPP, .RPP -> .cue.
    Config c;
    CHECK(loadConfig(dir + "/show.ini", c, &err) && saveCue(c, dir + "/show.cue", &err));
    CHECK_MSG(convertFile(dir + "/show.cue", "", o, r, &err), "%s", err.c_str());
    CHECK(r.out == dir + "/show.RPP" && r.chains == 2 && r.plugins == 8);
    std::filesystem::rename(dir + "/show.cue", dir + "/first.cue");
    CHECK_MSG(convertFile(dir + "/show.RPP", "", o, r, &err), "%s", err.c_str());
    CHECK(r.out == dir + "/show.cue" && r.chains == 2 && r.plugins == 8);
    // Converting back does not replace the project it came from.
    CHECK(convertFile(dir + "/show.cue", "", o, r, &err) && r.out == dir + "/show (2).RPP");
    Config back;
    const bool ok = loadCue(dir + "/show.cue", back, &err);
    CHECK_MSG(ok, "%s", err.c_str());
    if (ok && !back.scenes.empty() && back.scenes[0].chains.size() == 2) {
        const ChainSpec& punk = back.scenes[0].chains[0];
        CHECK(punk.lines.size() == 2 && punk.lines[0].plugins.size() == 5);
        if (!punk.lines.empty() && !punk.lines[0].plugins.empty())
            CHECK(text(punk.lines[0].plugins[0].state) == "TWOSTORMS 0 1\np 54 1\n");
    }
    std::filesystem::remove_all(dir);
}

namespace {

const char* kPedals = R"ini(
[performance]
outputs = FOH, Amp

[audio]
out1 = 2
out2 = 1
mix = sum

[scene Covers]
display = CV
pedal = 1

[chain Punk]
plugin = clap:/plugins/com.twostorms.wah.clap#com.twostorms.wah
name = Wah
plugin = clap:/plugins/com.twostorms.wah.clap#com.twostorms.wah
name = Wah
plugin = clap:/plugins/com.twostorms.gate.clap#com.twostorms.gate
name = Gate
branch = FOH line
tap = Amp

[branch FOH line]
plugin = clap:/plugins/com.twostorms.irloader.clap#com.twostorms.irloader
name = IR Loader
tap = FOH

[chain Slap]
plugin = clap:/plugins/com.twostorms.wah.clap#com.twostorms.wah
name = Wah

[preset Punk]
pedal = 6
display = PU
do = group Start

[preset Slap]
pedal = 7
program = 3
warm = none

[scene Originals]
[chain Djent]
plugin = clap:/plugins/com.twostorms.gate.clap#com.twostorms.gate
name = Gate

[control Wah]
values = Off, Auto, Pedal
pedal = 4
enable = Wah:1 when Auto
enable = Wah:2 when Pedal

[control Tuner]
pedal = 5
mute = on

[group Start]
do = Wah = Off

[trigger Gate off]
on = pedal 3
when = chain is Punk
do = bypass Gate

[fcb]
led value 1 = Wah is not Off
display = TU when Tuner
)ini";

std::string importedConfig(const std::string& rpp, std::vector<std::string>& notes) {
    struct Loc : PluginLocator {
        std::string findClap(const std::string& id, const std::string&) override { return "/plugins/" + id + ".clap"; }
        std::string findVst3(const std::string& file) override { return "/vst3/" + file; }
    } loc;
    Import imp;
    std::string err;
    CHECK_MSG(convertRpp(rpp, "Show", loc, "", imp, &err), "%s", err.c_str());
    notes = imp.notes;
    return imp.config;
}


// The ReaLearn state inside an exported project.
nlohmann::json realearnOf(const std::string& rpp) {
    RppNode root;
    std::string err;
    if (!parseRpp(rpp, root, &err))
        return {};
    static const std::string abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (const RppNode& track : root.children[0].children)
        if (const RppNode* chain = track.child("FXCHAIN"))
            for (const RppNode& fx : chain->children)
                if (fx.head.size() > 1 && fx.head[1].find("ReaLearn") != std::string::npos) {
                    // Line by line, as REAPER writes them.
                    std::vector<uint8_t> body;
                    for (const auto& it : fx.items) {
                        if (it.child >= 0)
                            continue;
                        uint32_t acc = 0;
                        int bits = 0;
                        for (char c : it.line) {
                            if (c == '=')
                                break;
                            acc = acc << 6 | static_cast<uint32_t>(abc.find(c));
                            bits += 6;
                            if (bits >= 8) {
                                bits -= 8;
                                body.push_back(static_cast<uint8_t>(acc >> bits));
                            }
                        }
                    }
                    std::string text;
                    if (realearnJson(body, text))
                        return nlohmann::json::parse(text);
                }
    return {};
}

std::vector<const nlohmann::json*> named(const nlohmann::json& unit, const std::string& prefix) {
    std::vector<const nlohmann::json*> out;
    for (const auto& m : unit["mappings"])
        if (m.value("name", "").rfind(prefix, 0) == 0)
            out.push_back(&m);
    return out;
}

} // namespace

TEST(rpp_export_carries_pedals_both_ways) {
    const std::string dir = tempDir();
    Config c;
    std::string err;
    const bool loaded = parseConfigText(kPedals, dir + "/pedals.ini", c, &err) && presetsToChains(c, &err);
    CHECK_MSG(loaded, "%s", err.c_str());
    if (!loaded)
        return;
    RppExport exp;
    CHECK_MSG(exportRpp(c, exp, &err, dir), "%s", err.c_str());
    // FOH plays on channel 2 and the amp on 1: the sends say so.
    CHECK(exp.text.find("HWOUT 1025 0 1 0 0 0 1026") != std::string::npos && exp.text.find("HWOUT 1024 0 1 0 0 0 1024") != std::string::npos);

    // REAPER side: ReaLearn's layout.
    const nlohmann::json rl = realearnOf(exp.text);
    CHECK(rl.contains("mainUnit"));
    if (!rl.contains("mainUnit"))
        return;
    const nlohmann::json& unit = rl["mainUnit"];
    CHECK(unit["parameters"]["0"]["value"].get<double>() == 0.01);
    CHECK(unit["parameters"]["1"]["name"] == "Wah" && unit["parameters"]["2"]["name"] == "Tuner");
    auto punk = named(unit, "Pedal 6: Punk");
    CHECK(punk.size() == 1);
    if (punk.size() == 1) {
        CHECK((*punk[0])["mode"]["minTargetValue"].get<double>() == 0.01);
        CHECK(std::abs((*punk[0])["mode"]["minSourceValue"].get<double>() - 6 / 127.0) < 1e-9);
        CHECK((*punk[0])["activationType"] == "expression"); // two scenes: only in its own
    }
    CHECK(named(unit, "Punk plays").size() == 1 && named(unit, "Djent plays").size() == 1);
    CHECK(named(unit, "Pedal 4: Wah").size() == 1 && named(unit, "Pedal 4: Wah")[0]->at("mode")["type"] == 1);
    // Wah:1 in each chain when Auto, Wah:2 (Punk's second) when Pedal.
    CHECK(named(unit, "Wah switches").size() == 3);
    CHECK(named(unit, "Punk: group Start").size() == 1);
    CHECK(named(unit, "Gate off").size() == 2); // Punk's gate and Djent's, while Punk plays
    CHECK(named(unit, "LED 18").size() == 2);
    CHECK(named(unit, "Punk display").size() == 3);
    bool tunerMute = false, display = false;
    for (const auto& n : exp.notes) {
        tunerMute = tunerMute || n.find("mute = on") != std::string::npos;
        display = display || n.find("[fcb] display") != std::string::npos;
    }
    CHECK(tunerMute && display);

    // And back: the performance as it was.
    std::vector<std::string> notes;
    const std::string ini = importedConfig(exp.text, notes);
    Config back;
    const bool ok = parseConfigText(ini, dir + "/back.ini", back, &err) && presetsToChains(back, &err);
    CHECK_MSG(ok, "%s\n%s", err.c_str(), ini.c_str());
    if (!ok)
        return;
    CHECK(back.outputs == std::vector<std::string>({"FOH", "Amp"}));
    CHECK(back.audio.outputChannels == std::vector<unsigned>({1, 0}));
    CHECK(back.mix == OutputMix::Sum);
    CHECK(back.scenes.size() == 2 && back.scenes[0].display == "CV" && back.scenes[0].pedal == 1);
    if (back.scenes.size() != 2 || back.scenes[0].chains.size() != 2)
        return;
    const ChainSpec& p = back.scenes[0].chains[0];
    CHECK(p.lines.size() == 2 && p.lines[1].name == "FOH line");
    CHECK(p.out.size() == 2 && p.out[0].line == 1 && p.out[1].line == 0);
    const PresetSpec& pp = back.scenes[0].presets[0];
    CHECK(pp.pedal == 6 && pp.display == "PU" && pp.actions.size() == 1 && pp.actions[0].text == "group Start");
    const PresetSpec& sp = back.scenes[0].presets[1];
    CHECK(sp.pedal == 7 && sp.program == 3 && sp.warm == "none");
    CHECK(back.controls.size() == 2 && back.controls[0].name == "Wah" && back.controls[0].enables.size() == 2 && back.controls[0].pedal == 4);
    CHECK(back.triggers.size() == 1 && back.triggers[0].when.text == "chain is Punk");
    CHECK(back.groups.size() == 1 && back.fcb.leds.size() == 1 && back.fcb.displays.size() == 1);
    std::filesystem::remove_all(dir);
}

TEST(rpp_import_turns_simple_realearn_mappings_into_triggers) {
    // A gate on one track and someone else's ReaLearn: pedal 3 toggles the
    // gate, CC 20 = 127 selects the track, and one mapping works through a
    // ReaLearn parameter.
    const std::string json = R"({"mainUnit":{"groups":[{"id":"g","name":"Pedal 3"}],"mappings":[
      {"id":"a","name":"Gate","groupId":"g","source":{"channel":0,"number":104},"mode":{"type":2,"minSourceValue":0.023622047244094488,"maxSourceValue":0.023622047244094488},
       "target":{"type":12,"trackGUID":"D23E2D9F-8959-C083-4EE2-90E84B6F3B99","fxGUID":"64BF3C86-12D3-ADD3-D5FE-8C87E323E207"}},
      {"id":"b","name":"Punk","source":{"number":20},"mode":{"minSourceValue":1.0},"target":{"type":6,"trackGUID":"D23E2D9F-8959-C083-4EE2-90E84B6F3B99"}},
      {"id":"c","name":"Wah step","source":{"category":"reaper","reaperSourceType":"realearn-parameter"},"target":{"paramIndex":4},"activationType":"expression","eelCondition":"p[0] > 0.5"},
      {"id":"d","name":"LED","source":{"number":106},"target":{"type":12},"controlIsEnabled":false}]}})";
    std::vector<uint8_t> body = realearnChunk(json);
    body.insert(body.end(), json.begin(), json.end());
    for (char c : std::string("\0Program 1\0\0\0\0\0", 15))
        body.push_back(static_cast<uint8_t>(c));
    const std::string rpp = "<REAPER_PROJECT 0.1 \"7.82/linux-x86_64\" 0 0\n"
                            "  <TRACK {D23E2D9F-8959-C083-4EE2-90E84B6F3B99}\n    NAME Punk\n    HWOUT 1024 0 1 0 0 0 1024 -1:U -1\n    <FXCHAIN\n"
                            "      BYPASS 0 0 0\n      <CLAP \"CLAP: TwoStorms Gate (TwoStorms)\" com.twostorms.gate \"\"\n      >\n"
                            "      FXID {64BF3C86-12D3-ADD3-D5FE-8C87E323E207}\n    >\n  >\n"
                            "  <TRACK {2A734110-E9BD-B328-BF82-2334CD58CE66}\n    NAME ReaLearn\n    <FXCHAIN\n"
                            "      <VST \"VSTi: Helgobox - ReaLearn & Playtime (Helgoboss)\" helgobox-x64.so 0 \"\" "
                            "1751282284<5653546862726C68656C676F626F7872> \"\"\n" +
                            b64lines(body) + "      >\n    >\n  >\n>\n";
    std::vector<std::string> notes;
    const std::string ini = importedConfig(rpp, notes);
    CHECK_MSG(ini.find("[trigger Gate]\non = pedal 3\ndo = toggle Punk/TwoStorms Gate\n") != std::string::npos, "%s", ini.c_str());
    CHECK(ini.find("[trigger Punk]\non = cc 20 = 127\ndo = chain Punk\n") != std::string::npos);
    bool converted = false, formula = false, realearnTrack = false;
    for (const auto& n : notes) {
        converted = converted || n.find("2 mapping(s) became triggers") != std::string::npos;
        formula = formula || (n.find("1 mapping(s) use ReaLearn parameters") != std::string::npos && n.find("Wah step") != std::string::npos);
        realearnTrack = realearnTrack || n.find("track ReaLearn") != std::string::npos;
    }
    CHECK(converted && formula && !realearnTrack);
    Config c;
    std::string err;
    CHECK_MSG(parseConfigText(ini, "/tmp/x.ini", c, &err), "%s", err.c_str());

    // With a pedal file the pedals come from it, and ReaLearn is left alone.
    const std::string dir = tempDir();
    writeText(dir + "/pedals.ini", "[trigger Five]\non = pedal 5\ndo = chain Punk\n");
    PathLocator loc;
    Import imp;
    CHECK_MSG(convertRpp(rpp, "Show", loc, dir + "/pedals.ini", imp, &err), "%s", err.c_str());
    CHECK_MSG(imp.config.find("include = " + dir + "/pedals.ini") != std::string::npos && imp.config.find("[trigger Gate]") == std::string::npos, "%s", imp.config.c_str());
    for (const auto& n : imp.notes)
        CHECK_MSG(n.find("became triggers") == std::string::npos, "%s", n.c_str());
    std::filesystem::remove_all(dir);
}
