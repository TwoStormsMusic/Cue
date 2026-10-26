#include "rpp.h"

#include "config.h"
#include "realearn.h"
#include "host/clap_processor.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace cue {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

// Base64, one line at a time: REAPER's lines are whole groups of four.
bool decodeBase64(const std::vector<std::string>& lines, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (const auto& line : lines) {
        if (line.size() % 4 != 0)
            return false;
        for (size_t i = 0; i < line.size(); i += 4) {
            int v[4];
            int pad = 0;
            for (int k = 0; k < 4; ++k) {
                const char c = line[i + k];
                if (c == '=') {
                    v[k] = 0;
                    ++pad;
                } else if ((v[k] = value(c)) < 0) {
                    return false;
                }
            }
            const uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
            out.push_back(static_cast<uint8_t>(n >> 16));
            if (pad < 2)
                out.push_back(static_cast<uint8_t>(n >> 8));
            if (pad < 1)
                out.push_back(static_cast<uint8_t>(n));
        }
    }
    return true;
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size() ? b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24) : 0;
}

uint64_t u64(const std::vector<uint8_t>& b, size_t at) { return u32(b, at) | (static_cast<uint64_t>(u32(b, at + 4)) << 32); }

std::string slug(const std::string& s) {
    std::string out;
    for (char c : lower(s)) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += c;
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out.empty() ? "plugin" : out;
}

// "CLAP: TwoStorms Wah (TwoStorms)" -> "TwoStorms Wah"; "VST3: Parallax (Neural DSP) (mono)" -> "Parallax".
std::string plainName(std::string display) {
    const auto colon = display.find(": ");
    if (colon != std::string::npos && colon < 8)
        display = display.substr(colon + 2);
    display = trim(display);
    while (!display.empty() && display.back() == ')') {
        const auto open = display.rfind(" (");
        if (open == std::string::npos)
            break;
        display = trim(display.substr(0, open));
    }
    return display;
}

std::string homeShort(const std::string& path) {
    const char* home = std::getenv("HOME");
    if (home && *home && startsWith(path, std::string(home) + "/"))
        return "~" + path.substr(std::strlen(home));
    return path;
}

std::vector<std::string> splitPaths(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!trim(item).empty())
            out.push_back(expandPath(trim(item)));
    return out;
}

} // namespace

// ---------------------------------------------------------------- parsing

std::vector<std::string> rppTokens(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size())
            break;
        const char q = line[i];
        if (q == '"' || q == '\'' || q == '`') {
            const size_t end = line.find(q, i + 1);
            out.push_back(line.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
        } else {
            size_t end = i;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])))
                ++end;
            out.push_back(line.substr(i, end - i));
            i = end;
        }
    }
    return out;
}

std::vector<std::string> RppNode::value(const std::string& key) const {
    for (const auto& it : items)
        if (it.child < 0) {
            auto t = rppTokens(it.line);
            if (!t.empty() && t[0] == key)
                return std::vector<std::string>(t.begin() + 1, t.end());
        }
    return {};
}

std::vector<std::vector<std::string>> RppNode::values(const std::string& key) const {
    std::vector<std::vector<std::string>> out;
    for (const auto& it : items)
        if (it.child < 0) {
            auto t = rppTokens(it.line);
            if (!t.empty() && t[0] == key)
                out.emplace_back(t.begin() + 1, t.end());
        }
    return out;
}

const RppNode* RppNode::child(const std::string& name) const {
    for (const auto& c : children)
        if (!c.head.empty() && c.head[0] == name)
            return &c;
    return nullptr;
}

