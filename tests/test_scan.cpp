// Tests of the plug-in scan: every file opened by a helper process, one
// that crashes or hangs skipped and listed, what was found kept, and only
// changed files opened again.

#include "check.h"

#include "app/scan.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace cue;
namespace fs = std::filesystem;

namespace {

// The packaged test run points these at the bundled plug-ins and cue.
const char* kTestClap = std::getenv("CUE_TEST_CLAP") ? std::getenv("CUE_TEST_CLAP") : CUE_TEST_CLAP;
const char* kTestVst3 = std::getenv("CUE_TEST_VST3") ? std::getenv("CUE_TEST_VST3") : CUE_TEST_VST3;
const char* kCue = std::getenv("CUE_BIN") ? std::getenv("CUE_BIN") : CUE_BIN;

// A home of its own, so the usual folders hold only what a test puts there.
struct Home {
    std::string dir, old;
    explicit Home(const char* seconds = nullptr) {
        char name[] = "/tmp/cue-scan-XXXXXX";
        dir = mkdtemp(name) ? name : "/tmp/cue-scan";
        old = std::getenv("HOME") ? std::getenv("HOME") : "";
        setenv("HOME", dir.c_str(), 1);
        unsetenv("CLAP_PATH");
        unsetenv("VST3_PATH");
        if (seconds)
            setenv("CUE_SCAN_SECONDS", seconds, 1);
        fs::create_directories(dir + "/.clap");
        fs::create_directories(dir + "/.vst3");
    }
    ~Home() {
        setenv("HOME", old.c_str(), 1);
        unsetenv("CUE_SCAN_SECONDS");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    PluginScanner scanner() const { return PluginScanner({kCue, "scan-file"}, dir + "/cache.json", dir + "/settings.json"); }
};

bool finish(PluginScanner& s, double seconds = 30) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (s.progress().scanning) {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

const ScannedFile* fileNamed(const std::vector<ScannedFile>& files, const std::string& name) {
    for (const auto& f : files)
        if (fs::path(f.path).filename() == name)
            return &f;
    return nullptr;
}

const FoundPlugin* plugin(const ScannedFile* f, const std::string& id) {
    if (f)
        for (const auto& p : f->plugins)
            if (p.id == id)
                return &p;
    return nullptr;
}

} // namespace

TEST(scan_finds_the_plugins_in_the_usual_folders) {
    Home home;
    fs::copy_file(kTestClap, home.dir + "/.clap/Test.clap");
    fs::copy(kTestVst3, home.dir + "/.vst3/Cue Test.vst3", fs::copy_options::recursive);
    PluginScanner s = home.scanner();
    std::string why;
    CHECK_MSG(s.start("changed", &why), "%s", why.c_str());
    CHECK(finish(s));
    CHECK(s.takeFinished() && s.progress().done == 2 && s.progress().scannedAt > 0);
    const auto files = s.files();
    CHECK(files.size() == 2);
    const ScannedFile* clap = fileNamed(files, "Test.clap");
    CHECK(clap && clap->error.empty() && clap->format == "clap" && clap->plugins.size() == 6);
    const FoundPlugin* gain = plugin(clap, "cue.test.gain");
    CHECK(gain && gain->name == "Test Gain" && gain->vendor == "TwoStorms test" && gain->opened && gain->editor && !gain->sidechain);
    CHECK(gain && gain->categories.size() == 1 && gain->categories[0] == "audio-effect" && !gain->bridged);
    const FoundPlugin* key = plugin(clap, "cue.test.key");
    CHECK(key && key->opened && key->sidechain);
    const FoundPlugin* pedal = plugin(clap, "cue.test.pedal");
    CHECK(pedal && pedal->midi);
    const ScannedFile* vst3 = fileNamed(files, "Cue Test.vst3");
    CHECK(vst3 && vst3->error.empty() && vst3->plugins.size() == 1);
    CHECK(vst3 && vst3->plugins[0].name == "Cue Test Delay" && vst3->plugins[0].opened && vst3->plugins[0].id.size() == 32);
}

TEST(scan_skips_a_plugin_that_crashes_or_hangs) {
    Home home("1");
    for (const char* name : {"Fine.clap", "scan-crash-open.clap", "scan-hang-open.clap", "scan-crash-make.clap"})
        fs::copy_file(kTestClap, home.dir + "/.clap/" + name);
    PluginScanner s = home.scanner();
    std::string why;
    CHECK(s.start("changed", &why));
    CHECK(finish(s));
    const auto files = s.files();
    CHECK(files.size() == 4);
    const ScannedFile* fine = fileNamed(files, "Fine.clap");
    CHECK(fine && fine->error.empty() && fine->plugins.size() == 6);
    const ScannedFile* crash = fileNamed(files, "scan-crash-open.clap");
    CHECK_MSG(crash && crash->plugins.empty() && crash->error.find("crashed") == 0, "%s", crash ? crash->error.c_str() : "missing");
    const ScannedFile* hang = fileNamed(files, "scan-hang-open.clap");
    CHECK_MSG(hang && hang->plugins.empty() && hang->error.find("did not answer") == 0, "%s", hang ? hang->error.c_str() : "missing");
    // Listed, but each falls over as it is made: three are tried, the
    // rest are left out with them.
    const ScannedFile* make = fileNamed(files, "scan-crash-make.clap");
    CHECK(make && make->error.empty() && make->plugins.size() == 6);
    if (make && make->plugins.size() == 6) {
        CHECK(make->plugins[0].error.find("crashed") == 0 && make->plugins[2].error.find("when made") != std::string::npos);
        CHECK(make->plugins[3].error.find("not tried") == 0 && !make->plugins[5].opened);
    }
}

TEST(scan_opens_only_what_changed_and_keeps_what_it_found) {
    Home home;
    fs::copy_file(kTestClap, home.dir + "/.clap/A.clap");
    fs::copy_file(kTestClap, home.dir + "/.clap/B.clap");
    std::string why;
    {
        PluginScanner s = home.scanner();
        CHECK(s.start("changed", &why) && finish(s) && s.progress().done == 2);
    }
    // Kept for the next run: nothing to open again.
    PluginScanner s = home.scanner();
    CHECK(s.files().size() == 2 && s.progress().scannedAt > 0);
    CHECK(s.start("changed", &why) && finish(s));
    CHECK(s.progress().total == 0 && s.files().size() == 2);
    // A file that changed is opened again; one that went is forgotten.
    fs::last_write_time(home.dir + "/.clap/A.clap", fs::file_time_type::clock::now() + std::chrono::hours(1));
    fs::remove(home.dir + "/.clap/B.clap");
    CHECK(s.start("changed", &why) && finish(s));
    CHECK(s.progress().total == 1 && s.files().size() == 1 && fileNamed(s.files(), "A.clap"));
    // All of them, or one by its path.
    CHECK(s.start("all", &why) && finish(s) && s.progress().total == 1);
    CHECK(s.start(home.dir + "/.clap/A.clap", &why) && finish(s) && s.progress().total == 1);
    CHECK(!s.start(home.dir + "/.clap/Nope.clap", &why) && !why.empty());
}

TEST(scan_looks_in_the_users_folders_too) {
    Home home;
    fs::create_directories(home.dir + "/more/deep");
    fs::copy_file(kTestClap, home.dir + "/more/deep/Extra.clap");
    std::string why;
    {
        PluginScanner s = home.scanner();
        CHECK(!s.setExtraFolders({"relative/path"}, &why) && !why.empty());
        CHECK(s.setExtraFolders({home.dir + "/more/", home.dir + "/more"}, &why));
        CHECK(s.extraFolders().size() == 1 && s.extraFolders()[0] == home.dir + "/more");
    }
    PluginScanner s = home.scanner();
    CHECK(s.extraFolders().size() == 1);
    CHECK(s.start("changed", &why) && finish(s));
    CHECK(fileNamed(s.files(), "Extra.clap") != nullptr);
    // A scan running is not started twice, and stopping one ends it.
    fs::copy_file(kTestClap, home.dir + "/.clap/scan-hang-open.clap");
    CHECK(s.start("all", &why));
    CHECK(!s.start("all", &why) && why == "a scan is running");
    s.stop();
    CHECK(!s.progress().scanning);
}

TEST(scan_names_bridged_plugins) {
    CHECK(isBridged("/home/a/.vst3/yabridge/Neural DSP/Archetype.vst3"));
    CHECK(!isBridged(kTestClap));
    const auto folders = pluginFolders({"~/x"});
    CHECK(!folders.clap.empty() && !folders.vst3.empty() && folders.extra.size() == 1 && folders.extra[0][0] == '/');
}
