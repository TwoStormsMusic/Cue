#pragma once

// Changes made to a performance while it plays: plug-ins added, removed and
// moved, wires drawn, cut and set. The performance, the engine's chain, the
// rig's list of plug-ins and the show change together, or not at all.

#include "app/build.h"
#include "app/config.h"
#include "control/show.h"
#include "host/editor_host.h"

#include <string>

namespace cue {

struct LiveRig {
    Config& config;
    Rig& rig;
    Show& show;
    EditorHost* editors = nullptr;
};

// A chain by the engine's number (its id; before the rig has numbered
// them, its place scene by scene), or nullptr.
ChainSpec* chainSpec(Config& config, int chain);

// Main thread. Each returns false with why, and then nothing has changed.

// A plug-in, loaded and added to a chain. on: the wire it goes on, which
// then runs through it (the wire's gains stay with the part that arrives
// where it did); without one it is not wired. slot: where it went.
bool addPlugin(LiveRig& live, int chain, PluginSpec spec, const WireSpec* on, size_t* slot, std::string* error);
// What a removal took out, so it can be put back as it was (an undo).
struct Removed {
    int chain = -1;
    size_t slot = 0;
    PluginSpec spec;             // with its state as it was taken out
    std::vector<WireSpec> wires; // the chain's wires with it there
    size_t blocks = 0;           // how many the chain had with it
};

// A plug-in and its wires go. What fed it is wired to what it fed, so one
// taken out of a line closes the gap. Refused while the mappings use it.
// removed, if given, keeps what it takes out.
bool removePlugin(LiveRig& live, int chain, size_t slot, Removed* removed, std::string* error);
// The plug-in back where it was, with its state and the chain's wires as
// they were. Refused if the chain has gained or lost a plug-in since.
bool restorePlugin(LiveRig& live, const Removed& removed, std::string* error);
// A wire between two ports, or new gains for the one already there (which
// move with a short fade rather than a swap).
bool setWire(LiveRig& live, int chain, const WireSpec& wire, std::string* error);
bool removeWire(LiveRig& live, int chain, Port from, Port to, std::string* error);
// A chain's wires all at once: what an undo puts back.
bool setWires(LiveRig& live, int chain, const std::vector<WireSpec>& wires, std::string* error);
// Where the routing board draws a plug-in.
bool placePlugin(LiveRig& live, int chain, size_t slot, double x, double y, std::string* error);

} // namespace cue
