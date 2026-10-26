#include "app/cuefile.h"

#include "miniz.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sys/stat.h>

namespace cue {

using json = nlohmann::ordered_json;

namespace {

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool same(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool endsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && lower(s.substr(s.size() - end.size())) == end;
}

std::string dirOf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// Paths under the home folder are written with ~, so a performance moves to
// a machine where the user has another name.
std::string portablePath(const std::string& path) {
    const std::string h = home();
    if (!h.empty() && path.size() > h.size() && path.compare(0, h.size(), h) == 0 && path[h.size()] == '/')
        return "~" + path.substr(h.size());
    return path;
}

std::string localPath(const std::string& path, const std::string& base) {
    std::string p = expandPath(path);
    if (!p.empty() && p[0] != '/')
        p = base + "/" + p;
    return p;
}

bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    data.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// A name that is safe as one part of a path inside the zip.
std::string fileName(const std::string& s) {
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.' ? c : '_';
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out.empty() ? "_" : out;
}

std::string number(size_t n, int width = 2) {
    std::string s = std::to_string(n);
    return std::string(s.size() < static_cast<size_t>(width) ? width - s.size() : 0, '0') + s;
}

std::string pluginStem(const PluginSpec& p) {
    if (!p.label.empty())
        return p.label;
    if (p.kind == "builtin")
        return p.args.empty() ? "builtin" : p.args[0];
    std::string name = p.path.substr(p.path.rfind('/') + 1);
    const auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

const char* mixName(OutputMix m) { return m == OutputMix::Left ? "left" : m == OutputMix::Right ? "right" : "sum"; }

std::vector<std::string> texts(const std::vector<ConfigText>& v) {
    std::vector<std::string> out;
    for (const auto& t : v)
        out.push_back(t.text);
    return out;
}

std::vector<ConfigText> texts(const json& j, const char* key, const std::string& where) {
    std::vector<ConfigText> out;
    if (j.contains(key))
        for (const auto& t : j.at(key))
            out.push_back({t.get<std::string>(), where});
    return out;
}

// ----------------------------------------------------------------- writing

struct Writer {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files; // states/..., in order
    std::string* error;
    bool withStates = true;

    // The state's path in the zip, or "" if the plug-in has none.
    bool state(const PluginSpec& p, bool controller, const std::string& name, std::string& out) {
        out.clear();
        if (!withStates)
            return true;
        const std::string& file = controller ? p.controllerStateFile : p.stateFile;
        std::vector<uint8_t> data;
        if (p.embeddedState) {
            data = controller ? p.controllerState : p.state;
            if (data.empty() && file.empty())
                return true;
        } else {
            if (file.empty())
                return true;
            if (!readFile(file, data)) {
                *error = p.where + ": cannot read state file " + file;
                return false;
            }
        }
        out = name + (p.kind == "vst3" ? (controller ? ".vst3ctrl" : ".vst3state") : ".clapstate");
        files.push_back({out, std::move(data)});
        return true;
    }

    bool plugin(const PluginSpec& p, const std::string& stateName, json& j) {
        j["format"] = p.kind;
        if (p.kind == "builtin") {
            j["args"] = p.args;
        } else {
            j["path"] = portablePath(p.path);
            if (!p.id.empty())
                j["id"] = p.id;
        }
        if (!p.label.empty())
            j["label"] = p.label;
        j["bypassed"] = p.bypassed;
        j["bypass_mode"] = p.muteOnBypass ? "mute" : "thru";
        j["midi"] = p.midi;
        if (!p.sidechain.empty())
            j["sidechain"] = p.sidechain;
        if (!p.params.empty()) {
            json params = json::object();
            for (auto [id, v] : p.params)
                params[std::to_string(id)] = v;
            j["params"] = params;
        }
        std::string name;
        if (!state(p, false, stateName, name))
            return false;
        if (!name.empty())
            j["state"] = name;
        if (p.kind == "vst3") {
            if (!state(p, true, stateName, name))
                return false;
            if (!name.empty())
                j["controller_state"] = name;
        }
        return true;
    }

    bool chain(const ChainSpec& c, const PresetSpec& p, const Config& config, const std::string& stateDir, json& j) {
        j["name"] = c.name;
        if (!p.display.empty())
            j["display"] = p.display;
        if (p.pedal)
            j["pedal"] = p.pedal;
        if (p.program >= 0)
            j["program"] = p.program;
        j["on_select"] = texts(p.actions);
        j["warm"] = p.warm.empty() ? config.warm : p.warm;
        if (config.inputs.size() > 1)
            j["input"] = config.inputs.at(c.inputIndex);
        json lines = json::array();
        size_t slot = 0;
        for (const LineSpec& l : c.lines) {
            json lj;
            if (l.parent >= 0) {
                lj["name"] = l.name;
                lj["from"] = c.lines[l.parent].name;
                lj["after"] = l.after;
            }
            json plugins = json::array();
            for (const PluginSpec& ps : l.plugins) {
                json pj;
                if (!plugin(ps, stateDir + "/" + number(++slot) + " " + fileName(pluginStem(ps)), pj))
                    return false;
                plugins.push_back(pj);
            }
            lj["plugins"] = plugins;
            lines.push_back(lj);
        }
        j["lines"] = lines;
        json taps = json::object();
        for (size_t o = 0; o < config.outputs.size() && o < c.out.size(); ++o) {
            const TapSpec& t = c.out[o];
            const LineSpec& l = c.lines.at(t.line);
            taps[config.outputs[o]] = {{"line", l.name}, {"after", std::min(t.after, l.plugins.size())}};
        }
        j["outputs"] = taps;
        return true;
    }

    bool performance(const Config& c, json& j) {
        j["cue"] = kCueFormat;
        j["saved_by"] = std::string("Cue ") + CUE_VERSION;
        j["name"] = c.performance;
        j["inputs"] = c.inputs;
        j["outputs"] = c.outputs;
        j["output_mix"] = mixName(c.mix);
        const EngineSettings& e = c.engine;
        j["switching"] = {{"preroll_ms", e.prerollMs},          {"crossfade_ms", e.crossfadeMs},
                          {"tail_cap_ms", e.tailCapMs},          {"tail_hold_ms", e.tailHoldMs},
                          {"tail_quiet_db", e.tailQuietDb},      {"bypass_fade_ms", e.bypassFadeMs},
                          {"mute_fade_ms", e.muteFadeMs},        {"reset_tails_on_warm", e.resetTailsOnWarmSwitch}};
        j["controller"] = {{"profile", c.midi.profile}, {"channel", c.midi.channel + 1}, {"forward", c.midi.forward}};
        const TunerSettings& t = c.tuner.settings;
        j["tuner"] = {{"input", c.inputs.at(c.tuner.inputIndex)},
                      {"reference", t.reference},
                      {"range", tunerRangeName(t.range)},
                      {"tolerance", t.tolerance},
                      {"names", t.flats ? "flats" : "sharps"}};

        json scenes = json::array();
        for (size_t s = 0; s < c.scenes.size(); ++s) {
            const SceneSpec& sc = c.scenes[s];
            json sj;
            sj["name"] = sc.name;
            if (!sc.display.empty())
                sj["display"] = sc.display;
            if (sc.pedal)
                sj["pedal"] = sc.pedal;
            if (sc.program >= 0)
                sj["program"] = sc.program;
            json chains = json::array();
            for (size_t k = 0; k < sc.chains.size(); ++k) {
                json cj;
                const std::string dir = "states/" + number(s + 1) + " " + fileName(sc.name) + "/" + number(k + 1) + " " + fileName(sc.chains[k].name);
                if (!chain(sc.chains[k], sc.presets.at(k), c, dir, cj))
                    return false;
                chains.push_back(cj);
            }
            sj["chains"] = chains;
            scenes.push_back(sj);
        }
        j["scenes"] = scenes;

        json controls = json::array();
        for (const ControlSpec& ct : c.controls) {
            json cj;
            cj["name"] = ct.name;
            const bool toggle = ct.type == ControlSpec::Type::Toggle;
            cj["type"] = toggle ? "toggle" : ct.type == ControlSpec::Type::Continuous ? "continuous" : "discrete";
            if (!toggle && !ct.labels.empty()) {
                cj["values"] = ct.labels;
            } else if (!toggle) {
                cj["min"] = ct.min;
                cj["max"] = ct.max;
                cj["step"] = ct.step;
            }
            cj["default"] = ct.def;
            cj["wrap"] = ct.wrap;
            if (ct.pedal)
                cj["pedal"] = ct.pedal;
            if (!ct.follows.text.empty())
                cj["follows"] = ct.follows.text;
            cj["enable"] = texts(ct.enables);
            cj["drive"] = texts(ct.drives);
            cj["mute"] = texts(ct.mutes);
            controls.push_back(cj);
        }
        j["controls"] = controls;

        json triggers = json::array();
        for (const TriggerSpec& t : c.triggers) {
            json tj;
            tj["name"] = t.name;
            if (!t.scene.empty())
                tj["scene"] = t.scene;
            if (!t.when.text.empty())
                tj["when"] = t.when.text;
            tj["on"] = texts(t.inputs);
            tj["do"] = texts(t.actions);
            triggers.push_back(tj);
        }
        j["triggers"] = triggers;

        json groups = json::array();
        for (const GroupSpec& g : c.groups)
            groups.push_back({{"name", g.name}, {"do", texts(g.actions)}});
        j["groups"] = groups;

        json leds = json::array();
        for (const auto& [led, when] : c.fcb.leds)
            leds.push_back({{"led", led}, {"when", when.text}});
        j["fcb"] = {{"leds", leds}, {"displays", texts(c.fcb.displays)}};
        return true;
    }
};

bool writeZip(const std::string& path, const std::string& performance, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files,
              std::string* error) {
    // Written beside the file and renamed over it, so a failed save leaves
    // the old one whole.
    const std::string tmp = path + ".saving";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmp.c_str(), 0)) {
        *error = "cannot write " + tmp;
        return false;
    }
    bool ok = mz_zip_writer_add_mem(&zip, "performance.json", performance.data(), performance.size(), MZ_DEFAULT_COMPRESSION);
    for (const auto& [name, data] : files)
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    if (!ok)
        *error = "writing " + path + ": " + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    mz_zip_writer_end(&zip);
    if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) {
        *error = "cannot replace " + path;
        ok = false;
    }
    if (!ok)
        std::remove(tmp.c_str());
    return ok;
}

