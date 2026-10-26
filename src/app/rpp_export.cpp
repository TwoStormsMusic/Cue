#include "rpp_export.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace cue {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string base64(const std::vector<uint8_t>& data) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        out += abc[v >> 18 & 63];
        out += abc[v >> 12 & 63];
        out += i + 1 < data.size() ? abc[v >> 6 & 63] : '=';
        out += i + 2 < data.size() ? abc[v & 63] : '=';
    }
    return out;
}

// REAPER's layout: lines of 128 characters; a group ends with a shorter line.
std::string base64Lines(const std::vector<uint8_t>& data, const std::string& indent) {
    const std::string b = base64(data);
    std::string out;
    for (size_t i = 0; i < b.size(); i += 128)
        out += indent + b.substr(i, 128) + "\n";
    return out;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}

// The same GUID every time for the same name, so exporting twice gives the same project.
std::string guid(const std::string& seed) {
    uint64_t a = 1469598103934665603ull, b = 1099511628211ull;
    for (unsigned char c : seed) {
        a = (a ^ c) * 1099511628211ull;
        b = (b ^ c) * 6364136223846793005ull + 1442695040888963407ull;
    }
    char s[40];
    std::snprintf(s, sizeof s, "{%08X-%04X-%04X-%04X-%012llX}", static_cast<unsigned>(a >> 32), static_cast<unsigned>(a >> 16 & 0xffff),
                  static_cast<unsigned>(a & 0xffff), static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xffffffffffffull));
    return s;
}

// REAPER writes names with spaces in double quotes, or other quotes when the name holds them.
std::string quoted(const std::string& s) {
    if (s.empty())
        return "\"\"";
    if (s.find_first_of(" \t\"'`") == std::string::npos)
        return s;
    for (char q : {'"', '\'', '`'})
        if (s.find(q) == std::string::npos)
            return std::string(1, q) + s + q;
    std::string t = s;
    std::replace(t.begin(), t.end(), '`', '\'');
    return "`" + t + "`";
}

bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

std::string stem(const std::string& path) {
    std::string p = path;
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    return fs::path(p).stem().string();
}

// What mappings call a plug-in: its label, or its file's name.
std::string labelOf(const PluginSpec& p) { return p.label.empty() ? stem(p.path) : p.label; }

// REAPER's number for each VST3 class, from its plug-in cache:
// "Plugin.vst3=<file id>,602020598{ABCDEF...},Name".
std::map<std::string, std::string> reaperVst3Ids(const std::string& reaperDir) {
    std::map<std::string, std::string> ids;
    std::error_code ec;
    const std::string dir = expandPath(reaperDir);
    if (dir.empty() || !fs::is_directory(dir, ec))
        return ids;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("reaper-vstplugins", 0) != 0 || e.path().extension() != ".ini")
            continue;
        std::ifstream f(e.path());
        std::string line;
        while (std::getline(f, line)) {
            const auto open = line.find('{'), close = line.find('}');
            if (open == std::string::npos || close == std::string::npos || close < open)
                continue;
            size_t start = open;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(line[start - 1])))
                --start;
            if (start < open)
                ids[lower(line.substr(open + 1, close - open - 1))] = line.substr(start, open - start);
        }
    }
    return ids;
}

