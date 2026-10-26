#pragma once

// Reads a REAPER project (.rpp) and turns its tracks into Cue chains: one
// chain per track that plays to a hardware output, with each plug-in's
// settings saved as a state file. VST2, JS and other formats Cue does not
// host are left out and listed.

#include <cstdint>
#include <string>
#include <vector>

namespace cue {

// One "<NAME ..." block of a project, with its lines and blocks in order.
struct RppNode {
    std::vector<std::string> head; // the opening line's tokens, without '<'
    struct Item {
        std::string line; // a plain line, trimmed, or empty for a block
        int child = -1;   // index into children for a block
    };
    std::vector<Item> items;
    std::vector<RppNode> children;

    // The tokens of the first line starting with key, without the key.
    std::vector<std::string> value(const std::string& key) const;
    std::vector<std::vector<std::string>> values(const std::string& key) const;
    const RppNode* child(const std::string& name) const;
};

bool parseRpp(const std::string& text, RppNode& root, std::string* error);
// Splits a line as REAPER writes it: words, "double", 'single' or `back` quoted.
std::vector<std::string> rppTokens(const std::string& line);

// Where plug-ins live on this machine.
class PluginLocator {
public:
    virtual ~PluginLocator() = default;
    // A .clap file holding the plug-in id; name is REAPER's name for it. "" if not found.
    virtual std::string findClap(const std::string& id, const std::string& name) = 0;
    // A .vst3 bundle with this file name ("Darkglass Ultra.vst3"). "" if not found.
    virtual std::string findVst3(const std::string& file) = 0;
};

// Looks in REAPER's plug-in caches and paths, the usual CLAP and VST3
// folders and any extra folders, and as a last resort opens native .clap
// files (not yabridge ones) to read their ids.
class SystemLocator : public PluginLocator {
public:
    explicit SystemLocator(std::vector<std::string> extraDirs, std::string reaperDir = "~/.config/REAPER", bool scanClap = true);
    std::string findClap(const std::string& id, const std::string& name) override;
    std::string findVst3(const std::string& file) override;

private:
    void indexFiles();
    std::vector<std::string> clapDirs_, vst3Dirs_;
    std::vector<std::string> clapFiles_, vst3Bundles_; // every .clap file and .vst3 bundle found
    std::vector<std::pair<std::string, std::string>> clapIds_; // id, file (REAPER's cache, then scans)
    bool indexed_ = false, scanned_ = false, scanClap_;
};

struct ImportedFile {
    std::string path; // relative to the config
    std::vector<uint8_t> data;
};

struct Import {
    std::string config;              // the .ini text
    std::vector<ImportedFile> files; // plug-in states
    std::vector<std::string> notes;  // what was left out or guessed
    int chains = 0, plugins = 0, skipped = 0, missing = 0;
};

// mapping: a file to include after the chains instead of a generated preset
// per chain (its [preset] sections then belong to the imported scene).
bool convertRpp(const std::string& rppText, const std::string& projectName, PluginLocator& locator, const std::string& mapping, Import& out,
                std::string* error);

} // namespace cue