// ----------------------------------------------------------------- reading

struct Reader {
    mz_zip_archive zip{};
    std::string path;

    ~Reader() { mz_zip_reader_end(&zip); }

    bool file(const std::string& name, std::vector<uint8_t>& out) {
        size_t size = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&zip, name.c_str(), &size, 0);
        if (!p)
            return false;
        out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
        mz_free(p);
        return true;
    }

    void plugin(const json& j, const std::string& where, PluginSpec& p) {
        p.where = where;
        p.kind = lower(j.at("format").get<std::string>());
        if (p.kind == "builtin") {
            p.args = j.at("args").get<std::vector<std::string>>();
            if (p.args.empty())
                throw std::runtime_error("a builtin plug-in needs its name in args");
            p.text = "builtin:";
            for (const auto& a : p.args)
                p.text += (p.text.back() == ':' ? "" : " ") + a;
        } else if (p.kind == "clap" || p.kind == "vst3") {
            p.path = localPath(j.at("path").get<std::string>(), dirOf(path));
            p.id = j.value("id", "");
            p.text = p.kind + ":" + j.at("path").get<std::string>() + (p.id.empty() ? "" : "#" + p.id);
        } else {
            throw std::runtime_error("unknown plug-in format '" + p.kind + "' (clap, vst3 or builtin)");
        }
        p.label = j.value("label", "");
        p.bypassed = j.value("bypassed", false);
        const std::string mode = lower(j.value("bypass_mode", "thru"));
        if (mode != "thru" && mode != "mute")
            throw std::runtime_error("bypass_mode is thru or mute");
        p.muteOnBypass = mode == "mute";
        p.bypassModeSet = true;
        p.midi = j.value("midi", true);
        p.sidechain = j.value("sidechain", "");
        if (j.contains("params"))
            for (const auto& [id, v] : j.at("params").items()) {
                size_t end = 0;
                unsigned long n = 0;
                try {
                    n = std::stoul(id, &end);
                } catch (const std::exception&) {
                    end = 0;
                }
                if (end == 0 || end != id.size() || n > UINT32_MAX)
                    throw std::runtime_error("parameter ids are whole numbers, like \"54\": 1");
                p.params.push_back({static_cast<uint32_t>(n), v.get<double>()});
            }
        for (const char* key : {"state", "controller_state"}) {
            if (!j.contains(key))
                continue;
            const std::string name = j.at(key).get<std::string>();
            const bool controller = key[0] == 'c';
            if (!file(name, controller ? p.controllerState : p.state))
                throw std::runtime_error("its state " + name + " is missing from the file");
            (controller ? p.controllerStateFile : p.stateFile) = path + ":" + name;
            p.embeddedState = true;
        }
    }

    int lineIndex(const ChainSpec& c, const std::string& name, size_t before) {
        for (size_t i = 0; i < before && i < c.lines.size(); ++i)
            if (same(c.lines[i].name, name))
                return static_cast<int>(i);
        return -1;
    }

    void chain(const json& j, const Config& config, const std::string& where, ChainSpec& c, PresetSpec& p) {
        c.name = j.at("name").get<std::string>();
        if (c.name.empty())
            throw std::runtime_error("a chain needs a name");
        c.where = where + " chain '" + c.name + "'";
        const auto& lines = j.at("lines");
        if (lines.empty())
            throw std::runtime_error("chain '" + c.name + "' has no lines");
        size_t slot = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const json& lj = lines[i];
            LineSpec l;
            if (i > 0) {
                l.name = lj.at("name").get<std::string>();
                if (l.name.empty() || lineIndex(c, l.name, i) >= 0)
                    throw std::runtime_error("chain '" + c.name + "': each branch needs a name of its own");
                l.parent = lineIndex(c, lj.value("from", ""), i);
                if (l.parent < 0)
                    throw std::runtime_error("chain '" + c.name + "': branch '" + l.name + "' comes from '" + lj.value("from", "") +
                                             "', which is not an earlier line");
                l.after = lj.at("after").get<size_t>();
                if (l.after > c.lines[l.parent].plugins.size())
                    throw std::runtime_error("chain '" + c.name + "': branch '" + l.name + "' leaves after more plug-ins than its line has");
            }
            for (const json& pj : lj.at("plugins")) {
                PluginSpec ps;
                plugin(pj, c.where + " plug-in " + std::to_string(++slot), ps);
                l.plugins.push_back(std::move(ps));
            }
            c.lines.push_back(std::move(l));
        }
        c.input = j.value("input", "");
        if (j.contains("outputs"))
            for (const auto& [name, tj] : j.at("outputs").items()) {
                bool known = false;
                for (const auto& o : config.outputs)
                    known = known || same(o, name);
                if (!known)
                    throw std::runtime_error("chain '" + c.name + "' feeds output '" + name + "', which the performance does not have");
                TapSpec t;
                t.output = name;
                t.where = c.where;
                t.line = lineIndex(c, tj.value("line", ""), c.lines.size());
                if (t.line < 0)
                    throw std::runtime_error("chain '" + c.name + "': output '" + name + "' is taken from line '" + tj.value("line", "") +
                                             "', which the chain does not have");
                t.after = std::min(tj.value("after", SIZE_MAX), c.lines[t.line].plugins.size());
                t.set = true;
                c.out.push_back(t);
            }

        p.name = c.name;
        p.chain = c.name;
        p.where = c.where;
        p.display = j.value("display", "");
        p.pedal = j.value("pedal", 0);
        p.program = j.value("program", -1);
        p.actions = texts(j, "on_select", c.where);
        p.warm = j.value("warm", "next");
    }

    void performance(const json& j, Config& c) {
        const int format = j.at("cue").get<int>();
        if (format > kCueFormat)
            throw std::runtime_error("made by a newer Cue (" + j.value("saved_by", std::string("format ") + std::to_string(format)) +
                                     "); this one reads format " + std::to_string(kCueFormat));
        c.performance = j.value("name", "");
        c.inputs = j.value("inputs", c.inputs);
        c.outputs = j.value("outputs", c.outputs);
        const std::string mix = lower(j.value("output_mix", "sum"));
        if (mix != "left" && mix != "right" && mix != "sum")
            throw std::runtime_error("output_mix is left, right or sum");
        c.mix = mix == "left" ? OutputMix::Left : mix == "right" ? OutputMix::Right : OutputMix::Sum;
        if (j.contains("switching")) {
            const json& s = j.at("switching");
            EngineSettings& e = c.engine;
            e.prerollMs = s.value("preroll_ms", e.prerollMs);
            e.crossfadeMs = s.value("crossfade_ms", e.crossfadeMs);
            e.tailCapMs = s.value("tail_cap_ms", e.tailCapMs);
            e.tailHoldMs = s.value("tail_hold_ms", e.tailHoldMs);
            e.tailQuietDb = s.value("tail_quiet_db", e.tailQuietDb);
            e.bypassFadeMs = s.value("bypass_fade_ms", e.bypassFadeMs);
            e.muteFadeMs = s.value("mute_fade_ms", e.muteFadeMs);
            e.resetTailsOnWarmSwitch = s.value("reset_tails_on_warm", e.resetTailsOnWarmSwitch);
        }
        if (j.contains("controller")) {
            const json& m = j.at("controller");
            c.midi.profile = lower(m.value("profile", c.midi.profile));
            if (c.midi.profile != "eurekaprom" && c.midi.profile != "none")
                throw std::runtime_error("controller profile is eurekaprom or none");
            const int channel = m.value("channel", 1);
            if (channel < 1 || channel > 16)
                throw std::runtime_error("controller channel is 1 to 16");
            c.midi.channel = static_cast<uint8_t>(channel - 1);
            c.midi.forward = m.value("forward", c.midi.forward);
        }
        if (j.contains("tuner")) {
            const json& t = j.at("tuner");
            TunerSettings& s = c.tuner.settings;
            c.tuner.input = t.value("input", "");
            s.reference = t.value("reference", s.reference);
            if (s.reference < 415 || s.reference > 466)
                throw std::runtime_error("tuner reference is 415 to 466 Hz");
            const int range = tunerRange(lower(t.value("range", std::string(tunerRangeName(s.range)))));
            if (range < 0)
                throw std::runtime_error("tuner range is guitar, bass, vocals or any");
            s.range = range;
            s.tolerance = t.value("tolerance", s.tolerance);
            if (s.tolerance <= 0 || s.tolerance > 10)
                throw std::runtime_error("tuner tolerance is above 0 and at most 10 cents");
            const std::string names = lower(t.value("names", std::string("sharps")));
            if (names != "sharps" && names != "flats")
                throw std::runtime_error("tuner names are sharps or flats");
            s.flats = names == "flats";
        }

        for (const json& sj : j.at("scenes")) {
            SceneSpec s;
            s.name = sj.at("name").get<std::string>();
            s.where = path + ": scene '" + s.name + "'";
            s.display = sj.value("display", "");
            s.pedal = sj.value("pedal", 0);
            s.program = sj.value("program", -1);
            for (const json& cj : sj.at("chains")) {
                ChainSpec ch;
                PresetSpec p;
                chain(cj, c, s.where, ch, p);
                for (const auto& other : s.chains)
                    if (same(other.name, ch.name))
                        throw std::runtime_error("scene '" + s.name + "' has two chains named " + ch.name);
                s.chains.push_back(std::move(ch));
                s.presets.push_back(std::move(p));
            }
            c.scenes.push_back(std::move(s));
        }

        for (const json& cj : j.value("controls", json::array())) {
            ControlSpec ct;
            ct.name = cj.at("name").get<std::string>();
            ct.where = path + ": control '" + ct.name + "'";
            const std::string type = lower(cj.value("type", "discrete"));
            if (type == "toggle") ct.type = ControlSpec::Type::Toggle;
            else if (type == "discrete") ct.type = ControlSpec::Type::Discrete;
            else if (type == "continuous") ct.type = ControlSpec::Type::Continuous;
            else throw std::runtime_error("control '" + ct.name + "': type is toggle, discrete or continuous");
            ct.typeSet = true;
            ct.labels = cj.value("values", std::vector<std::string>{});
            if (cj.contains("min") || cj.contains("max")) {
                ct.min = cj.value("min", ct.min);
                ct.max = cj.value("max", ct.max);
                ct.rangeSet = true;
            }
            if (cj.contains("step")) {
                ct.step = cj.at("step").get<double>();
                ct.stepSet = true;
            }
            if (cj.contains("default")) {
                ct.def = cj.at("default").get<double>();
                ct.defSet = true;
            }
            ct.wrap = cj.value("wrap", true);
            ct.pedal = cj.value("pedal", 0);
            if (cj.contains("follows"))
                ct.follows = {cj.at("follows").get<std::string>(), ct.where};
            ct.enables = texts(cj, "enable", ct.where);
            ct.drives = texts(cj, "drive", ct.where);
            ct.mutes = texts(cj, "mute", ct.where);
            c.controls.push_back(std::move(ct));
        }
        for (const json& tj : j.value("triggers", json::array())) {
            TriggerSpec t;
            t.name = tj.value("name", "trigger " + std::to_string(c.triggers.size() + 1));
            t.where = path + ": trigger '" + t.name + "'";
            t.scene = tj.value("scene", "");
            if (tj.contains("when"))
                t.when = {tj.at("when").get<std::string>(), t.where};
            t.inputs = texts(tj, "on", t.where);
            t.actions = texts(tj, "do", t.where);
            c.triggers.push_back(std::move(t));
        }
        for (const json& gj : j.value("groups", json::array())) {
            GroupSpec g;
            g.name = gj.at("name").get<std::string>();
            g.where = path + ": group '" + g.name + "'";
            g.actions = texts(gj, "do", g.where);
            c.groups.push_back(std::move(g));
        }
        if (j.contains("fcb")) {
            const json& f = j.at("fcb");
            for (const json& lj : f.value("leds", json::array())) {
                const int led = lj.at("led").get<int>();
                if (led < 1 || led > 23)
                    throw std::runtime_error("fcb LEDs are numbered 1 to 23");
                c.fcb.leds.push_back({led, {lj.at("when").get<std::string>(), path + ": fcb led " + std::to_string(led)}});
            }
            c.fcb.displays = texts(f, "displays", path + ": fcb display");
        }
    }
};

} // namespace