bool parseRpp(const std::string& text, RppNode& root, std::string* error) {
    root = RppNode{};
    root.head = {"ROOT"};
    std::vector<RppNode*> stack{&root};
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string l = trim(raw);
        if (l.empty())
            continue;
        if (l[0] == '<') {
            RppNode* parent = stack.back();
            parent->children.emplace_back();
            parent->children.back().head = rppTokens(l.substr(1));
            parent->items.push_back({"", static_cast<int>(parent->children.size()) - 1});
            // Safe to hold: a node's siblings are only added after it closes.
            stack.push_back(&parent->children.back());
        } else if (l == ">") {
            if (stack.size() <= 1) {
                *error = "line " + std::to_string(lineNo) + ": unexpected '>'";
                return false;
            }
            stack.pop_back();
        } else {
            stack.back()->items.push_back({l, -1});
        }
    }
    if (stack.size() != 1) {
        *error = "the project ends inside a block";
        return false;
    }
    if (root.children.empty() || root.children[0].head.empty() || root.children[0].head[0] != "REAPER_PROJECT") {
        *error = "not a REAPER project";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- locating

SystemLocator::SystemLocator(std::vector<std::string> extraDirs, std::string reaperDir, bool scanClap) : scanClap_(scanClap) {
    for (const auto& d : extraDirs) {
        clapDirs_.push_back(expandPath(d));
        vst3Dirs_.push_back(expandPath(d));
    }
    for (const char* d : {"~/.clap", "/usr/lib/clap", "/usr/local/lib/clap"})
        clapDirs_.push_back(expandPath(d));
    for (const char* d : {"~/.vst3", "/usr/lib/vst3", "/usr/local/lib/vst3"})
        vst3Dirs_.push_back(expandPath(d));
    if (const char* p = std::getenv("CLAP_PATH"))
        for (const auto& d : splitPaths(p, ':'))
            clapDirs_.push_back(d);

    // REAPER's own settings: its plug-in paths, and the ids in its CLAP cache.
    reaperDir = expandPath(reaperDir);
    std::ifstream ini(reaperDir + "/reaper.ini");
    for (std::string line; std::getline(ini, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = lower(line.substr(0, eq)), value = line.substr(eq + 1);
        if (key.find("clap") != std::string::npos && value.find('/') != std::string::npos)
            for (const auto& d : splitPaths(value, ';'))
                clapDirs_.push_back(d);
        if (startsWith(key, "vstpath") && value.find('/') != std::string::npos)
            for (const auto& d : splitPaths(value, ';'))
                vst3Dirs_.push_back(d);
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(reaperDir, ec)) {
        const std::string name = e.path().filename().string();
        if (!startsWith(name, "reaper-clap-") || !endsWith(name, ".ini"))
            continue;
        std::ifstream f(e.path());
        std::string section;
        for (std::string line; std::getline(f, line);) {
            line = trim(line);
            if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos || section.empty() || startsWith(line, "_="))
                continue;
            clapIds_.push_back({line.substr(0, eq), section});
        }
    }
}

void SystemLocator::indexFiles() {
    if (indexed_)
        return;
    indexed_ = true;
    std::set<std::string> seen;
    auto walk = [&](const std::string& dir, bool clap) {
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string name = p.filename().string();
            if (it.depth() > 6) {
                it.disable_recursion_pending();
                continue;
            }
            if (endsWith(lower(name), ".vst3")) {
                it.disable_recursion_pending(); // a bundle: do not look inside
                if (!clap && seen.insert(p.string()).second)
                    vst3Bundles_.push_back(p.string());
            } else if (clap && endsWith(lower(name), ".clap") && !it->is_directory(ec) && seen.insert(p.string()).second) {
                clapFiles_.push_back(p.string());
            }
        }
    };
    for (const auto& d : clapDirs_)
        walk(d, true);
    for (const auto& d : vst3Dirs_)
        walk(d, false);
}

std::string SystemLocator::findClap(const std::string& id, const std::string& name) {
    indexFiles();
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return fs::exists(p, ec);
    };
    auto byFileName = [&](const std::string& file) -> std::string {
        if (!file.empty() && file[0] == '/')
            return exists(file) ? file : "";
        for (const auto& f : clapFiles_)
            if (fs::path(f).filename() == file)
                return f;
        return "";
    };
    for (const auto& [pid, file] : clapIds_)
        if (pid == id)
            if (auto f = byFileName(file); !f.empty())
                return f;

    // A file named after the plug-in, checked by opening it if allowed.
    auto native = [](const std::string& f) { return f.find("/yabridge/") == std::string::npos; };
    for (const auto& f : clapFiles_) {
        if (!native(f) || lower(fs::path(f).stem().string()) != lower(name))
            continue;
        if (!scanClap_)
            return f;
        for (const auto& info : scanClapFile(f))
            if (info.id == id)
                return f;
    }
    if (scanClap_ && !scanned_) {
        scanned_ = true;
        for (const auto& f : clapFiles_)
            if (native(f))
                for (const auto& info : scanClapFile(f))
                    clapIds_.push_back({info.id, f});
        for (const auto& [pid, file] : clapIds_)
            if (pid == id && exists(file))
                return file;
    }
    return "";
}

std::string SystemLocator::findVst3(const std::string& file) {
    indexFiles();
    for (const auto& b : vst3Bundles_)
        if (lower(fs::path(b).filename().string()) == lower(file))
            return b;
    return "";
}

// ---------------------------------------------------------------- converting

namespace {

struct RppPlugin {
    std::string kind;  // clap, vst3
    std::string path;  // "" if not found
    std::string guess; // a path to write when not found
    std::string id;
    std::string label;
    bool bypassed = false;
    std::string state, controllerState; // file names
    std::vector<std::string> comments;
    std::vector<std::string> params;
    int keyLine = -1;   // the plug-in whose output feeds its side-chain:
    size_t keyIndex = 0; // line and index in it
};

struct RppLine {
    std::string name;
    int parent = -1;
    size_t after = 0;
    std::vector<RppPlugin> plugins;
};

// The channels a pin list writes: "PIN 0 2 3" -> {2, 3} for pin 0.
std::vector<int> pinChannels(const RppNode* pins, int pin) {
    if (!pins)
        return {};
    for (const auto& t : pins->values("PIN"))
        if (!t.empty() && std::atoi(t[0].c_str()) == pin) {
            std::vector<int> ch;
            for (size_t i = 1; i < t.size(); ++i)
                ch.push_back(std::atoi(t[i].c_str()));
            return ch;
        }
    return {};
}

// Every channel a pin list maps; REAPER's default (no PIN lines) is the main pair.
std::vector<int> allPinChannels(const RppNode* pins) {
    std::set<int> ch;
    bool any = false;
    if (pins)
        for (const auto& t : pins->values("PIN")) {
            any = true;
            for (size_t i = 1; i < t.size(); ++i)
                ch.insert(std::atoi(t[i].c_str()));
        }
    if (!any)
        return {0, 1};
    return std::vector<int>(ch.begin(), ch.end());
}

std::vector<int> maskChannels(uint64_t mask) {
    std::vector<int> ch;
    for (int i = 0; i < 64; ++i)
        if (mask >> i & 1)
            ch.push_back(i);
    return ch;
}

std::string encodeBase64(const uint8_t* p, size_t n) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += abc[v >> 18 & 63];
        out += abc[v >> 12 & 63];
        out += i + 1 < n ? abc[v >> 6 & 63] : '=';
        out += i + 2 < n ? abc[v & 63] : '=';
    }
    return out;
}

