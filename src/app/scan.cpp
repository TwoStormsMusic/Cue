#include "app/scan.h"

#include "app/config.h"
#include "host/clap_processor.h"
#include "host/vst3_processor.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

extern char** environ;

namespace fs = std::filesystem;

namespace cue {

namespace {

std::string lowered(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& end) { return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0; }

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

std::string xdg(const char* var, const char* fallback) {
    const char* v = std::getenv(var);
    return v && *v ? std::string(v) : home() + fallback;
}

std::vector<std::string> splitPath(const char* value) {
    std::vector<std::string> out;
    std::stringstream ss(value ? value : "");
    for (std::string d; std::getline(ss, d, ':');)
        if (!d.empty())
            out.push_back(d);
    return out;
}

// A bundle's Linux binaries; a .clap is its own.
std::vector<fs::path> binaries(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec))
        return {fs::path(path)};
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(fs::path(path) / "Contents" / "x86_64-linux", ec))
        if (e.path().extension() == ".so")
            out.push_back(e.path());
    return out;
}

bool writeAll(int fd, const std::string& text) {
    size_t at = 0;
    while (at < text.size()) {
        const ssize_t n = ::write(fd, text.data() + at, text.size() - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        at += static_cast<size_t>(n);
    }
    return true;
}

bool writeLine(int fd, const Json& j) { return writeAll(fd, j.dump() + "\n"); }

std::vector<std::string> splitBars(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string part; std::getline(ss, part, '|');)
        if (!part.empty())
            out.push_back(part);
    return out;
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "a segmentation fault";
    case SIGABRT: return "an abort";
    case SIGBUS: return "a bus error";
    case SIGILL: return "an illegal instruction";
    case SIGFPE: return "a floating-point error";
    case SIGKILL: return "being killed";
    default: return nullptr;
    }
}

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

} // namespace

Json toJson(const FoundPlugin& p) {
    Json j = {{"format", p.format}, {"path", p.path}, {"id", p.id}, {"name", p.name}, {"vendor", p.vendor}, {"version", p.version}};
    j["categories"] = p.categories;
    if (p.bridged)
        j["bridged"] = true;
    if (p.opened)
        j["opened"] = true, j["sidechain"] = p.sidechain, j["editor"] = p.editor, j["midi"] = p.midi;
    if (!p.error.empty())
        j["error"] = p.error;
    return j;
}

Json toJson(const ScannedFile& f) {
    Json j = {{"path", f.path}, {"format", f.format}, {"mtime", f.mtime}, {"size", f.size}};
    if (!f.error.empty())
        j["error"] = f.error;
    Json list = Json::array();
    for (const FoundPlugin& p : f.plugins)
        list.push_back(toJson(p));
    j["plugins"] = list;
    return j;
}

namespace {

FoundPlugin foundOf(const Json& j) {
    FoundPlugin p;
    p.format = j.value("format", "");
    p.path = j.value("path", "");
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.vendor = j.value("vendor", "");
    p.version = j.value("version", "");
    if (j.contains("categories") && j.at("categories").is_array())
        for (const auto& c : j.at("categories"))
            if (c.is_string())
                p.categories.push_back(c.get<std::string>());
    p.bridged = j.value("bridged", false);
    p.opened = j.value("opened", false);
    p.sidechain = j.value("sidechain", false);
    p.editor = j.value("editor", false);
    p.midi = j.value("midi", false);
    p.error = j.value("error", "");
    return p;
}

} // namespace

ScannedFile scannedFileOf(const Json& j) {
    ScannedFile f;
    f.path = j.value("path", "");
    f.format = j.value("format", "");
    f.mtime = j.value("mtime", int64_t(0));
    f.size = j.value("size", int64_t(0));
    f.error = j.value("error", "");
    if (j.contains("plugins") && j.at("plugins").is_array())
        for (const auto& p : j.at("plugins"))
            f.plugins.push_back(foundOf(p));
    return f;
}