bool isCueFile(const std::string& path) { return endsWith(path, ".cue"); }

bool presetsToChains(Config& config, std::string* error) {
    for (SceneSpec& s : config.scenes) {
        // Which presets select each chain.
        std::vector<std::vector<size_t>> by(s.chains.size());
        for (size_t p = 0; p < s.presets.size(); ++p)
            for (size_t c = 0; c < s.chains.size(); ++c)
                if (same(s.chains[c].name, s.presets[p].chain))
                    by[c].push_back(p);
        for (size_t c = 0; c < s.chains.size(); ++c)
            if (by[c].size() > 1) {
                *error = s.chains[c].where + ": chain '" + s.chains[c].name + "' is played by presets '" + s.presets[by[c][0]].name + "' and '" +
                         s.presets[by[c][1]].name + "'. Each chain now has its own pedal and actions: give each preset a copy of the chain";
                return false;
            }

        // Chains in their presets' order, each named after its preset; then
        // the chains no preset played, with a preset of their own.
        std::vector<ChainSpec> chains;
        std::vector<PresetSpec> presets;
        std::map<std::string, std::string> renamed; // old name (lower case) -> new
        for (const PresetSpec& p : s.presets)
            for (size_t c = 0; c < s.chains.size(); ++c)
                if (same(s.chains[c].name, p.chain)) {
                    renamed[lower(s.chains[c].name)] = p.name;
                    chains.push_back(s.chains[c]);
                    chains.back().name = p.name;
                    presets.push_back(p);
                    if (!same(p.name, s.chains[c].name))
                        config.warnings.push_back(p.where + ": chain '" + s.chains[c].name + "' is now called '" + p.name + "', after its preset");
                }
        for (size_t c = 0; c < s.chains.size(); ++c)
            if (by[c].empty()) {
                renamed[lower(s.chains[c].name)] = s.chains[c].name;
                chains.push_back(s.chains[c]);
                PresetSpec p;
                p.name = p.chain = s.chains[c].name;
                p.where = s.chains[c].where;
                presets.push_back(p);
                config.warnings.push_back(s.chains[c].where + ": chain '" + s.chains[c].name +
                                          "' had no preset; it now has its own, so stepping through the chains reaches it too");
            }
        for (size_t a = 0; a < chains.size(); ++a)
            for (size_t b = a + 1; b < chains.size(); ++b)
                if (same(chains[a].name, chains[b].name)) {
                    *error = chains[b].where + ": two chains of scene '" + s.name + "' would be called '" + chains[b].name +
                             "'; rename the chain or the preset";
                    return false;
                }
        for (PresetSpec& p : presets) {
            p.chain = p.name;
            if (p.warm.empty())
                p.warm = config.warm;
            auto it = renamed.find(lower(p.warm));
            if (it != renamed.end() && !same(p.warm, "next") && !same(p.warm, "none"))
                p.warm = it->second;
        }
        s.chains = std::move(chains);
        s.presets = std::move(presets);
    }
    return true;
}

