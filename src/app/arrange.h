#pragma once

// Chains and scenes added, copied, moved, set and taken out while the
// performance plays. A chain keeps the engine's number for it (its id)
// whatever happens around it, so what the app holds on to stays good. As
// with plug-ins, the performance, the engine, the rig and the show change
// together, or not at all. Each needs the performance's chains and presets
// one to one (as a .cue file has them): see arranged().

#include "app/edit.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cue {

// Whether every scene's presets are its chains', one each, in order.
bool arranged(const Config& config);
// Where a chain is: its scene and its place there. False if there is no
// chain with that id.
bool findChain(const Config& config, int id, int& scene, int& at);

// Main thread. Each returns false with why, and then nothing has changed.

struct NewChain {
    int scene = 0;
    int at = -1;       // its place in the scene; -1: the end
    std::string name;  // empty: the copied chain's, or "Chain N"
    // A chain to copy (by id): its plug-ins with their states as they are
    // now, its wires and its settings. A pedal is kept only in another
    // scene where it is free, a program change never. -1: an empty chain,
    // its input wired to every output.
    int copy = -1;
    std::string input; // an empty chain's input, by name; empty: the first
};
bool addChain(LiveRig& live, const NewChain& what, int* id, std::string* error);

// What taking a chain out took, so it can go back as it was.
struct RemovedChain {
    int id = -1;
    int scene = 0, at = 0;
    ChainSpec chain; // with its plug-ins' states as they were
    PresetSpec preset;
    std::vector<std::string> warmFor; // the chains that kept it warm (they keep the next one now)
};
// Refused for the chain playing, the last of its scene, or one the
// mappings name.
bool removeChain(LiveRig& live, int id, RemovedChain* removed, std::string* error);
// Back where it was, under its id if no chain has taken it since.
bool restoreChain(LiveRig& live, const RemovedChain& removed, int* id, std::string* error);

// What changes: only what is given. A pedal or program change given to one
// chain is taken from any other that had it (pedals within the scene).
struct ChainSettings {
    std::optional<std::string> name, display, warm; // warm: next, none or a chain of the scene
    std::optional<int> pedal, program;               // 0 / -1: none
    std::optional<std::vector<std::string>> onSelect;
};
bool setChain(LiveRig& live, int id, const ChainSettings& settings, std::string* error);
// To another place in its scene, or into another scene (at: the place
// there, -1 the end). Refused if it is the last of its scene.
bool moveChain(LiveRig& live, int id, int scene, int at, std::string* error);

struct NewScene {
    int at = -1;      // -1: the end
    std::string name; // empty: "Scene N"
    // A scene to copy: every chain as addChain copies one (pedals kept),
    // and the triggers that work only in it. -1: one empty chain.
    int copy = -1;
};
bool addScene(LiveRig& live, const NewScene& what, int* index, std::vector<int>* ids, std::string* error);

struct RemovedScene {
    int at = 0;
    SceneSpec scene; // with every plug-in's state as it was
    std::vector<std::pair<size_t, TriggerSpec>> triggers; // the ones only for it, by their places
};
// Every chain of it goes, and its triggers. Refused for the scene playing
// or the only one.
bool removeScene(LiveRig& live, int scene, RemovedScene* removed, std::string* error);
bool restoreScene(LiveRig& live, const RemovedScene& removed, std::string* error);

struct SceneSettings {
    std::optional<std::string> name, display;
    std::optional<int> pedal, program; // taken from any other scene that had it
};
bool setScene(LiveRig& live, int scene, const SceneSettings& settings, std::string* error);
bool moveScene(LiveRig& live, int scene, int at, std::string* error);

} // namespace cue
