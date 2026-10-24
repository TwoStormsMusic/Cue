// REAPER project import, from a small project written here.

#include "check.h"

#include "app/config.h"
#include "app/rpp.h"

#include <cstring>
#include <string>
#include <vector>

using namespace cue;

namespace {

std::string base64(const std::vector<uint8_t>& data) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t n = (data[i] << 16) | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        out += t[n >> 18 & 63];
        out += t[n >> 12 & 63];
        out += i + 1 < data.size() ? t[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? t[n & 63] : '=';
    }
    return out;
}

// REAPER's layout: lines of 128 characters, a group ends with a shorter line.
std::string lines(const std::vector<uint8_t>& data, const std::string& indent) {
    const std::string b = base64(data);
    std::string out;
    for (size_t i = 0; i < b.size(); i += 128)
        out += indent + b.substr(i, 128) + "\n";
    return out;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}

std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// A VST3 body as REAPER writes it: header, chunk, program name.
std::string vst3Body(const std::string& comp, const std::string& ctrl) {
    std::vector<uint8_t> chunk;
    put32(chunk, static_cast<uint32_t>(comp.size()));
    put32(chunk, 1);
    chunk.insert(chunk.end(), comp.begin(), comp.end());
    put32(chunk, static_cast<uint32_t>(ctrl.size()));
    put32(chunk, 0);
    chunk.insert(chunk.end(), ctrl.begin(), ctrl.end());
    std::vector<uint8_t> header{0xf6, 0x1a, 0xe2, 0x23, 0xee, 0x5e, 0xed, 0xfe};
    put32(header, 2);
    put64(header, 1);
    put64(header, 2);
    put32(header, 2);
    put64(header, 1);
    put64(header, 2);
    put32(header, static_cast<uint32_t>(chunk.size()));
    put32(header, 1);
    put32(header, 0);
    return lines(header, "        ") + lines(chunk, "        ") + lines(bytes(std::string("\0Program 1\0", 11)), "        ");
}

struct FakeLocator : PluginLocator {
    std::string findClap(const std::string& id, const std::string&) override {
        return id == "com.twostorms.gainstage" || id == "in.lsp-plug.impulse_responses_mono" ? "/plugins/" + id + ".clap" : "";
    }
    std::string findVst3(const std::string& file) override { return "/vst3/" + file; }
};

std::string project() {
    const std::string gainState = "TWOSTORMS 0 1\np 54 0\n";
    return std::string(R"rpp(<REAPER_PROJECT 0.1 "7.82/linux-x86_64" 1792805934 0
  SAMPLERATE 48000 0 0
  <TRACK {D23E2D9F-8959-C083-4EE2-90E84B6F3B99}
    NAME Punk
    NCHAN 4
    REC 1 0 1 2 0 0 0 0
    MAINSEND 0 0
    HWOUT 1024 0 1 0 0 0 1026 -1:U -1
    HWOUT 1025 0 1 0 0 0 1024 -1:U -1
    <FXCHAIN
      SHOW 0
      BYPASS 0 0 0
      <CLAP "CLAP: TwoStorms Gain Stage (TwoStorms)" com.twostorms.gainstage ""
        <IN_PINS
        >
        <OUT_PINS
        >
        <STATE
)rpp") + lines(bytes(gainState), "          ") +
           R"rpp(        >
      >
      FXID {64BF3C86-12D3-ADD3-D5FE-8C87E323E207}
      BYPASS 0 0 0
      <VST "VST: ReaEQ (Cockos)" reaeq.vst.so 0 "" 1919247729<56535472656571726561657100000000> ""
        AAAA
      >
      BYPASS 1 0 0
      <VST "VST3: Darkglass Ultra (Neural DSP)" "Darkglass Ultra.vst3" 0 "" 602020598{ABCDEF019182FAEB4E44535044524B55} ""
)rpp" + vst3Body("component-state", "controller-state") +
           R"rpp(      >
      BYPASS 0 0 0
      <CLAP "CLAP: Impulse Responses Mono (LSP CLAP)" in.lsp-plug.impulse_responses_mono ""
        <OUT_PINS
          PIN 0 2 3
        >
      >
      BYPASS 0 0 0
      <CLAP "CLAP: TwoStorms Delay (TwoStorms)" com.twostorms.delay "Slapback"
      >
    >
  >
  <TRACK {5E86E2A7-C560-4219-59EE-CF1300F4B5F7}
    NAME Tuner
    <FXCHAIN
      BYPASS 0 0 0
      <CLAP "CLAP: TwoStorms Tuner (TwoStorms)" com.twostorms.tuner ""
      >
    >
  >
>
)rpp";
}

} // namespace

TEST(rpp_tokens_follow_reaper_quoting) {
    const auto t = rppTokens(R"rpp(<VST "VST3: Pro-Q 4 (FabFilter)" "FabFilter Pro-Q 4.vst3" 0 '' `y z`)rpp");
    CHECK(t.size() == 6 && t[1] == "VST3: Pro-Q 4 (FabFilter)" && t[2] == "FabFilter Pro-Q 4.vst3" && t[4].empty() && t[5] == "y z");
}

