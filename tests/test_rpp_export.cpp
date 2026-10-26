// REAPER project export: a performance written as a project and read back
// by the import must keep its chains, lines, plug-ins, states and outputs.

#include "check.h"

#include "app/config.h"
#include "app/convert.h"
#include "app/cuefile.h"
#include "app/rpp.h"
#include "app/rpp_export.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    CHECK(exp.tracks == 2 && exp.plugins == 8 && exp.skipped == 0);
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
    CHECK(exp.tracks == 4 && exp.plugins == 1 && exp.skipped == 1);
    CHECK(exp.text.find("NAME One\n    ISBUS 1 1") != std::string::npos && exp.text.find("ISBUS 2 -1") != std::string::npos);
    bool builtin = false, mute = false;
    for (const auto& n : exp.notes) {
        builtin = builtin || n.find("built-in") != std::string::npos;
        mute = mute || n.find("bypass_mode mute") != std::string::npos;
    }
    CHECK(builtin && mute);
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