PluginFolders pluginFolders(const std::vector<std::string>& extra) {
    PluginFolders f;
    for (const char* d : {"~/.clap", "/usr/lib/clap", "/usr/local/lib/clap"})
        f.clap.push_back(expandPath(d));
    for (const auto& d : splitPath(std::getenv("CLAP_PATH")))
        f.clap.push_back(d);
    for (const char* d : {"~/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3"})
        f.vst3.push_back(expandPath(d));
    for (const auto& d : splitPath(std::getenv("VST3_PATH")))
        f.vst3.push_back(d);
    for (const auto& d : extra)
        f.extra.push_back(expandPath(d));
    return f;
}

std::vector<std::pair<std::string, std::string>> pluginFiles(const PluginFolders& folders) {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::string> seen;
    auto walk = [&](const std::string& dir, bool clap, bool vst3) {
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 6) {
                it.disable_recursion_pending();
                continue;
            }
            const fs::path& p = it->path();
            const std::string name = lowered(p.filename().string());
            if (endsWith(name, ".vst3")) {
                it.disable_recursion_pending(); // a bundle: not inside it
                if (vst3 && seen.insert(p.string()).second)
                    out.push_back({p.string(), "vst3"});
            } else if (clap && endsWith(name, ".clap") && !it->is_directory(ec) && seen.insert(p.string()).second) {
                out.push_back({p.string(), "clap"});
            }
        }
    };
    for (const auto& d : folders.clap)
        walk(d, true, false);
    for (const auto& d : folders.vst3)
        walk(d, false, true);
    for (const auto& d : folders.extra)
        walk(d, true, true);
    std::sort(out.begin(), out.end());
    return out;
}

std::pair<int64_t, int64_t> fileStamp(const std::string& path) {
    int64_t mtime = 0, size = 0;
    for (const fs::path& b : binaries(path)) {
        struct stat st {};
        if (::stat(b.c_str(), &st) != 0)
            continue;
        mtime = std::max<int64_t>(mtime, st.st_mtim.tv_sec);
        size += st.st_size;
    }
    return {mtime, size};
}

bool isBridged(const std::string& path) {
    if (lowered(path).find("yabridge") != std::string::npos)
        return true;
    // A yabridge chainloader is small and names its library.
    for (const fs::path& b : binaries(path)) {
        std::error_code ec;
        if (fs::file_size(b, ec) > (8u << 20) || ec)
            continue;
        std::ifstream f(b, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.find("libyabridge") != std::string::npos)
            return true;
    }
    return false;
}

int scanFileMain(const std::string& path, const std::string& format, int fd, size_t first) {
    // Nothing of the engine's but the line to it.
    if (::syscall(SYS_close_range, fd + 1, ~0u, 0) != 0)
        for (int i = fd + 1; i < 4096; ++i)
            ::close(i);
    const bool bridged = isBridged(path);
    std::vector<FoundPlugin> found;
    std::string err;
    if (format == "clap") {
        for (const auto& c : scanClapFile(path, &err)) {
            FoundPlugin p;
            p.format = "clap";
            p.path = path;
            p.id = c.id;
            p.name = c.name;
            p.vendor = c.vendor;
            p.version = c.version;
            p.categories = c.features;
            p.bridged = bridged;
            found.push_back(std::move(p));
        }
    } else {
        for (const auto& v : scanVst3(path, &err)) {
            FoundPlugin p;
            p.format = "vst3";
            p.path = path;
            p.id = v.uid;
            p.name = v.name;
            p.vendor = v.vendor;
            p.version = v.version;
            p.categories = splitBars(v.category);
            p.bridged = bridged;
            found.push_back(std::move(p));
        }
    }
    if (found.empty()) {
        writeLine(fd, {{"error", err.empty() ? "it holds no audio plug-ins" : err}});
        ::_exit(1);
    }
    Json list = Json::array();
    for (const FoundPlugin& p : found)
        list.push_back(toJson(p));
    writeLine(fd, {{"plugins", list}});
    // Each made once, the way Cue makes it, and let go. Not a bridged one:
    // each would start Wine.
    if (!bridged)
        for (size_t i = first; i < found.size(); ++i) {
            const FoundPlugin& p = found[i];
            std::unique_ptr<Processor> made;
            std::string why;
            if (p.format == "clap")
                made = ClapProcessor::load(p.path, p.id, &why);
            else
                made = Vst3Processor::load(p.path, p.id, &why);
            if (!made) {
                writeLine(fd, {{"opened", i}, {"error", why.empty() ? "it could not be made" : why}});
                continue;
            }
            writeLine(fd, {{"opened", i}, {"sidechain", made->hasSidechain()}, {"editor", made->hasEditor()}, {"midi", made->acceptsMidi()}});
        }
    writeLine(fd, {{"done", true}});
    // Some plug-ins fall over as a process ends; what they said is out.
    ::_exit(0);
}