TEST(rpp_import_makes_chains_branches_and_states) {
    FakeLocator loc;
    Import imp;
    std::string err;
    CHECK_MSG(convertRpp(project(), "Cue Template", loc, "", imp, &err), "%s", err.c_str());
    CHECK(imp.chains == 1 && imp.plugins == 4 && imp.skipped == 1 && imp.missing == 1);
    // The config loads and has the routing REAPER had.
    Config c;
    const bool ok = parseConfigText(imp.config, "/out/Cue Template.ini", c, &err);
    CHECK_MSG(ok, "%s\n%s", err.c_str(), imp.config.c_str());
    if (!ok)
        return;
    CHECK(c.scenes.size() == 1 && c.scenes[0].name == "Cue Template" && c.scenes[0].chains.size() == 1);
    const ChainSpec& punk = c.scenes[0].chains[0];
    CHECK(punk.name == "Punk" && punk.lines.size() == 2);
    // Main line: gain stage, Darkglass, delay. The IR writes channels 3-4
    // only, so it is a branch, and out 1 (FOH) takes it.
    CHECK(punk.lines[0].plugins.size() == 3 && punk.lines[1].plugins.size() == 1);
    CHECK(punk.lines[1].name == "Out 1" && punk.lines[1].after == 2);
    CHECK(punk.out[0].line == 1 && punk.out[1].line == 0 && punk.out[1].after == 3);
    const PluginSpec& gain = punk.lines[0].plugins[0];
    CHECK(gain.kind == "clap" && gain.path == "/plugins/com.twostorms.gainstage.clap" && gain.label == "TwoStorms Gain Stage");
    CHECK(gain.params.size() == 1 && gain.params[0].first == 54 && gain.params[0].second == 1);
    const PluginSpec& ultra = punk.lines[0].plugins[1];
    CHECK(ultra.kind == "vst3" && ultra.id == "ABCDEF019182FAEB4E44535044524B55" && ultra.bypassed && ultra.label == "Darkglass Ultra");
    CHECK(ultra.path == "/vst3/Darkglass Ultra.vst3");
    const PluginSpec& delay = punk.lines[0].plugins[2];
    CHECK(delay.label == "Slapback"); // REAPER's own name for it
    CHECK(delay.path.find(".clap/TwoStorms Delay.clap") != std::string::npos); // not found: a guess
    CHECK(imp.config.find("NOT FOUND") != std::string::npos);
    CHECK(c.audio.inputChannel == 0 && c.mix == OutputMix::Left && c.midi.in == "M2");

    // State files: the CLAP state as saved, the VST3 split in two.
    auto file = [&](const std::string& ending) -> const ImportedFile* {
        for (const auto& f : imp.files)
            if (f.path.size() >= ending.size() && f.path.compare(f.path.size() - ending.size(), ending.size(), ending) == 0)
                return &f;
        return nullptr;
    };
    const ImportedFile* gs = file("01-twostorms-gain-stage.clapstate");
    CHECK(gs && std::string(gs->data.begin(), gs->data.end()) == "TWOSTORMS 0 1\np 54 0\n");
    const ImportedFile* comp = file("03-darkglass-ultra.vst3state");
    const ImportedFile* ctrl = file("03-darkglass-ultra.vst3ctrl");
    CHECK(comp && std::string(comp->data.begin(), comp->data.end()) == "component-state");
    CHECK(ctrl && std::string(ctrl->data.begin(), ctrl->data.end()) == "controller-state");
    CHECK(ultra.stateFile == "/out/" + comp->path && ultra.controllerStateFile == "/out/" + ctrl->path);

    // Notes say what was left out and why.
    bool reaEq = false, tuner = false;
    for (const auto& n : imp.notes) {
        reaEq = reaEq || (n.find("ReaEQ") != std::string::npos && n.find("VST2") != std::string::npos);
        tuner = tuner || n.find("track Tuner") != std::string::npos;
    }
    CHECK(reaEq && tuner);
}

TEST(rpp_import_includes_a_mapping_instead_of_presets) {
    FakeLocator loc;
    Import imp;
    std::string err;
    CHECK(convertRpp(project(), "Cue Template", loc, "/maps/template-mapping.ini", imp, &err));
    CHECK(imp.config.find("include = /maps/template-mapping.ini") != std::string::npos);
    CHECK(imp.config.find("[preset") == std::string::npos);
}

TEST(rpp_import_rejects_what_is_not_a_project) {
    FakeLocator loc;
    Import imp;
    std::string err;
    CHECK(!convertRpp("hello\n", "x", loc, "", imp, &err) && err == "not a REAPER project");
    CHECK(!convertRpp("<REAPER_PROJECT 0.1\n  <TRACK\n", "x", loc, "", imp, &err));
}
