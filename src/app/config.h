#pragma once

#include "audio/alsa_backend.h"
#include "core/engine.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cue {

// A value from the config with the place it came from, for error messages.
struct ConfigText {
    std::string text;
    std::string where; // "file:line"
};

struct PluginSpec {
    std::string kind;              // "clap", "vst3" or "builtin"
    std::string path;              // the .clap file or .vst3 bundle
    std::string id;                // CLAP id, or VST3 effect name or class ID, inside it; empty = first
    std::vector<std::string> args; // builtin: name then numbers
    std::vector<std::pair<uint32_t, double>> params;
    std::string stateFile;           // plug-in state to load after activation
    std::string controllerStateFile; // VST3 only: the edit controller's state
    std::string label;               // `name =`: what mappings call it
    bool bypassed = false;           // loaded, but not processed until a mapping enables it
    bool muteOnBypass = false;       // bypass_mode: when bypassed, pass silence (mute) instead of the input (thru)
    bool bypassModeSet = false;      // false: [switching] bypass_mode decides
    std::string sidechain;           // `sidechain =`: the earlier plug-in whose output feeds its side-chain input
    bool midi = true;                // receives MIDI, if it has a MIDI input
    std::string text, where;         // as written, for messages
};

// A line of plug-ins in series. The chain's first line is its main line;
// the others are branches, each taking a copy of an earlier line's signal.
struct LineSpec {
    std::string name;  // empty for the main line
    int parent = -1;   // line it comes off
    size_t after = 0;  // after this many of the parent's plug-ins
    std::vector<PluginSpec> plugins;
};

struct TapSpec {
    int line = 0;
    size_t after = SIZE_MAX; // SIZE_MAX = end of the line
    bool set = false;
};

struct ChainSpec {
    std::string name;
    std::vector<LineSpec> lines;
    TapSpec out[2];
    std::string where;
};

// A preset: which chain plays, and what happens when it is selected.
struct PresetSpec {
    std::string name;
    std::string chain;   // empty: the scene's only chain, or the chain of the same name
    std::string display; // up to 3 characters for the FCB's display; empty: from the name
    int pedal = 0;       // FCB pedal (1-10) that selects it
    int program = -1;    // MIDI program change that selects it
    std::vector<ConfigText> actions;
    std::string where;
};

// A scene: one routing grid of chains, and its presets.
struct SceneSpec {
    std::string name;
    std::string display;
    int pedal = 0;
    int program = -1;
    std::vector<ChainSpec> chains;
    std::vector<PresetSpec> presets;
    std::string where;
};

// A named value that pedals and MIDI move, and that switches plug-ins on and
// off, moves their parameters, mutes the outputs and lights LEDs.
struct ControlSpec {
    enum class Type { Toggle, Discrete, Continuous };
    std::string name;
    Type type = Type::Discrete;
    double min = 0, max = 1, step = 1, def = 0;
    std::string defText;              // default given as one of the values' names
    bool rangeSet = false, stepSet = false, defSet = false, typeSet = false;
    bool wrap = true;                 // stepping past the end comes back to the start
    std::vector<std::string> labels;  // names of the values, from min up
    int pedal = 0;                    // FCB pedal that steps it, with its LED lit when it is not at its minimum
    ConfigText follows;               // an input that sets it in proportion, like "expression 1" or "cc 7"
    std::vector<ConfigText> enables, drives, mutes;
    std::string where;
};

// When an input arrives, do every action in order.
struct TriggerSpec {
    std::string name;
    std::string scene;               // only in this scene; empty = everywhere
    ConfigText when;                 // only while this condition holds
    std::vector<ConfigText> inputs;
    std::vector<ConfigText> actions;
    std::string where;
};

// Actions with a name, run together by `do = group <name>`.
struct GroupSpec {
    std::string name;
    std::vector<ConfigText> actions;
    std::string where;
};

// What the FCB's LEDs and display show (EurekaPROM profile).
struct FcbSpec {
    std::vector<std::pair<int, ConfigText>> leds; // pedal, condition
    std::vector<ConfigText> displays;             // text [when condition], first match wins
};

struct MidiSettings {
    bool enabled = true;
    std::string in = "any";    // sequencer port name match for input, or "any"
    std::string out;           // match for output (LEDs); empty = same as in unless "any"
    std::string profile = "eurekaprom";
    uint8_t channel = 0;       // 0-based channel used for output
    bool forward = true;       // pass incoming MIDI on to plug-ins that take MIDI
};

struct Config {
    AlsaConfig audio;
    bool cpuAuto = true;
    int workers = 2;           // helper threads that process playing chains alongside the audio thread
    OutputMix mix = OutputMix::Sum;
    EngineSettings engine;
    std::string warm = "next"; // "none" or "next"
    bool muteOnBypass = false; // [switching] bypass_mode: what plug-ins without their own do
    MidiSettings midi;
    std::string performance;   // the performance's name
    std::vector<SceneSpec> scenes;
    std::vector<ControlSpec> controls;
    std::vector<TriggerSpec> triggers;
    std::vector<GroupSpec> groups;
    FcbSpec fcb;
    std::string path;          // the file that was loaded
    std::vector<std::string> warnings; // things that load but are probably mistakes
};

// Parses the INI-style config described in docs/config.md.
bool loadConfig(const std::string& path, Config& config, std::string* error);
bool parseConfigText(const std::string& text, const std::string& path, Config& config, std::string* error);

// Every plug-in of a chain, line by line: the order of the engine's slots.
std::vector<const PluginSpec*> chainPlugins(const ChainSpec& chain);

// The side-chain feeds of a chain as (slot, source slot) pairs, given each
// slot's label. `sidechain = Gate` is the nearest Gate before the plug-in;
// `Gate:2` is the chain's second Gate, which must come before it.
bool chainSidechains(const ChainSpec& chain, const std::vector<std::string>& labels, std::vector<std::pair<size_t, size_t>>& feeds,
                     std::string* error);

std::string expandPath(std::string path);

} // namespace cue