// ---------------------------------------------------------------- PluginScanner

PluginScanner::PluginScanner(std::vector<std::string> helper, std::string cache, std::string settings)
    : helper_(std::move(helper)), cache_(std::move(cache)), settings_(std::move(settings)) {
    try {
        std::ifstream f(cache_);
        if (f) {
            const Json j = Json::parse(f);
            progress_.scannedAt = j.value("scanned_at", int64_t(0));
            for (const auto& fj : j.value("files", Json::array()))
                files_.push_back(scannedFileOf(fj));
        }
    } catch (const std::exception&) {
        files_.clear(); // a broken cache is scanned again
    }
    try {
        std::ifstream f(settings_);
        if (f) {
            const Json j = Json::parse(f);
            for (const auto& d : j.value("folders", Json::array()))
                if (d.is_string())
                    extra_.push_back(d.get<std::string>());
        }
    } catch (const std::exception&) {
    }
}

PluginScanner::~PluginScanner() {
    stop();
}

std::string PluginScanner::defaultCache() { return xdg("XDG_CACHE_HOME", "/.cache") + "/cue/plugins.json"; }
std::string PluginScanner::defaultSettings() { return xdg("XDG_CONFIG_HOME", "/.config") + "/cue/plugins.json"; }

std::vector<std::string> PluginScanner::extraFolders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return extra_;
}

bool PluginScanner::setExtraFolders(std::vector<std::string> folders, std::string* error) {
    std::vector<std::string> kept;
    for (auto& d : folders) {
        while (d.size() > 1 && d.back() == '/')
            d.pop_back();
        if (d.empty())
            continue;
        if (d[0] != '/' && d[0] != '~') {
            *error = "a folder is a full path, like ~/plugins or /opt/plugins: " + d;
            return false;
        }
        if (std::find(kept.begin(), kept.end(), d) == kept.end())
            kept.push_back(d);
    }
    Json j = {{"folders", kept}};
    std::error_code ec;
    fs::create_directories(fs::path(settings_).parent_path(), ec);
    const std::string tmp = settings_ + ".tmp";
    {
        std::ofstream f(tmp);
        f << j.dump(2) << "\n";
        if (!f) {
            *error = "could not write " + settings_;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), settings_.c_str()) != 0) {
        *error = "could not write " + settings_;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    extra_ = std::move(kept);
    return true;
}

