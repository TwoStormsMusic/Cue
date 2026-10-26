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

// A chain by the engine's number (scene by scene), or nullptr.
ChainSpec* chainSpec(Config& config, int chain);

// Main thread. Each returns false with why, and then nothing has changed.

// A plug-in, loaded and added to a chain. on: the wire it goes on, which
// then runs through it (the wire's gains stay with the part that arrives
// where it did); without one it is not wired. slot: where it went.
bool addPlugin(LiveRig& live, int chain, PluginSpec spec, const WireSpec* on, size_t* slot, std::string* error);
// A plug-in and its wires go. What fed it is wired to what it fed, so one
// taken out of a line closes the gap. Refused while the mappings use it.
bool removePlugin(LiveRig& live, int chain, size_t slot, std::string* error);
// A wire between two ports, or new gains for the one already there (which
// move with a short fade rather than a swap).
bool setWire(LiveRig& live, int chain, const WireSpec& wire, std::string* error);
bool removeWire(LiveRig& live, int chain, Port from, Port to, std::string* error);
// A chain's wires all at once: what an undo puts back.
bool setWires(LiveRig& live, int chain, const std::vector<WireSpec>& wires, std::string* error);
// Where the routing board draws a plug-in.
bool placePlugin(LiveRig& live, int chain, size_t slot, double x, double y, std::string* error);

} // namespace cue