bool saveCue(const Config& config, const std::string& path, std::string* error, std::vector<std::string>* warnings) {
    Config c = config;
    c.warnings.clear();
    if (!presetsToChains(c, error))
        return false;
    if (warnings)
        *warnings = c.warnings;
    Writer w{{}, error};
    json j;
    if (!w.performance(c, j))
        return false;
    return writeZip(path, j.dump(2) + "\n", w.files, error);
}

bool performanceText(const Config& config, std::string& text, std::string* error) {
    Config c = config;
    if (!presetsToChains(c, error))
        return false;
    Writer w{{}, error, false};
    json j;
    if (!w.performance(c, j))
        return false;
    text = j.dump(2) + "\n";
    return true;
}

bool loadCue(const std::string& path, Config& config, std::string* error) {
    Reader r;
    r.path = path;
    if (!mz_zip_reader_init_file(&r.zip, path.c_str(), 0)) {
        struct stat st;
        *error = stat(path.c_str(), &st) == 0 ? path + " is not a .cue file (" + mz_zip_get_error_string(mz_zip_get_last_error(&r.zip)) + ")"
                                              : "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> text;
    if (!r.file("performance.json", text)) {
        *error = path + ": no performance.json inside";
        return false;
    }
    Config c;
    c.audio = config.audio;
    c.cpuAuto = config.cpuAuto;
    c.workers = config.workers;
    c.midi.enabled = config.midi.enabled;
    c.midi.in = config.midi.in;
    c.midi.out = config.midi.out;
    c.path = path;
    try {
        r.performance(json::parse(text.begin(), text.end()), c);
    } catch (const std::exception& e) {
        *error = path + ": " + e.what();
        return false;
    }
    if (!finishConfig(c, error))
        return false;
    config = std::move(c);
    return true;
}

std::string defaultRigPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string base = xdg && *xdg ? xdg : home() + "/.config";
    return base + "/cue/rig.json";
}