bool PluginScanner::start(const std::string& what, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (progress_.scanning) {
            *error = "a scan is running";
            return false;
        }
    }
    if (thread_.joinable())
        thread_.join();
    const auto all = pluginFiles(folders());
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (what == "all" || what == "changed") {
            // What has gone is forgotten.
            std::set<std::string> there;
            for (const auto& [path, format] : all)
                there.insert(path);
            files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const ScannedFile& f) { return !there.count(f.path); }), files_.end());
            for (const auto& [path, format] : all) {
                const auto it = std::find_if(files_.begin(), files_.end(), [&](const ScannedFile& f) { return f.path == path; });
                const auto [mtime, size] = fileStamp(path);
                if (what == "all" || it == files_.end() || it->mtime != mtime || it->size != size)
                    todo.push_back({path, format});
            }
        } else {
            const auto it = std::find_if(all.begin(), all.end(), [&](const auto& f) { return f.first == what; });
            if (it == all.end()) {
                *error = "no plug-in file " + what + " in the folders scanned";
                return false;
            }
            todo.push_back(*it);
        }
        progress_.scanning = true;
        progress_.done = 0;
        progress_.total = todo.size();
        progress_.current.clear();
    }
    stop_ = false;
    thread_ = std::thread([this, todo = std::move(todo)]() mutable { run(std::move(todo)); });
    return true;
}

void PluginScanner::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

PluginScanner::Progress PluginScanner::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::vector<ScannedFile> PluginScanner::files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_;
}

bool PluginScanner::takeProgressed() { return progressed_.exchange(false); }
bool PluginScanner::takeFinished() { return finished_.exchange(false); }

int PluginScanner::silenceLimitMs(bool bridged) {
    if (const char* s = std::getenv("CUE_SCAN_SECONDS"); s && *s)
        return std::max(100, static_cast<int>(std::atof(s) * 1000));
    return bridged ? 120000 : 30000;
}

void PluginScanner::run(std::vector<std::pair<std::string, std::string>> todo) {
    size_t sinceSave = 0;
    for (const auto& [path, format] : todo) {
        if (stop_)
            break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.current = path;
        }
        progressed_ = true;
        ScannedFile f = scanOne(path, format);
        if (stop_)
            break; // cut short: not what the file is
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = std::find_if(files_.begin(), files_.end(), [&](const ScannedFile& o) { return o.path == path; });
            if (it != files_.end())
                *it = std::move(f);
            else
                files_.push_back(std::move(f));
            std::sort(files_.begin(), files_.end(), [](const ScannedFile& a, const ScannedFile& b) { return a.path < b.path; });
            ++progress_.done;
        }
        progressed_ = true;
        if (++sinceSave == 10) {
            sinceSave = 0;
            save();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.scanning = false;
        progress_.current.clear();
        progress_.scannedAt = now();
    }
    save();
    finished_ = true;
}

ScannedFile PluginScanner::scanOne(const std::string& path, const std::string& format) {
    ScannedFile f;
    f.path = path;
    f.format = format;
    std::tie(f.mtime, f.size) = fileStamp(path);
    const bool bridged = isBridged(path);
    // A plug-in that falls over as it is made is left out, and the helper
    // starts again after it; after three, the rest are not tried.
    size_t first = 0;
    for (int failures = 0;;) {
        std::string failure;
        const int fell = runHelper(f, first, bridged, failure);
        if (fell < 0 || stop_)
            break;
        f.plugins[static_cast<size_t>(fell)].error = failure + " when made";
        first = static_cast<size_t>(fell) + 1;
        if (first >= f.plugins.size())
            break;
        if (++failures == 3) {
            for (size_t i = first; i < f.plugins.size(); ++i)
                if (!f.plugins[i].opened && f.plugins[i].error.empty())
                    f.plugins[i].error = "not tried: three plug-ins of this file fell over when made";
            break;
        }
    }
    return f;
}