// A .wav file's samples, as the TwoStorms IR Loader reads them: PCM 8 to 32
// bit or float, its first two channels, at most 262144 frames.
struct Wav {
    int frames = 0, channels = 0;
    double rate = 0;
    std::vector<float> samples; // interleaved
};

bool readWav(const std::string& path, Wav& wav, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = "cannot open it";
        return false;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    auto u16 = [&](size_t at) { return static_cast<uint32_t>(b[at] | b[at + 1] << 8); };
    if (b.size() < 44 || (std::memcmp(b.data(), "RIFF", 4) && std::memcmp(b.data(), "RF64", 4)) || std::memcmp(b.data() + 8, "WAVE", 4)) {
        *error = "not a .wav file";
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t data = 0, length = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t len = u32(b, at + 4);
        if (!std::memcmp(b.data() + at, "fmt ", 4) && len >= 16 && at + 24 <= b.size()) {
            format = static_cast<int>(u16(at + 8));
            channels = static_cast<int>(u16(at + 10));
            rate = u32(b, at + 12);
            bits = static_cast<int>(u16(at + 22));
            if (format == 0xFFFE && len >= 40 && at + 34 <= b.size())
                format = static_cast<int>(u16(at + 32)); // extensible: the sub-format
        } else if (!std::memcmp(b.data() + at, "data", 4)) {
            data = at + 8;
            length = std::min<size_t>(len == 0xFFFFFFFFu ? b.size() : len, b.size() - data);
            break;
        }
        at += 8 + static_cast<size_t>(len) + (len & 1);
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool flt = format == 3 && (bits == 32 || bits == 64);
    if (!data || channels < 1 || rate < 8000 || (!pcm && !flt)) {
        *error = "a .wav format the IR Loader does not read";
        return false;
    }
    const size_t bytes = bits / 8;
    wav.frames = static_cast<int>(std::min<size_t>(length / (bytes * channels), 262144));
    wav.channels = std::min(2, channels);
    wav.rate = rate;
    wav.samples.resize(static_cast<size_t>(wav.frames) * wav.channels);
    for (int i = 0; i < wav.frames; ++i)
        for (int c = 0; c < wav.channels; ++c) {
            const uint8_t* q = b.data() + data + (static_cast<size_t>(i) * channels + c) * bytes;
            double v;
            if (flt && bits == 32) {
                float x;
                std::memcpy(&x, q, 4);
                v = x;
            } else if (flt) {
                double x;
                std::memcpy(&x, q, 8);
                v = x;
            } else if (bits == 8) {
                v = (q[0] - 128) / 128.0;
            } else if (bits == 16) {
                v = static_cast<int16_t>(q[0] | q[1] << 8) / 32768.0;
            } else if (bits == 24) {
                v = static_cast<int32_t>(static_cast<uint32_t>(q[0]) << 8 | static_cast<uint32_t>(q[1]) << 16 | static_cast<uint32_t>(q[2]) << 24) /
                    2147483648.0;
            } else {
                v = static_cast<int32_t>(q[0] | q[1] << 8 | q[2] << 16 | static_cast<uint32_t>(q[3]) << 24) / 2147483648.0;
            }
            wav.samples[static_cast<size_t>(i) * wav.channels + c] = static_cast<float>(v);
        }
    if (wav.frames <= 0) {
        *error = "it has no audio";
        return false;
    }
    return true;
}

// A TwoStorms IR Loader state names its IRs by path but only plays the
// samples saved in it. For an IR given only by path, read the file and add
// its samples, so the IR plays in Cue. Returns notes on files it could not use.
std::vector<std::string> embedTwoStormsIrs(std::vector<uint8_t>& state) {
    std::vector<std::string> notes;
    std::string text(state.begin(), state.end());
    if (!startsWith(text, "TWOSTORMS "))
        return notes;
    std::string added;
    for (int k = 0; k < 2; ++k) {
        const std::string key = "x ir" + std::to_string(k);
        if (text.find("\n" + key + "data ") != std::string::npos)
            continue;
        const auto at = text.find("\n" + key + "path ");
        if (at == std::string::npos)
            continue;
        const size_t from = at + key.size() + 6;
        const std::string path = trim(text.substr(from, text.find('\n', from) - from));
        Wav wav;
        std::string err;
        if (!readWav(path, wav, &err)) {
            notes.push_back("IR " + path + ": " + err + "; that IR Loader starts without it");
            continue;
        }
        char head[96];
        std::snprintf(head, sizeof head, "%sdata %d %d %.17g ", key.c_str(), wav.frames, wav.channels, wav.rate);
        added += head + encodeBase64(reinterpret_cast<const uint8_t*>(wav.samples.data()), wav.samples.size() * sizeof(float)) + "\n";
    }
    if (!added.empty()) {
        if (!text.empty() && text.back() != '\n')
            text += '\n';
        text += added;
        state.assign(text.begin(), text.end());
    }
    return notes;
}

} // namespace