bool isUid(const std::string& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

struct Writer {
    const Config& config;
    RppExport& out;
    std::string* error;
    std::map<std::string, std::string> vst3Ids;
    std::ostringstream rpp;
    // What was left out, with the chains it was in, in order.
    std::vector<std::pair<std::string, std::vector<std::string>>> left;

    void leave(const std::string& what, const std::string& chain) {
        ++out.skipped;
        for (auto& [w, chains] : left)
            if (w == what) {
                if (chains.back() != chain)
                    chains.push_back(chain);
                return;
            }
        left.push_back({what, {chain}});
    }

    bool state(const PluginSpec& p, bool controller, std::vector<uint8_t>& data) {
        data.clear();
        if (p.embeddedState) {
            data = controller ? p.controllerState : p.state;
            return true;
        }
        const std::string& file = controller ? p.controllerStateFile : p.stateFile;
        if (file.empty())
            return true;
        if (!readFile(file, data)) {
            *error = p.where + ": cannot read state file " + file;
            return false;
        }
        return true;
    }

    // Pins as REAPER lists them: each pin and the channels it reads or writes.
    // Pins left empty are listed too, so REAPER does not map them to its defaults.
    static void pins(std::ostringstream& o, const char* block, const std::vector<std::vector<int>>& map) {
        o << "        <" << block << "\n";
        for (size_t pin = 0; pin < map.size(); ++pin) {
            o << "          PIN " << pin;
            for (int c : map[pin])
                o << " " << c;
            o << "\n";
        }
        o << "        >\n";
    }

    static uint64_t mask(const std::vector<int>& channels) {
        uint64_t m = 0;
        for (int c : channels)
            if (c >= 0 && c < 64)
                m |= 1ull << c;
        return m;
    }

    bool plugin(const PluginSpec& p, const std::string& chain, const std::vector<std::vector<int>>& in, const std::vector<std::vector<int>>& outPins,
                const std::string& seed) {
        const std::string label = labelOf(p);
        std::ostringstream o;
        if (p.kind == "clap") {
            std::vector<uint8_t> st;
            if (!state(p, false, st))
                return false;
            o << "      BYPASS " << (p.bypassed ? 1 : 0) << " 0 0\n";
            o << "      <CLAP " << quoted("CLAP: " + label) << " " << quoted(p.id) << " " << quoted(label) << "\n";
            pins(o, "IN_PINS", in);
            pins(o, "OUT_PINS", outPins);
            if (!st.empty())
                o << "        <STATE\n" << base64Lines(st, "          ") << "        >\n";
            o << "      >\n";
        } else if (p.kind == "vst3") {
            std::vector<uint8_t> comp, ctrl;
            if (!state(p, false, comp) || !state(p, true, ctrl))
                return false;
            std::string uid = p.id;
            uid.erase(std::remove_if(uid.begin(), uid.end(), [](char c) { return c == '{' || c == '}' || c == '-'; }), uid.end());
            if (!isUid(uid)) {
                leave(label + " (VST3 without a class ID in the performance; add it in REAPER)", chain);
                return true;
            }
            std::string number = "0";
            if (auto it = vst3Ids.find(lower(uid)); it != vst3Ids.end())
                number = it->second;
            else
                out.notes.push_back(chain + ": " + label + ": not in REAPER's plug-in list on this machine; REAPER should still find it by its class ID");
            // The body: a header with the pin masks, the plug-in's chunk
            // ([size][1][component][size][0][controller]) and an empty program name.
            const uint32_t id = static_cast<uint32_t>(std::strtoul(number.c_str(), nullptr, 10));
            std::vector<uint8_t> chunk;
            put32(chunk, static_cast<uint32_t>(comp.size()));
            put32(chunk, 1);
            chunk.insert(chunk.end(), comp.begin(), comp.end());
            put32(chunk, static_cast<uint32_t>(ctrl.size()));
            put32(chunk, 0);
            chunk.insert(chunk.end(), ctrl.begin(), ctrl.end());
            std::vector<uint8_t> header;
            put32(header, id);
            put32(header, 0xfeed5eee);
            put32(header, static_cast<uint32_t>(in.size()));
            for (const auto& ch : in)
                put64(header, mask(ch));
            put32(header, static_cast<uint32_t>(outPins.size()));
            for (const auto& ch : outPins)
                put64(header, mask(ch));
            put32(header, static_cast<uint32_t>(chunk.size()));
            put32(header, 1);
            put32(header, 0x8000ffff);
            std::string bundle = p.path;
            while (!bundle.empty() && bundle.back() == '/')
                bundle.pop_back();
            o << "      BYPASS " << (p.bypassed ? 1 : 0) << " 0 0\n";
            // yabridge keeps the vendor's folder: "Neural DSP/Parallax.vst3" is
            // REAPER's "VST3: Parallax (Neural DSP)".
            const std::string vendor = fs::path(bundle).parent_path().filename().string();
            const bool folder = !vendor.empty() && lower(vendor) != "vst3" && lower(vendor) != ".vst3" && lower(vendor) != "yabridge";
            o << "      <VST " << quoted("VST3: " + label + (folder ? " (" + vendor + ")" : "")) << " " << quoted(fs::path(bundle).filename().string()) << " 0 " << quoted(label) << " " << number
              << "{" << uid << "} \"\"\n";
            o << base64Lines(header, "        ") << base64Lines(chunk, "        ") << base64Lines(std::vector<uint8_t>(6, 0), "        ");
            o << "      >\n";
        } else {
            leave(label + " (" + (p.kind == "builtin" ? "a Cue built-in plug-in" : p.kind + " plug-in") + ")", chain);
            return true;
        }
        if (!p.params.empty())
            out.notes.push_back(chain + ": " + label + ": the parameters Cue sets after loading it are not in REAPER's copy; its saved state is");
        if (p.muteOnBypass)
            out.notes.push_back(chain + ": " + label + ": bypass_mode mute has no REAPER equivalent; bypassing it there passes its input");
        o << "      FXID " << guid(seed) << "\n      WAK 0 0\n";
        rpp << o.str();
        ++out.plugins;
        return true;
    }

    bool chain(const SceneSpec& scene, const ChainSpec& c, const std::string& trackName, bool muted, int folder) {
        const std::string where = config.scenes.size() > 1 ? scene.name + " / " + c.name : c.name;
        // Channels: line i on 2i and 2i+1, then a pair for each side-chain feed.
        const auto all = chainPlugins(c);
        std::vector<std::string> labels;
        std::vector<std::pair<size_t, size_t>> slotLine; // slot -> line, index
        for (size_t li = 0; li < c.lines.size(); ++li)
            for (size_t k = 0; k < c.lines[li].plugins.size(); ++k) {
                labels.push_back(labelOf(c.lines[li].plugins[k]));
                slotLine.push_back({li, k});
            }
        std::vector<std::pair<size_t, size_t>> feeds;
        if (!chainSidechains(c, labels, feeds, error))
            return false;
        std::map<std::pair<size_t, size_t>, int> keyOut, keyIn; // (line, index) -> first channel of its key pair
        int channels = 2 * static_cast<int>(c.lines.size());
        // REAPER runs a branch where it leaves its line, so a side-chain only
        // works when its source comes first in that order.
        std::map<std::pair<size_t, size_t>, size_t> order;
        std::function<void(size_t)> number = [&](size_t li) {
            const LineSpec& l = c.lines[li];
            for (size_t k = 0; k <= l.plugins.size(); ++k) {
                for (size_t b = 0; b < c.lines.size(); ++b)
                    if (c.lines[b].parent == static_cast<int>(li) && c.lines[b].after == k)
                        number(b);
                if (k < l.plugins.size())
                    order[{li, k}] = order.size();
            }
        };
        number(0);
        for (auto [slot, src] : feeds) {
            const auto to = slotLine[slot], from = slotLine[src];
            if (order[from] > order[to]) {
                out.notes.push_back(where + ": " + labels[slot] + " takes its side-chain from " + labels[src] +
                                    ", which REAPER runs after it; left without a side-chain");
                continue;
            }
            if (!keyOut.count(from)) {
                keyOut[from] = channels;
                channels += 2;
            }
            keyIn[to] = keyOut[from];
        }
        if (channels > 128) {
            *error = where + ": needs " + std::to_string(channels) + " channels, more than REAPER's 128";
            return false;
        }

        const std::string id = guid("track " + scene.name + "\n" + c.name);
        rpp << "  <TRACK " << id << "\n";
        rpp << "    NAME " << quoted(trackName) << "\n";
        rpp << "    VOLPAN 1 0 -1 -1 1\n";
        rpp << "    MUTESOLO " << (muted ? 1 : 0) << " 0 0\n";
        rpp << "    ISBUS " << (folder < 0 ? "2 " + std::to_string(folder) : "0 0") << "\n";
        const unsigned input = c.inputIndex < config.audio.inputChannels.size() ? config.audio.inputChannels[c.inputIndex] : c.inputIndex;
        rpp << "    REC 1 " << input << " 1 2 0 0 0 0\n";
        rpp << "    NCHAN " << std::max(2, channels) << "\n";
        rpp << "    FX 1\n";
        rpp << "    TRACKID " << id << "\n";
        rpp << "    MAINSEND 0 0\n";
        // Each output: a mono hardware send of the left channel of the line
        // it is tapped on, as Adam's template does.
        for (size_t o = 0; o < config.outputs.size() && o < c.out.size(); ++o) {
            const TapSpec& t = c.out[o];
            int line = t.line;
            const LineSpec& l = c.lines.at(line);
            const size_t after = std::min(t.after, l.plugins.size());
            if (after < l.plugins.size()) {
                out.notes.push_back(where + ": output " + config.outputs[o] + " is taken after " + std::to_string(after) + " plug-in(s) of " +
                                    (l.name.empty() ? "the main line" : l.name) + "; REAPER takes the end of that line");
            } else if (l.plugins.empty() && l.parent >= 0) {
                // An empty branch is its parent's signal where it leaves.
                const LineSpec& parent = c.lines[l.parent];
                if (l.after < parent.plugins.size())
                    out.notes.push_back(where + ": output " + config.outputs[o] + " takes the empty branch " + l.name +
                                        "; REAPER takes the end of the line it leaves");
                line = l.parent;
            }
            const unsigned hw = o < config.audio.outputChannels.size() ? config.audio.outputChannels[o] : static_cast<unsigned>(o);
            rpp << "    HWOUT " << (1024 + hw) << " 0 1 0 0 0 " << (1024 + 2 * line) << " -1:U -1\n";
        }
        rpp << "    <FXCHAIN\n      SHOW 0\n      LASTSEL 0\n      DOCKED 0\n";
        bool ok = true;
        std::function<void(size_t)> emit = [&](size_t li) {
            const LineSpec& l = c.lines[li];
            const int own = 2 * static_cast<int>(li);
            for (size_t k = 0; k <= l.plugins.size() && ok; ++k) {
                for (size_t b = 0; b < c.lines.size() && ok; ++b)
                    if (c.lines[b].parent == static_cast<int>(li) && c.lines[b].after == k)
                        emit(b);
                if (k == l.plugins.size() || !ok)
                    continue;
                // A branch's first plug-in reads its parent's channels where it leaves.
                int from = own;
                if (k == 0 && l.parent >= 0) {
                    int p = l.parent;
                    // An empty parent branch is its own parent's signal.
                    while (p >= 0 && c.lines[p].plugins.empty() && c.lines[p].parent >= 0)
                        p = c.lines[p].parent;
                    from = 2 * p;
                }
                std::vector<std::vector<int>> in(8), outPins(8);
                in[0] = {from};
                in[1] = {from + 1};
                if (auto it = keyIn.find({li, k}); it != keyIn.end()) {
                    in[2] = {it->second};
                    in[3] = {it->second + 1};
                }
                outPins[0] = {own};
                outPins[1] = {own + 1};
                if (auto it = keyOut.find({li, k}); it != keyOut.end()) {
                    outPins[0].push_back(it->second);
                    outPins[1].push_back(it->second + 1);
                }
                ok = plugin(l.plugins[k], where, in, outPins, id + std::to_string(li) + "/" + std::to_string(k));
            }
        };
        emit(0);
        if (!ok)
            return false;
        rpp << "    >\n  >\n";
        ++out.tracks;
        return true;
    }

    bool project() {
        rpp << "<REAPER_PROJECT 0.1 \"7.0/linux-x86_64\" 0 0\n";
        rpp << "  SAMPLERATE " << config.audio.rate << " 0 0\n";
        rpp << "  TEMPO 120 4 4 0\n";
        rpp << "  MASTER_NCH 2 2\n";
        const bool folders = config.scenes.size() > 1;
        bool first = true;
        for (const SceneSpec& s : config.scenes) {
            if (folders) {
                const std::string id = guid("scene " + s.name);
                rpp << "  <TRACK " << id << "\n    NAME " << quoted(s.name) << "\n    ISBUS 1 1\n    TRACKID " << id << "\n    MAINSEND 0 0\n  >\n";
                ++out.tracks;
            }
            for (size_t k = 0; k < s.chains.size(); ++k) {
                const bool last = folders && k + 1 == s.chains.size();
                if (!chain(s, s.chains[k], s.chains[k].name, !first, last ? -1 : 0))
                    return false;
                first = false;
            }
        }
        rpp << ">\n";
        return true;
    }
};

} // namespace

bool exportRpp(const Config& config, RppExport& out, std::string* error, const std::string& reaperDir) {
    out = RppExport{};
    if (config.scenes.empty() || config.scenes[0].chains.empty()) {
        *error = "the performance has no chains";
        return false;
    }
    Writer w{config, out, error, reaperVst3Ids(reaperDir), {}, {}};
    if (!w.project())
        return false;
    for (const auto& [what, chains] : w.left) {
        std::string in;
        for (size_t i = 0; i < chains.size(); ++i)
            in += (i == 0 ? "" : i + 1 == chains.size() ? " and " : ", ") + chains[i];
        out.notes.push_back("left out " + what + " in " + in);
    }
    if (config.mix == OutputMix::Sum)
        out.notes.push_back("each output takes the left channel of its line (REAPER's mono hardware sends); Cue summed both");
    size_t chains = 0;
    for (const auto& s : config.scenes)
        chains += s.chains.size();
    if (chains > 1)
        out.notes.push_back("REAPER plays every track at once: all chains but the first are muted, unmute one to play it");
    out.notes.push_back("warm chains, crossfades and pedals are not in the REAPER project");
    out.text = w.rpp.str();
    return true;
}

} // namespace cue