bool saveRig(const Config& c, const std::string& path, std::string* error) {
    json j;
    j["cue_rig"] = 1;
    j["audio"] = {{"device", c.audio.device},
                  {"rate", c.audio.rate},
                  {"period", c.audio.period},
                  {"periods", c.audio.periods},
                  {"priority", c.audio.rtPriority},
                  {"workers", c.workers}};
    if (c.cpuAuto)
        j["audio"]["cpu"] = "auto";
    else if (c.audio.cpu < 0)
        j["audio"]["cpu"] = "none";
    else
        j["audio"]["cpu"] = c.audio.cpu;
    json ins = json::object(), outs = json::object();
    for (size_t i = 0; i < c.inputs.size(); ++i)
        ins[c.inputs[i]] = i < c.audio.inputChannels.size() ? c.audio.inputChannels[i] + 1 : i + 1;
    for (size_t o = 0; o < c.outputs.size(); ++o)
        outs[c.outputs[o]] = o < c.audio.outputChannels.size() ? c.audio.outputChannels[o] + 1 : o + 1;
    j["inputs"] = ins;
    j["outputs"] = outs;
    j["midi"] = {{"enabled", c.midi.enabled}, {"in", c.midi.in}, {"out", c.midi.out}};
    const std::string text = j.dump(2) + "\n";
    const std::string tmp = path + ".saving";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!(f << text)) {
            *error = "cannot write " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        *error = "cannot replace " + path;
        return false;
    }
    return true;
}

