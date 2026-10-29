#pragma once

// Pedals between Cue and REAPER.
//
// Going to REAPER, a performance's pedals become one ReaLearn instance laid
// out the same way every time: ReaLearn parameter 1 is the playing chain
// (chain k is k/100, and every other chain's track is muted), and each
// discrete control is a parameter of its own (its value i is i/100). Pedals
// set those parameters, and the parameters switch plug-ins, light the
// FCB1010's LEDs and write its display. The performance's own mappings go
// along in the project's notes, so converting back gives them exactly.
//
// Coming from REAPER, the notes are read back, and ReaLearn mappings that
// are simple (a pedal or controller switching a plug-in or picking a track)
// become triggers. Mappings driven by ReaLearn parameters and formulas are
// listed, not converted.

#include "app/config.h"

#include <map>
#include <string>
#include <vector>

namespace cue {

// What the REAPER project holds for each chain and plug-in, by REAPER's GUIDs
// (upper case, without braces).
struct RppChain {
    std::string scene, name, trackGuid;
};
struct RppFx {
    std::string chain, label, fxGuid, trackGuid;
};

// The ReaLearn track for a performance's pedals, as RPP text. Empty when
// there is nothing for it to do.
std::string realearnTrack(const Config& config, const std::vector<RppChain>& chains, const std::vector<RppFx>& fx, std::vector<std::string>& notes);

// The performance's mappings, as the project's <NOTES block.
std::string cueMappingNotes(const Config& config);

// Read back from a project's notes.
struct CueMappings {
    struct Port {
        std::string name;
        int channel = 0; // 0-based
    };
    struct Chain {
        std::string name, display, warm;
        int pedal = 0, program = -1;
        std::vector<std::string> onSelect;
        std::vector<std::string> branches; // names of its lines after the main line
    };
    struct Scene {
        std::string name, display;
        int pedal = 0, program = -1;
        std::vector<Chain> chains;
    };
    bool found = false;
    std::vector<Port> inputs, outputs;
    std::vector<Scene> scenes;
    std::string ini; // [control], [trigger], [group] and [fcb] sections
};
// notesLines: the lines of the project's <NOTES block.
CueMappings readCueMappings(const std::vector<std::string>& notesLines);

// The [trigger] sections for a ReaLearn instance's simple mappings. json is
// its saved state; trackChain and fxPlugin name what the GUIDs are in Cue
// (fxPlugin as a mapping names a plug-in: "Punk/Wah:2"). Mappings this file
// wrote itself are skipped when skipOwn is set.
std::string realearnTriggers(const std::string& json, const std::map<std::string, std::string>& trackChain,
                             const std::map<std::string, std::string>& fxPlugin, bool skipOwn, std::vector<std::string>& notes);

// REAPER's VST2 chunk around a ReaLearn state, and back.
std::vector<uint8_t> realearnChunk(const std::string& json);
bool realearnJson(const std::vector<uint8_t>& body, std::string& json);

} // namespace cue
