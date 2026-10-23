#pragma once

#include "audio/alsa_backend.h"
#include "core/engine.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sh {

struct PluginSpec {
    std::string kind;              // "clap", "vst3" or "builtin"
    std::string path;              // the .clap file or .vst3 bundle
    std::string id;                // CLAP id, or VST3 effect name, inside it; empty = first
    std::vector<std::string> args; // builtin: name then numbers
    std::vector<std::pair<uint32_t, double>> params;
    std::string stateFile;         // raw plug-in state to load after activation
    std::string text;              // as written, for messages
};

struct SceneSpec {
    std::string name;
    int pedal = 0;    // FCB pedal (1-10) that selects it; 0 = none
    int program = -1; // MIDI program change that selects it; -1 = none
    std::vector<PluginSpec> chain;
    size_t out1After = SIZE_MAX; // SIZE_MAX = end of chain
    size_t out2After = SIZE_MAX;
};

struct MidiSettings {
    bool enabled = true;
    std::string in = "any";    // sequencer port name match for input, or "any"
    std::string out;           // match for output (LEDs); empty = same as in unless "any"
    std::string profile = "eurekaprom";
    uint8_t channel = 0;       // 0-based channel used for output
};

struct Config {
    AlsaConfig audio;
    bool cpuAuto = true;
    OutputMix mix = OutputMix::Sum;
    EngineSettings engine;
    std::string warm = "next"; // "none" or "next"
    MidiSettings midi;
    std::vector<SceneSpec> scenes;
};

// Parses the INI-style config described in configs/example.ini.
bool loadConfig(const std::string& path, Config& config, std::string* error);

} // namespace sh
