#pragma once

// A .cue file: a whole performance in one file that opens on any machine. It
// is a zip holding performance.json (scenes, chains, plug-ins, mappings) and
// each plug-in's saved state under states/. What belongs to the machine (the
// sound card, which channel each input and output is, the MIDI ports, the
// buffer) is in that machine's rig profile instead. docs/cue-format.md
// describes both.

#include "app/config.h"

#include <string>
#include <vector>

namespace cue {

constexpr int kCueFormat = 1;

bool isCueFile(const std::string& path);

// Makes each chain selected by one preset of its own name, which is what a
// .cue holds: a chain carries its own pedal, display, actions and warm
// choice. A chain is renamed after the preset that selects it, chains follow
// their presets' order, and a chain no preset selects gets a preset of its
// own (with a warning). Fails for a chain that two presets select.
bool presetsToChains(Config& config, std::string* error);

// Writes the performance, with every plug-in state read from its state file
// (or carried from the .cue it was loaded from). The rig settings are not
// part of it. warnings: what presetsToChains changed.
bool saveCue(const Config& config, const std::string& path, std::string* error, std::vector<std::string>* warnings = nullptr);
// performance.json as saveCue would write it, without the states: what the
// app is told about a running performance.
bool performanceText(const Config& config, std::string& json, std::string* error);
// Reads a performance, leaving config's audio and MIDI port settings alone.
bool loadCue(const std::string& path, Config& config, std::string* error);

// The rig profile: this machine's sound card, buffer and threads, which
// channel each named input and output is, and the MIDI ports.
std::string defaultRigPath(); // $XDG_CONFIG_HOME/cue/rig.json, or ~/.config/cue/rig.json
bool saveRig(const Config& config, const std::string& path, std::string* error);
// Applies a rig profile to a loaded performance: the audio and MIDI port
// settings, and the channel of each of its inputs and outputs, found by name.
// One the rig does not name takes the channel of its position, with a warning.
bool loadRig(const std::string& path, Config& config, std::string* error);

// An INI config, or a .cue with a rig profile: rigPath, or the default one if
// it exists (without one, the defaults: the first input and outputs).
bool loadPerformance(const std::string& path, const std::string& rigPath, Config& config, std::string* error);

} // namespace cue