bool loadRig(const std::string& path, Config& c, std::string* error) {
    std::ifstream f(path);
    if (!f) {
        *error = "cannot open " + path;
        return false;
    }
    try {
        const json j = json::parse(f);
        if (j.value("cue_rig", 1) > 1)
            throw std::runtime_error("made by a newer Cue");
        if (j.contains("audio")) {
            const json& a = j.at("audio");
            c.audio.device = a.value("device", c.audio.device);
            c.audio.rate = a.value("rate", c.audio.rate);
            c.audio.period = a.value("period", c.audio.period);
            c.audio.periods = a.value("periods", c.audio.periods);
            c.audio.rtPriority = a.value("priority", c.audio.rtPriority);
            c.workers = a.value("workers", c.workers);
            if (c.workers < 0 || c.workers > 8)
                throw std::runtime_error("workers is 0 to 8");
            if (a.contains("cpu")) {
                const json& cpu = a.at("cpu");
                if (cpu.is_number()) {
                    c.cpuAuto = false;
                    c.audio.cpu = cpu.get<int>();
                } else if (lower(cpu.get<std::string>()) == "auto") {
                    c.cpuAuto = true;
                } else if (lower(cpu.get<std::string>()) == "none") {
                    c.cpuAuto = false;
                    c.audio.cpu = -1;
                } else {
                    throw std::runtime_error("cpu is auto, none or a CPU number");
                }
            }
        }
        // Each of the performance's inputs and outputs by name, else by position.
        auto channel = [&](const char* key, const std::string& name, size_t position) {
            if (j.contains(key))
                for (const auto& [n, ch] : j.at(key).items())
                    if (same(n, name)) {
                        const int v = ch.get<int>();
                        if (v < 1)
                            throw std::runtime_error(std::string(key) + ": channels are numbered from 1");
                        return static_cast<unsigned>(v - 1);
                    }
            c.warnings.push_back(path + ": no channel for " + (key[0] == 'i' ? "input" : "output") + " '" + name + "'; using channel " +
                                 std::to_string(position + 1));
            return static_cast<unsigned>(position);
        };
        c.audio.inputChannels.clear();
        for (size_t i = 0; i < c.inputs.size(); ++i)
            c.audio.inputChannels.push_back(channel("inputs", c.inputs[i], i));
        c.audio.outputChannels.clear();
        for (size_t o = 0; o < c.outputs.size(); ++o)
            c.audio.outputChannels.push_back(channel("outputs", c.outputs[o], o));
        if (j.contains("midi")) {
            const json& m = j.at("midi");
            c.midi.enabled = m.value("enabled", c.midi.enabled);
            c.midi.in = m.value("in", c.midi.in);
            c.midi.out = m.value("out", std::string());
            if (c.midi.out.empty() && lower(c.midi.in) != "any")
                c.midi.out = c.midi.in;
        }
    } catch (const std::exception& e) {
        *error = path + ": " + e.what();
        return false;
    }
    return true;
}

bool loadPerformance(const std::string& path, const std::string& rigPath, Config& config, std::string* error) {
    if (!isCueFile(path))
        return loadConfig(path, config, error);
    if (!loadCue(path, config, error))
        return false;
    const std::string rig = rigPath.empty() ? defaultRigPath() : rigPath;
    struct stat st;
    if (rigPath.empty() && stat(rig.c_str(), &st) != 0)
        return true;
    return loadRig(rig, config, error);
}

} // namespace cue