int PluginScanner::runHelper(ScannedFile& f, size_t first, bool bridged, std::string& failure) {
    const int limitMs = silenceLimitMs(bridged);
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        f.error = "could not start the scan helper";
        return -1;
    }
    // The helper writes on fd 3; it says nothing elsewhere, and a plug-in
    // that prints goes nowhere. Its own process group, so what it starts
    // (a Wine host) goes with it.
    int writeFd = fds[1];
    if (writeFd == 3) {
        writeFd = ::fcntl(fds[1], F_DUPFD_CLOEXEC, 4);
        ::close(fds[1]);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, writeFd, 3);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<std::string> args = helper_;
    args.insert(args.end(), {f.path, f.format, "3", std::to_string(first)});
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(writeFd);
    if (rc != 0) {
        ::close(fds[0]);
        f.error = std::string("could not start the scan helper: ") + std::strerror(rc);
        return -1;
    }

    using Clock = std::chrono::steady_clock;
    auto heard = Clock::now();
    bool listed = false, done = false, hung = false;
    int making = -1; // the plug-in being made, when the helper stopped
    std::string buffer, error;
    auto onLine = [&](const std::string& line) {
        Json j;
        try {
            j = Json::parse(line);
        } catch (const std::exception&) {
            return;
        }
        if (j.contains("error") && !j.contains("opened")) {
            error = j.at("error").get<std::string>();
        } else if (j.contains("plugins")) {
            // Listed again after a fall: what was learnt is kept.
            if (first == 0 || f.plugins.empty())
                f.plugins = scannedFileOf(j).plugins;
            listed = true;
            making = first < f.plugins.size() && !f.plugins[first].bridged ? static_cast<int>(first) : -1;
        } else if (j.contains("opened")) {
            const size_t i = j.at("opened").get<size_t>();
            if (i < f.plugins.size()) {
                FoundPlugin& p = f.plugins[i];
                if (j.contains("error")) {
                    p.error = j.at("error").get<std::string>();
                } else {
                    p.opened = true;
                    p.sidechain = j.value("sidechain", false);
                    p.editor = j.value("editor", false);
                    p.midi = j.value("midi", false);
                }
                making = i + 1 < f.plugins.size() ? static_cast<int>(i + 1) : -1;
            }
        } else if (j.contains("done")) {
            done = true;
        }
    };
    for (;;) {
        if (stop_) {
            hung = true;
            break;
        }
        const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - heard).count();
        if (silent > limitMs) {
            hung = true;
            break;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        const int n = ::poll(&pfd, 1, 100);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            continue;
        char chunk[4096];
        const ssize_t got = ::read(fds[0], chunk, sizeof chunk);
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (got <= 0)
            break; // the helper is done, or gone
        heard = Clock::now();
        buffer.append(chunk, static_cast<size_t>(got));
        for (size_t nl; (nl = buffer.find('\n')) != std::string::npos;) {
            onLine(buffer.substr(0, nl));
            buffer.erase(0, nl + 1);
        }
    }
    ::close(fds[0]);
    if (hung)
        ::kill(-pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (hung)
        ::kill(-pid, SIGKILL); // anything it left in its group

    if (done)
        return -1;
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%g", limitMs / 1000.0);
    if (hung)
        failure = std::string("did not answer for ") + seconds + " s";
    else if (WIFSIGNALED(status))
        failure = signalName(WTERMSIG(status)) ? std::string("crashed with ") + signalName(WTERMSIG(status))
                                               : "crashed (signal " + std::to_string(WTERMSIG(status)) + ")";
    else
        failure = error.empty() ? "could not be opened" : error;
    if (listed)
        return making; // the plug-in it fell over making, if any
    f.plugins.clear();
    f.error = failure;
    return -1;
}

void PluginScanner::save() {
    Json j;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        j["cue_scan"] = 1;
        j["scanned_at"] = progress_.scannedAt;
        Json list = Json::array();
        for (const ScannedFile& f : files_)
            list.push_back(toJson(f));
        j["files"] = list;
    }
    std::error_code ec;
    fs::create_directories(fs::path(cache_).parent_path(), ec);
    const std::string tmp = cache_ + ".tmp";
    {
        std::ofstream f(tmp);
        f << j.dump() << "\n";
        if (!f)
            return;
    }
    std::rename(tmp.c_str(), cache_.c_str());
}

} // namespace cue
