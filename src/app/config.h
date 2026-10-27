#pragma once

#include "audio/alsa_backend.h"
#include "core/engine.h"
#include "tuner/tuner.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cue {

// The most inputs, and the most outputs, a performance may have.
constexpr size_t kMaxPorts = 64;

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
    // A .cue file carries the states themselves; stateFile then only names
    // where they came from, for messages.
    bool embeddedState = false;
    std::vector<uint8_t> state, controllerState;
    std::string label;               // `name =`: what mappings call it
    bool bypassed = false;           // loaded, but not processed until a mapping enables it
    bool muteOnBypass = false;       // bypass_mode: when bypassed, pass silence (mute) instead of the input (thru)
    bool bypassModeSet = false;      // false: [switching] bypass_mode decides
    std::string sidechain;           // `sidechain =`: the earlier plug-in whose output feeds its side-chain input
    bool midi = true;                // receives MIDI, if it has a MIDI input
    // The parameters the app shows for it, by id in the app's order; unset
    // until they are picked there.
    std::vector<uint32_t> shown;
    bool shownSet = false;
    // Where the routing board draws it, once it has been put somewhere.
    bool placed = false;
    double x = 0, y = 0;
    std::string text, where;         // as written, for messages
};

// A wire of a chain: from the chain's input or a block, to a block (its
// input or its key) or an output, with what it does to the signal. Blocks
// are numbered from 0 in the order of the chain's blocks; outputs are the
// performance's, in its order.
struct WireSpec {
    Port from, to;
    WireGains gains;
    std::string where;
    bool feedback = false; // heard a block late, so it may close a loop (see Wire)
    bool jump = false;     // only how the board draws it: out at its source, in at its end, by a letter
    // ...and where its two letters are, from the ports, in board units: the app's to say (empty: where it puts them).
    std::vector<double> jumpOut, jumpIn;
};

// A line of plug-ins in series, as an INI config and a version 1 .cue file
// describe a chain. The chain's first line is its main line; the others are
// branches, each taking a copy of an earlier line's signal.
struct LineSpec {
    std::string name;  // empty for the main line
    int parent = -1;   // line it comes off
    size_t after = 0;  // after this many of the parent's plug-ins
    std::vector<PluginSpec> plugins;
};

// Where a chain's signal is taken for one output.
struct TapSpec {
    std::string output;      // the output: its name, or out1, out2...
    int line = 0;
    size_t after = SIZE_MAX; // SIZE_MAX = end of the line
    bool set = false;
    std::string where;
};

struct ChainSpec {
    std::string name;
    // The engine's number for it while it plays, which stays the same when
    // chains are added, moved or taken out: the rig numbers the chains it
    // loads scene by scene, in order, and one added later takes a spare.
    // -1 until then.
    int id = -1;
    // The performance input that feeds it: its name, or in1, in2...; empty
    // = the first. finishConfig finds inputIndex.
    std::string input;
    size_t inputIndex = 0;
    // Its plug-ins, in the order of the engine's slots, and the wires
    // between them, its input and the performance's outputs.
    std::vector<PluginSpec> blocks;
    std::vector<WireSpec> wires;
    // As an INI config or a version 1 .cue file describes it: lines, and
    // where each output is taken (an output no tap names takes the end of
    // the main line). finishConfig makes blocks and wires of them, and
    // leaves both empty.
    std::vector<LineSpec> lines;
    std::vector<TapSpec> out;
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
    // Which chain is kept warm while it plays: "next", "none" or a chain of
    // its scene. Empty: [switching] warm decides.
    std::string warm;
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
// off, moves their parameters, mutes the inputs and lights LEDs.
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
    // LED, condition. LEDs 1-10 are the pedals'; 11-23 are EurekaPROM's
    // other LEDs by their own numbers (Expr A 11 ... Value 1 18, Value 2 19 ... Config 23).
    std::vector<std::pair<int, ConfigText>> leds;
    std::vector<ConfigText> displays;             // text [when condition], first match wins
};

// The built-in tuner: the input it listens to, and its settings.
struct TunerSpec {
    std::string input;     // by name or as in1, in2...; empty = the first
    size_t inputIndex = 0; // found by finishConfig
    TunerSettings settings;
    std::string where;
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
    // What the performance calls its inputs and outputs. A machine's rig
    // profile says which sound card channel each one is (audio.inputChannels
    // and outputChannels, in the same order).
    std::vector<std::string> inputs = {"In 1"};
    std::vector<std::string> outputs = {"Out 1", "Out 2"};
    std::vector<SceneSpec> scenes;
    std::vector<ControlSpec> controls;
    std::vector<TriggerSpec> triggers;
    std::vector<GroupSpec> groups;
    FcbSpec fcb;
    TunerSpec tuner;
    std::string path;          // the file that was loaded
    std::vector<std::string> warnings; // things that load but are probably mistakes
};

// Parses the INI-style config described in docs/config.md.
bool loadConfig(const std::string& path, Config& config, std::string* error);
bool parseConfigText(const std::string& text, const std::string& path, Config& config, std::string* error);
// What every loaded performance goes through: a channel for each input and
// output, presets for scenes without any, presets matched to chains, chains'
// inputs found and outputs tapped, controls completed.
bool finishConfig(Config& config, std::string* error);

// A plug-in as a config's plugin line names it: "clap:/path/file.clap#id",
// "vst3:/path/file.vst3#name" or "builtin:gain 0.5".
bool pluginFromText(const std::string& text, PluginSpec& spec, std::string* error);

// Every plug-in of a chain: the order of the engine's slots.
std::vector<const PluginSpec*> chainPlugins(const ChainSpec& chain);

// The side-chain feeds a chain's plug-ins name with `sidechain =`, as
// (slot, source slot) pairs, given each slot's label. `sidechain = Gate` is
// the nearest Gate before the plug-in; `Gate:2` is the chain's second Gate,
// which must come before it.
bool chainSidechains(const ChainSpec& chain, const std::vector<std::string>& labels, std::vector<std::pair<size_t, size_t>>& feeds,
                     std::string* error);

// A chain's wires in short, for messages and tests: "in>0 0>1 1>FOH 0>2:key",
// blocks by number, outputs by name, then any gains as "(-6dB w50 p-20)".
std::string wireList(const ChainSpec& chain, const std::vector<std::string>& outputs);

std::string expandPath(std::string path);

} // namespace cue
