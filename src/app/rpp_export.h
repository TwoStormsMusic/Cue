#pragma once

// Writes a performance as a REAPER project (.rpp), the other way from
// rpp.h: one track per chain, each line of the chain on its own pair of the
// track's channels, each output a hardware send from the line it is tapped
// on, and every plug-in with its saved state. A scene is a folder of tracks
// when there is more than one. What REAPER has no place for (warm chains,
// crossfades, Cue's built-in plug-ins) is left out and listed.

#include "app/config.h"

#include <string>
#include <vector>

namespace cue {

struct RppExport {
    std::string text;               // the project
    std::vector<std::string> notes; // what was left out or changed
    int tracks = 0, plugins = 0, skipped = 0;
};

// reaperDir: REAPER's settings folder, where its plug-in cache gives the
// number REAPER files a VST3 plug-in under. Without it the number is 0.
bool exportRpp(const Config& config, RppExport& out, std::string* error, const std::string& reaperDir = "~/.config/REAPER");

} // namespace cue
