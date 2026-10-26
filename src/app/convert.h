#pragma once

// Converting whole files between REAPER and Cue, as the converter app and
// `cue export-rpp` do: a REAPER project (.rpp) to a .cue performance, and a
// .cue performance (or an INI config) to a REAPER project.

#include <string>
#include <vector>

namespace cue {

struct ConvertOptions {
    std::string rig;                 // rig profile for a .cue's channels; empty: the default one if it exists
    std::vector<std::string> search; // more folders to look for plug-ins in
    bool scanClap = true;            // open native .clap files to find ids REAPER's cache lacks
    std::string reaperDir = "~/.config/REAPER";
};

struct ConvertResult {
    std::string out;                 // the file written
    std::vector<std::string> notes;  // what was left out, guessed or changed
    int chains = 0, plugins = 0, skipped = 0, missing = 0;
};

// The file another would convert to: Song.RPP -> Song.cue, Song.cue -> Song.RPP.
std::string convertedName(const std::string& in);
bool isRppFile(const std::string& path);

// Converts in to out, keeping an existing out as out.bak. Without out, to
// convertedName(in), or "Song (2).RPP" and so on when that file exists.
bool convertFile(const std::string& in, const std::string& out, const ConvertOptions& options, ConvertResult& result, std::string* error);

} // namespace cue
