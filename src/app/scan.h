#pragma once

// The plug-ins on this machine. Every .clap file and .vst3 bundle in the
// usual folders, and in any the user adds, is opened by a helper process of
// its own (`cue scan-file`), one at a time, so a plug-in that hangs or
// crashes is skipped and listed rather than taking Cue down. What was found
// is kept between runs, and a file is opened again only when it changes or
// a rescan asks for it.
//
// The scan runs on a thread of its own and in the helper, never on the
// audio path; the helper is started without copying the engine's memory
// (posix_spawn), so the audio thread does not notice it.

#include "app/service.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cue {

struct FoundPlugin {
    std::string format; // "clap" or "vst3"
    std::string path;   // the .clap file or .vst3 bundle
    std::string id;     // the CLAP id, or the VST3 class id
    std::string name, vendor, version;
    std::vector<std::string> categories; // CLAP features, VST3 subcategories
    bool bridged = false;                // a Windows plug-in, through yabridge
    // Made once by the scan (not for bridged ones: that would start Wine for
    // each), so these are known; error says why it could not be made.
    bool opened = false;
    bool sidechain = false, editor = false, midi = false;
    std::string error;
};

struct ScannedFile {
    std::string path, format;
    int64_t mtime = 0, size = 0; // of the file, or of a bundle's Linux binary, when scanned
    std::string error;           // why it was skipped; empty when it was read
    std::vector<FoundPlugin> plugins;
};

Json toJson(const FoundPlugin& p);
Json toJson(const ScannedFile& f);
ScannedFile scannedFileOf(const Json& j);

// The usual folders (with CLAP_PATH and VST3_PATH), and the user's.
struct PluginFolders {
    std::vector<std::string> clap, vst3;
    std::vector<std::string> extra; // searched for both
};
PluginFolders pluginFolders(const std::vector<std::string>& extra);
// Every .clap file and .vst3 bundle in them: path, format.
std::vector<std::pair<std::string, std::string>> pluginFiles(const PluginFolders& folders);
// When a file last changed, and its size: a bundle's Linux binary's.
std::pair<int64_t, int64_t> fileStamp(const std::string& path);
// A bridged Windows plug-in: in a yabridge folder, or a yabridge chainloader.
bool isBridged(const std::string& path);

// The helper's work: opens one file, lists its plug-ins, then makes each
// once to learn whether it has a side-chain input, an editor and a MIDI
// input. Writes a line of JSON to fd for the list and one for each plug-in
// made, so what was learnt survives a plug-in that crashes or hangs; first:
// where to start making them, after one that fell over.
int scanFileMain(const std::string& path, const std::string& format, int fd, size_t first = 0);

class PluginScanner {
public:
    // helper: the command that runs scanFileMain ("cue scan-file", by path);
    // cache: where the results are kept; settings: the user's folders.
    PluginScanner(std::vector<std::string> helper, std::string cache, std::string settings);
    ~PluginScanner();

    // $XDG_CACHE_HOME/cue/plugins.json and $XDG_CONFIG_HOME/cue/plugins.json.
    static std::string defaultCache();
    static std::string defaultSettings();

    std::vector<std::string> extraFolders() const;
    bool setExtraFolders(std::vector<std::string> folders, std::string* error);
    PluginFolders folders() const { return pluginFolders(extraFolders()); }

    // Starts scanning in the background: new and changed files ("changed"),
    // every file ("all"), or one file by its path. False if a scan runs.
    bool start(const std::string& what, std::string* error);
    void stop();

    struct Progress {
        bool scanning = false;
        size_t done = 0, total = 0;
        std::string current;
        int64_t scannedAt = 0; // when the last scan ended, seconds since 1970
    };
    Progress progress() const;
    std::vector<ScannedFile> files() const;
    // Main thread: true once after a file was scanned, and once after a scan ended.
    bool takeProgressed();
    bool takeFinished();

    // How long a helper may stay silent before it counts as hung, in ms:
    // CUE_SCAN_SECONDS, else 30 s, 120 s for bridged plug-ins (Wine starts).
    static int silenceLimitMs(bool bridged);

private:
    void run(std::vector<std::pair<std::string, std::string>> todo);
    ScannedFile scanOne(const std::string& path, const std::string& format);
    // One run of the helper, making plug-ins from first on. Returns the
    // plug-in it fell over making (-1: none), and why in failure.
    int runHelper(ScannedFile& f, size_t first, bool bridged, std::string& failure);
    void save();

    std::vector<std::string> helper_;
    std::string cache_, settings_;
    mutable std::mutex mutex_;
    std::vector<ScannedFile> files_;
    std::vector<std::string> extra_;
    Progress progress_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, progressed_{false}, finished_{false};
};

} // namespace cue