bool convertRpp(const std::string& rppText, const std::string& projectName, PluginLocator& locator, const std::string& mapping, Import& out,
                std::string* error) {
    RppNode root;
    if (!parseRpp(rppText, root, error))
        return false;
    const RppNode& project = root.children[0];
    out = Import{};
    // Scenes: a REAPER folder is a scene, tracks outside folders play in one
    // named after the project.
    struct Scene {
        std::string name;
        std::ostringstream text;
        std::vector<std::string> chains;
    };
    std::vector<Scene> scenes;
    int depth = 0;
    auto sceneNamed = [&](const std::string& n) -> Scene& {
        for (auto& sc : scenes)
            if (sc.name == n)
                return sc;
        scenes.emplace_back();
        scenes.back().name = n;
        return scenes.back();
    };
    std::vector<std::string> chainNames;
    std::set<int> inputs;
    // What was left out or not found, with the chains it was in, in order.
    std::vector<std::pair<std::string, std::vector<std::string>>> skippedIn, missingIn;
    auto note = [](std::vector<std::pair<std::string, std::vector<std::string>>>& list, const std::string& what, const std::string& chain) {
        for (auto& [w, chains] : list)
            if (w == what) {
                if (chains.back() != chain)
                    chains.push_back(chain);
                return;
            }
        list.push_back({what, {chain}});
    };
    double rate = 0;
    if (auto sr = project.value("SAMPLERATE"); !sr.empty())
        rate = std::atof(sr[0].c_str());
    // A project the converter wrote carries the performance's own mappings
    // in its notes. A mapping file given here wins over them.
    CueMappings cue;
    if (const RppNode* notes = project.child("NOTES"); notes && mapping.empty()) {
        std::vector<std::string> lines;
        for (const auto& it : notes->items)
            if (it.child < 0)
                lines.push_back(it.line);
        cue = readCueMappings(lines);
    }
    // Outputs: the performance's, by the hardware channel each was sent to;
    // else one per hardware output the project uses.
    int outputs = 2;
    if (cue.found && !cue.outputs.empty()) {
        outputs = static_cast<int>(cue.outputs.size());
    } else {
        for (const RppNode& track : project.children)
            if (!track.head.empty() && track.head[0] == "TRACK")
                for (const auto& h : track.values("HWOUT"))
                    if (!h.empty())
                        outputs = std::max(outputs, std::min(64, (std::atoi(h[0].c_str()) & 1023) + 1));
    }
    auto outputOf = [&](int dest) {
        if (!cue.found || cue.outputs.empty())
            return dest < outputs ? dest : -1;
        for (size_t o = 0; o < cue.outputs.size(); ++o)
            if (cue.outputs[o].channel == dest)
                return static_cast<int>(o);
        return -1;
    };
    // Tracks outside folders: the performance's one scene, or one named after the project.
    const std::string defaultScene = cue.found && cue.scenes.size() == 1 ? cue.scenes[0].name : projectName;
    // ReaLearn instances, and what their GUIDs name in Cue.
    std::vector<std::string> realearns;
    std::map<std::string, std::string> trackChain, fxPlugin;
    auto bare = [](std::string g) {
        g.erase(std::remove_if(g.begin(), g.end(), [](char c) { return c == '{' || c == '}'; }), g.end());
        std::transform(g.begin(), g.end(), g.begin(), [](unsigned char c) { return std::toupper(c); });
        return g;
    };

    for (const RppNode& track : project.children) {
        if (track.head.empty() || track.head[0] != "TRACK")
            continue;
        auto nameTokens = track.value("NAME");
        std::string name = nameTokens.empty() ? "" : trim(nameTokens[0]);
        if (name.empty())
            name = "Track " + std::to_string(out.chains + 1);
        const auto hwouts = track.values("HWOUT");
        const RppNode* fxchain = track.child("FXCHAIN");
        bool pedals = false;
        if (fxchain)
            for (const RppNode& fx : fxchain->children)
                if (fx.head.size() > 1 && fx.head[0] == "VST" && fx.head[1].find("ReaLearn") != std::string::npos) {
                    std::vector<std::string> b64;
                    for (const auto& it : fx.items)
                        if (it.child < 0)
                            b64.push_back(it.line);
                    std::vector<uint8_t> body;
                    std::string text;
                    if (decodeBase64(b64, body) && realearnJson(body, text))
                        realearns.push_back(text);
                    else
                        out.notes.push_back("track " + name + ": its ReaLearn settings could not be read");
                    pedals = true;
                }
        int folder = 0, change = 0;
        if (auto bus = track.value("ISBUS"); bus.size() > 1) {
            folder = std::atoi(bus[0].c_str());
            change = std::atoi(bus[1].c_str());
        }
        if (folder == 1 && depth == 0)
            sceneNamed(name);
        Scene& scene = depth > 0 || folder == 1 ? scenes.back() : sceneNamed(defaultScene);
        if (folder == 1)
            depth += std::max(1, change);
        else if (folder == 2)
            depth = std::max(0, depth + change);
        if (hwouts.empty()) {
            if (folder != 1 && !pedals)
                out.notes.push_back("track " + name + ": no hardware output, not imported");
            continue;
        }
        std::ostringstream& chainsText = scene.text;
        // Unique chain names.
        std::string chainName = name;
        for (int n = 2; std::find(chainNames.begin(), chainNames.end(), chainName) != chainNames.end(); ++n)
            chainName = name + " " + std::to_string(n);
        chainNames.push_back(chainName);
        scene.chains.push_back(chainName);
        if (track.head.size() > 1)
            trackChain[bare(track.head[1])] = chainName;
        ++out.chains;
        if (auto rec = track.value("REC"); rec.size() > 1) {
            const int in = std::atoi(rec[1].c_str());
            if (in >= 0 && in < 1024)
                inputs.insert(in);
            else
                out.notes.push_back("track " + name + ": records from a stereo or MIDI input; Cue plays it from the configured mono input");
        }
        if (auto ms = track.value("MAINSEND"); !ms.empty() && ms[0] == "1")
            out.notes.push_back("track " + name + ": also sends to the master track, which Cue does not have");

        // Follow the signal through the track's channels: each channel holds
        // the output of one line, the main line or a branch off it.
        std::vector<RppLine> lines(1);
        std::map<int, int> owner{{0, 0}, {1, 0}};
        std::map<int, std::pair<int, size_t>> writer; // channel -> the plug-in that last wrote it
        int branches = 0;
        int index = 0;
        bool bypassNext = false, offlineNext = false;
        std::pair<int, size_t> lastFx{-1, 0};                       // the plug-in an FXID line names
        std::vector<std::pair<std::string, std::pair<int, size_t>>> fxIds; // GUID, line and index
        if (fxchain) {
            for (const auto& item : fxchain->items) {
                if (item.child < 0) {
                    const auto t = rppTokens(item.line);
                    if (!t.empty() && t[0] == "BYPASS") {
                        bypassNext = t.size() > 1 && t[1] == "1";
                        offlineNext = t.size() > 2 && t[2] == "1";
                    } else if (t.size() > 1 && t[0] == "FXID" && lastFx.first >= 0) {
                        fxIds.push_back({bare(t[1]), lastFx});
                        lastFx = {-1, 0};
                    }
                    continue;
                }
                const RppNode& fx = fxchain->children[item.child];
                lastFx = {-1, 0};
                if (fx.head.empty())
                    continue;
                const std::string type = fx.head[0];
                if (type != "CLAP" && type != "VST" && type != "JS" && type != "AU" && type != "LV2" && type != "DX" && type != "VIDEO_EFFECT")
                    continue;
                ++index;
                const std::string display = fx.head.size() > 1 ? fx.head[1] : type;
                const bool bypassed = bypassNext, offline = offlineNext;
                bypassNext = offlineNext = false;
                auto skip = [&](const std::string& why) {
                    note(skippedIn, plainName(display) + " (" + why + ")", chainName);
                    ++out.skipped;
                };
                if (offline) {
                    skip("offline in REAPER");
                    continue;
                }
                RppPlugin p;
                p.bypassed = bypassed;
                std::vector<int> inCh{0}, outCh{0}, written{0, 1};
                int keyCh = -1; // the channel its side-chain input reads
                std::string custom;
                if (type == "CLAP") {
                    p.kind = "clap";
                    p.id = fx.head.size() > 2 ? fx.head[2] : "";
                    custom = fx.head.size() > 3 ? fx.head[3] : "";
                    p.label = plainName(display);
                    p.path = locator.findClap(p.id, p.label);
                    p.guess = startsWith(p.id, "in.lsp-plug.")     ? "~/.clap/lsp-plugins.clap"
                              : startsWith(p.id, "com.twostorms.") ? "~/.clap/CLAP Plugins Linux/" + p.label + ".clap"
                                                                   : "~/.clap/" + p.label + ".clap";
                    if (auto ch = pinChannels(fx.child("OUT_PINS"), 0); !ch.empty())
                        outCh = ch;
                    if (auto ch = pinChannels(fx.child("IN_PINS"), 0); !ch.empty())
                        inCh = ch;
                    written = allPinChannels(fx.child("OUT_PINS"));
                    // Pins 2-3 are the side-chain input of a stereo plug-in:
                    // only an explicit mapping counts.
                    if (auto ch = pinChannels(fx.child("IN_PINS"), 2); !ch.empty())
                        keyCh = ch[0];
                    std::vector<std::string> b64;
                    if (const RppNode* st = fx.child("STATE"))
                        for (const auto& it : st->items)
                            if (it.child < 0)
                                b64.push_back(it.line);
                    std::vector<uint8_t> state;
                    if (!b64.empty() && decodeBase64(b64, state) && !state.empty()) {
                        if (p.id == "com.twostorms.irloader")
                            for (const auto& n : embedTwoStormsIrs(state))
                                out.notes.push_back(chainName + ": " + n);
                        p.state = "states/" + slug(chainName) + "-" + (index < 10 ? "0" : "") + std::to_string(index) + "-" + slug(p.label) + ".clapstate";
                        out.files.push_back({p.state, state});
                    }
                    if (p.id == "com.twostorms.gainstage") {
                        p.params.push_back("54:1");
                        p.comments.push_back("param 54 is ON/OFF: REAPER switched tracks with it, Cue switches chains");
                    }
                } else if (type == "VST" && (startsWith(display, "VST3:") || startsWith(display, "VST3i:"))) {
                    p.kind = "vst3";
                    const std::string file = fx.head.size() > 2 ? fx.head[2] : "";
                    custom = fx.head.size() > 4 ? fx.head[4] : "";
                    p.label = plainName(display);
                    const std::string uidField = fx.head.size() > 5 ? fx.head[5] : "";
                    const auto open = uidField.find('{'), close = uidField.find('}');
                    if (open != std::string::npos && close != std::string::npos && close > open)
                        p.id = uidField.substr(open + 1, close - open - 1);
                    p.path = locator.findVst3(file);
                    // yabridge keeps the Windows folders: Neural DSP installs into its own.
                    p.guess = "~/.vst3/yabridge/" + std::string(display.find("(Neural DSP)") != std::string::npos ? "Neural DSP/" : "") + file;
                    // The body: a header with pin masks, then the plug-in's
                    // chunk: [size][1][component][size][0][controller].
                    std::vector<std::string> b64;
                    for (const auto& it : fx.items)
                        if (it.child < 0)
                            b64.push_back(it.line);
                    std::vector<uint8_t> body;
                    if (decodeBase64(b64, body) && body.size() > 16) {
                        size_t at = 8;
                        const uint32_t nIn = u32(body, at);
                        at += 4;
                        if (nIn > 0)
                            inCh = maskChannels(u64(body, at));
                        at += 8ull * nIn;
                        const uint32_t nOut = u32(body, at);
                        at += 4;
                        if (nOut > 0) {
                            outCh = maskChannels(u64(body, at));
                            std::set<int> all;
                            for (uint32_t k = 0; k < nOut && k < 64; ++k)
                                for (int c : maskChannels(u64(body, at + 8ull * k)))
                                    all.insert(c);
                            written.assign(all.begin(), all.end());
                        }
                        at += 8ull * nOut;
                        const uint32_t size = u32(body, at);
                        at += 12;
                        if (nIn < 64 && nOut < 64 && at + size <= body.size() && size >= 8) {
                            std::vector<uint8_t> chunk(body.begin() + at, body.begin() + at + size);
                            const uint32_t compSize = u32(chunk, 0);
                            if (8ull + compSize <= chunk.size()) {
                                std::vector<uint8_t> comp(chunk.begin() + 8, chunk.begin() + 8 + compSize);
                                const size_t c2 = 8 + compSize;
                                const uint32_t ctrlSize = u32(chunk, c2);
                                const std::string base = "states/" + slug(chainName) + "-" + (index < 10 ? "0" : "") + std::to_string(index) + "-" + slug(p.label);
                                p.state = base + ".vst3state";
                                out.files.push_back({p.state, comp});
                                if (ctrlSize > 0 && c2 + 8 + ctrlSize <= chunk.size()) {
                                    p.controllerState = base + ".vst3ctrl";
                                    out.files.push_back({p.controllerState, std::vector<uint8_t>(chunk.begin() + c2 + 8, chunk.begin() + c2 + 8 + ctrlSize)});
                                }
                            }
                        }
                    }
                    if (inCh.empty())
                        inCh = {0};
                    if (outCh.empty())
                        outCh = {0};
                } else if (type == "VST") {
                    skip("VST2; Cue hosts CLAP and VST3");
                    continue;
                } else if (type == "LV2") {
                    skip("LV2 is not supported yet");
                    continue;
                } else {
                    skip(type + " is not supported");
                    continue;
                }
                if (!custom.empty())
                    p.label = custom;
                if (p.path.empty()) {
                    ++out.missing;
                    note(missingIn, display, chainName);
                }
                ++out.plugins;

                // Which line: the one whose signal it reads. If it writes
                // neither that line's channel nor the main pair, it starts a
                // branch whose signal lives on the channels it writes.
                const int src = owner.count(inCh[0]) ? owner[inCh[0]] : 0;
                bool continues = false;
                for (int c : outCh)
                    continues = continues || c == inCh[0] || (owner.count(c) && owner[c] == src && (c == 0 || c == 1));
                int target = src;
                if (!continues) {
                    RppLine b;
                    b.name = "Branch " + std::to_string(++branches);
                    b.parent = src;
                    b.after = lines[src].plugins.size();
                    lines.push_back(b);
                    target = static_cast<int>(lines.size()) - 1;
                }
                if (continues && std::count(written.begin(), written.end(), inCh[0]) && !std::count(written.begin(), written.end(), inCh[0] + 1))
                    out.notes.push_back(chainName + ": " + p.label + " writes channel " + std::to_string(inCh[0] + 1) + " but not " +
                                        std::to_string(inCh[0] + 2) + " in REAPER; Cue's chains are stereo, so both carry its output");
                if (keyCh >= 0 && writer.count(keyCh)) {
                    p.keyLine = writer[keyCh].first;
                    p.keyIndex = writer[keyCh].second;
                }
                for (int c : written) {
                    owner[c] = target;
                    writer[c] = {target, lines[target].plugins.size()};
                }
                lastFx = {target, lines[target].plugins.size()};
                lines[target].plugins.push_back(p);
            }
        }

        // Outputs: which line each hardware output takes its signal from.
        std::vector<int> tapLine(static_cast<size_t>(outputs), -1);
        for (const auto& h : hwouts) {
            if (h.size() < 7)
                continue;
            const int hw = std::atoi(h[0].c_str()) & 1023;
            const int srcCh = std::atoi(h[6].c_str()) & 1023;
            const int dest = outputOf(hw);
            if (dest < 0) {
                out.notes.push_back(chainName + ": hardware output " + std::to_string(hw + 1) + " left out (no output of the performance plays there)");
                continue;
            }
            if (tapLine[dest] >= 0) {
                out.notes.push_back(chainName + ": more than one send to output " + std::to_string(dest + 1) + "; Cue takes the first");
                continue;
            }
            if (!owner.count(srcCh)) {
                out.notes.push_back(chainName + ": output " + std::to_string(dest + 1) + " takes channel " + std::to_string(srcCh + 1) +
                                    ", which no plug-in writes; Cue takes the end of the chain");
                tapLine[dest] = 0;
            } else {
                tapLine[dest] = owner[srcCh];
            }
        }
        for (int o = 0; o < outputs; ++o)
            if (tapLine[o] < 0) {
                out.notes.push_back(chainName + ": nothing goes to output " + std::to_string(o + 1) + " in REAPER; Cue sends the end of the chain there");
                tapLine[o] = 0;
            }
        // Branches named after the output they feed, or as the performance named them.
        for (int o = 0; o < outputs; ++o)
            if (tapLine[o] > 0 && startsWith(lines[tapLine[o]].name, "Branch "))
                lines[tapLine[o]].name = "Out " + std::to_string(o + 1);
        for (const auto& sc : cue.scenes)
            for (const auto& ch : sc.chains)
                if (ch.name == chainName && ch.branches.size() + 1 == lines.size())
                    for (size_t b = 1; b < lines.size(); ++b)
                        lines[b].name = ch.branches[b - 1];

        // Slots as the config will number them: the main line, then the
        // branches in the order their branch lines appear.
        std::vector<int> order{0};
        for (size_t li = 0; li < lines.size(); ++li)
            for (size_t k = 0; k <= lines[li].plugins.size(); ++k)
                for (size_t b = 1; b < lines.size(); ++b)
                    if (lines[b].parent == static_cast<int>(li) && lines[b].after == k)
                        order.push_back(static_cast<int>(b));
        std::map<std::pair<int, size_t>, size_t> slotOf;
        std::vector<std::string> labels;
        for (int li : order)
            for (size_t k = 0; k < lines[li].plugins.size(); ++k) {
                slotOf[{li, k}] = labels.size();
                labels.push_back(lines[li].plugins[k].label);
            }
        for (const auto& [id, at] : fxIds) {
            const size_t slot = slotOf[at];
            int count = 0, nth = 0;
            for (size_t i = 0; i < labels.size(); ++i)
                if (labels[i] == labels[slot]) {
                    ++count;
                    if (i == slot)
                        nth = count;
                }
            fxPlugin[id] = chainName + "/" + labels[slot] + (count > 1 ? ":" + std::to_string(nth) : "");
        }
        // `sidechain = Label`, numbered as Label:N when the chain has more than one.
        auto keyRef = [&](const RppPlugin& p, int li, size_t k) -> std::string {
            if (p.keyLine < 0)
                return "";
            const size_t from = slotOf[{p.keyLine, p.keyIndex}], self = slotOf[{li, static_cast<size_t>(k)}];
            const std::string& label = labels[from];
            if (from >= self) {
                out.notes.push_back(chainName + ": " + p.label + " takes its side-chain from " + label +
                                    ", which Cue processes after it; its side-chain gets silence");
                return "";
            }
            int count = 0, nth = 0;
            for (size_t i = 0; i < labels.size(); ++i)
                if (labels[i] == label) {
                    ++count;
                    if (i == from)
                        nth = count;
                }
            return count > 1 ? label + ":" + std::to_string(nth) : label;
        };

        // Write the chain.
        auto writePlugin = [&](const RppPlugin& p, int li, size_t k) {
            const std::string path = p.path.empty() ? p.guess : homeShort(p.path);
            chainsText << "plugin = " << p.kind << ":" << path << (p.id.empty() ? "" : "#" + p.id);
            if (p.path.empty())
                chainsText << "   ; NOT FOUND: fix this path";
            chainsText << "\n";
            chainsText << "name = " << p.label << "\n";
            if (!p.state.empty())
                chainsText << "state = " << p.state << "\n";
            if (!p.controllerState.empty())
                chainsText << "controller_state = " << p.controllerState << "\n";
            for (size_t i = 0; i < p.params.size(); ++i)
                chainsText << "param = " << p.params[i] << (i < p.comments.size() ? "   ; " + p.comments[i] : "") << "\n";
            if (p.bypassed)
                chainsText << "bypass = yes\n";
            if (auto ref = keyRef(p, li, k); !ref.empty())
                chainsText << "sidechain = " << ref << "\n";
        };
        auto writeLine = [&](int li) {
            const RppLine& l = lines[li];
            for (size_t k = 0; k <= l.plugins.size(); ++k) {
                for (size_t b = 1; b < lines.size(); ++b)
                    if (lines[b].parent == li && lines[b].after == k)
                        chainsText << "branch = " << lines[b].name << "\n";
                if (k < l.plugins.size())
                    writePlugin(l.plugins[k], li, k);
            }
            for (int o = 0; o < outputs; ++o)
                if (tapLine[o] == li && li > 0)
                    chainsText << "tap = out" << o + 1 << "\n";
        };
        chainsText << "\n[chain " << chainName << "]\n";
        writeLine(0);
        for (size_t b = 1; b < lines.size(); ++b) {
            chainsText << "\n[branch " << lines[b].name << "]\n";
            writeLine(static_cast<int>(b));
        }
    }

    if (out.chains == 0) {
        *error = "no track plays to a hardware output";
        return false;
    }
    auto inChains = [](const std::vector<std::string>& chains) {
        std::string s;
        for (size_t i = 0; i < chains.size(); ++i)
            s += (i == 0 ? "" : i + 1 == chains.size() ? " and " : ", ") + chains[i];
        return s;
    };
    for (const auto& [what, chains] : skippedIn)
        out.notes.push_back("left out " + what + " in " + inChains(chains));
    for (const auto& [what, chains] : missingIn)
        out.notes.push_back("not found on this machine: " + what + " (" + inChains(chains) + "); fix its path in the config");

    std::ostringstream cfg;
    cfg << "; " << projectName << ", imported from REAPER by cue import-rpp.\n"
        << "; Plug-in settings are in states/. Re-import to refresh them.\n";
    if (!out.notes.empty()) {
        cfg << ";\n; Notes from the import:\n";
        for (const auto& n : out.notes)
            cfg << ";   " << n << "\n";
    }
    const int input = inputs.size() == 1 ? *inputs.begin() + 1 : 1;
    if (cue.found) {
        auto names = [](const std::vector<CueMappings::Port>& ports) {
            std::string s;
            for (const auto& p : ports)
                s += (s.empty() ? "" : ", ") + p.name;
            return s;
        };
        cfg << "\n[performance]\n";
        if (!cue.inputs.empty())
            cfg << "inputs = " << cue.inputs.front().name << "\n";
        if (!cue.outputs.empty())
            cfg << "outputs = " << names(cue.outputs) << "\n";
        if (cue.inputs.size() > 1)
            out.notes.push_back("the performance had " + std::to_string(cue.inputs.size()) + " inputs; the import plays every chain from the first");
    } else if (outputs > 2) {
        cfg << "\n[performance]\noutputs = ";
        for (int o = 0; o < outputs; ++o)
            cfg << (o ? ", " : "") << "Out " << o + 1;
        cfg << "\n";
    }
    cfg << "\n[audio]\n"
        << "device = hw:M2\n"
        << "rate = " << (rate > 0 ? static_cast<int>(rate) : 48000) << "\n"
        << "period = 64\n"
        << "periods = 3         ; as REAPER on the M2: a block may run a period late without an xrun\n"
        << "input = " << input << "\n"
        << [&] {
               std::string s;
               for (int o = 0; o < outputs; ++o)
                   s += "out" + std::to_string(o + 1) + " = " +
                        std::to_string(cue.found && !cue.outputs.empty() ? cue.outputs[o].channel + 1 : o + 1) + "\n";
               return s;
           }()
        << "mix = left          ; as REAPER's mono hardware sends: the chain's left channel\n"
        << "\n[switching]\n"
        << "preroll_ms = 50\n"
        << "crossfade_ms = 20\n"
        << "warm = next\n"
        << "\n[midi]\n"
        << "in = M2             ; the FCB1010 on the M2's MIDI ports\n"
        << "profile = eurekaprom\n"
        << "channel = 1\n";
    for (const Scene& sc : scenes) {
        if (sc.chains.empty())
            continue;
        cfg << "\n[scene " << sc.name << "]\n";
        const CueMappings::Scene* saved = nullptr;
        for (const auto& s : cue.scenes)
            if (s.name == sc.name)
                saved = &s;
        if (saved) {
            if (!saved->display.empty())
                cfg << "display = " << saved->display << "\n";
            if (saved->pedal)
                cfg << "pedal = " << saved->pedal << "\n";
            if (saved->program >= 0)
                cfg << "program = " << saved->program << "\n";
        }
        cfg << sc.text.str();
        if (mapping.empty() && saved) {
            for (const auto& c : sc.chains) {
                cfg << "\n[preset " << c << "]\nchain = " << c << "\n";
                for (const auto& ch : saved->chains)
                    if (ch.name == c) {
                        if (!ch.display.empty())
                            cfg << "display = " << ch.display << "\n";
                        if (ch.pedal)
                            cfg << "pedal = " << ch.pedal << "\n";
                        if (ch.program >= 0)
                            cfg << "program = " << ch.program << "\n";
                        if (!ch.warm.empty())
                            cfg << "warm = " << ch.warm << "\n";
                        for (const auto& a : ch.onSelect)
                            cfg << "do = " << a << "\n";
                    }
            }
        } else if (mapping.empty()) {
            cfg << "\n; One preset per chain. Keys 1-" << std::min<size_t>(9, chainNames.size())
                << " select them in cue run; add pedal = N to select one from the FCB.\n";
            for (const auto& c : sc.chains)
                cfg << "\n[preset " << c << "]\nchain = " << c << "\n";
        }
    }
    if (!mapping.empty())
        cfg << "\n; Presets, controls and pedals.\ninclude = " << mapping << "\n";
    if (cue.found)
        cfg << "\n; The performance's controls and pedals, as Cue Converter saved them in the project.\n" << cue.ini;
    std::string triggers;
    for (const auto& text : mapping.empty() ? realearns : std::vector<std::string>{}) // a pedal file replaces ReaLearn
        triggers += realearnTriggers(text, trackChain, fxPlugin, cue.found, out.notes);
    if (!triggers.empty())
        cfg << "\n; From ReaLearn.\n" << triggers;
    if (cue.found)
        out.notes.push_back("pedals: read from the Cue performance saved in the project; changes made in ReaLearn are not read back");
    out.config = cfg.str();
    return true;
}

} // namespace cue
