#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

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

bool parseBool(const std::string& v, bool& out) {
    const std::string l = lower(v);
    if (l == "yes" || l == "true" || l == "on" || l == "1")
        out = true;
    else if (l == "no" || l == "false" || l == "off" || l == "0")
        out = false;
    else
        return false;
    return true;
}

// thru (the input passes untouched) or mute (nothing passes): out = mute.
bool parseBypassMode(const std::string& v, bool& mute) {
    const std::string l = lower(v);
    if (l == "thru" || l == "through")
        mute = false;
    else if (l == "mute")
        mute = true;
    else
        return false;
    return true;
}

bool parseNumber(const std::string& v, double& out) {
    if (v.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(v.c_str(), &end);
    return end && *end == '\0';
}

std::vector<std::string> splitList(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!trim(item).empty())
            out.push_back(trim(item));
    return out;
}

// "out3" with prefix "out": 3. 0 when it is not the prefix and a number from 1.
size_t numbered(const std::string& key, const std::string& prefix) {
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0)
        return 0;
    size_t n = 0;
    for (size_t i = prefix.size(); i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9' || n > 1000)
            return 0;
        n = n * 10 + static_cast<size_t>(key[i] - '0');
    }
    return n;
}

// Sets the channel of the index-th input or output, the ones before it
// taking the channel of their position.
void setChannel(std::vector<unsigned>& channels, size_t index, unsigned channel) {
    while (channels.size() <= index)
        channels.push_back(static_cast<unsigned>(channels.size()));
    channels[index] = channel;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string dirOf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string resolve(const std::string& dir, std::string path) {
    path = expandPath(path);
    if (!path.empty() && path[0] != '/')
        path = dir + "/" + path;
    return path;
}

bool parsePlugin(const std::string& value, const std::string& dir, PluginSpec& spec, std::string* error) {
    spec.text = value;
    const auto colon = value.find(':');
    if (colon == std::string::npos) {
        *error = "plugin needs a kind, like clap:/path/to/file.clap or builtin:gain 0.5";
        return false;
    }
    spec.kind = lower(trim(value.substr(0, colon)));
    const std::string rest = trim(value.substr(colon + 1));
    if (spec.kind == "clap" || spec.kind == "vst3") {
        const auto hash = rest.rfind('#');
        spec.path = resolve(dir, trim(hash == std::string::npos ? rest : rest.substr(0, hash)));
        if (hash != std::string::npos)
            spec.id = trim(rest.substr(hash + 1));
        return true;
    }
    if (spec.kind == "builtin") {
        std::stringstream ss(rest);
        std::string word;
        while (ss >> word)
            spec.args.push_back(word);
        if (spec.args.empty()) {
            *error = "builtin needs a name";
            return false;
        }
        return true;
    }
    *error = "unknown plug-in kind '" + spec.kind + "' (clap, vst3 or builtin)";
    return false;
}

// "0..10", "0 to 10" or "-12, 12".
bool parseRange(const std::string& v, double& a, double& b) {
    for (const char* sep : {"..", " to ", ","}) {
        const auto pos = v.find(sep);
        if (pos != std::string::npos)
            return parseNumber(trim(v.substr(0, pos)), a) && parseNumber(trim(v.substr(pos + std::strlen(sep))), b);
    }
    return false;
}

enum class Section { None, Audio, Switching, Midi, Performance, Scene, Chain, Branch, Preset, Control, Trigger, Group, Fcb };

struct Parser {
    Config& config;
    std::string* error;
    int depth = 0;

    Section section = Section::None;
    // Where lines go: indexes, since the vectors grow.
    int scene = -1, chain = -1, line = -1, preset = -1, control = -1, trigger = -1, group = -1;
    bool sceneHasOwnChain = false; // an old-style [scene] with plugin lines

    std::string where; // file:line of the line being parsed
    // The highest inN and outN of [audio], checked against the performance's
    // inputs and outputs once they are all read.
    size_t audioIns = 0, audioOuts = 0;
    std::string audioInsWhere, audioOutsWhere;

    bool fail(const std::string& what) {
        *error = where + ": " + what;
        return false;
    }

    SceneSpec* currentScene() { return scene >= 0 ? &config.scenes[scene] : nullptr; }
    ChainSpec* currentChain() { return scene >= 0 && chain >= 0 ? &config.scenes[scene].chains[chain] : nullptr; }
    LineSpec* currentLine() {
        ChainSpec* c = currentChain();
        return c && line >= 0 ? &c->lines[line] : nullptr;
    }

    SceneSpec& ensureScene() {
        if (scene < 0) {
            config.scenes.emplace_back();
            scene = static_cast<int>(config.scenes.size()) - 1;
            config.scenes.back().name = "Scene 1";
            config.scenes.back().where = where;
        }
        return config.scenes[scene];
    }

    bool header(const std::string& text) {
        const std::string h = trim(text);
        const auto space = h.find_first_of(" \t");
        const std::string kind = lower(space == std::string::npos ? h : h.substr(0, space));
        const std::string name = space == std::string::npos ? "" : trim(h.substr(space + 1));
        line = -1;
        preset = control = trigger = group = -1;

        if (kind == "audio") section = Section::Audio;
        else if (kind == "switching") section = Section::Switching;
        else if (kind == "midi") section = Section::Midi;
        else if (kind == "fcb") section = Section::Fcb;
        else if (kind == "performance") {
            section = Section::Performance;
            config.performance = name;
        } else if (kind == "scene") {
            section = Section::Scene;
            config.scenes.emplace_back();
            scene = static_cast<int>(config.scenes.size()) - 1;
            chain = -1;
            sceneHasOwnChain = false;
            SceneSpec& s = config.scenes.back();
            s.name = name.empty() ? "Scene " + std::to_string(config.scenes.size()) : name;
            s.where = where;
        } else if (kind == "chain") {
            section = Section::Chain;
            SceneSpec& s = ensureScene();
            if (sceneHasOwnChain)
                return fail("scene '" + s.name + "' already has plugin lines of its own; put every chain in a [chain] section");
            s.chains.emplace_back();
            chain = static_cast<int>(s.chains.size()) - 1;
            ChainSpec& c = s.chains.back();
            c.name = name.empty() ? s.name + " " + std::to_string(s.chains.size()) : name;
            c.where = where;
            c.lines.emplace_back();
            line = 0;
        } else if (kind == "branch") {
            section = Section::Branch;
            ChainSpec* c = currentChain();
            if (!c)
                return fail("[branch] must follow a [chain]");
            for (size_t i = 1; i < c->lines.size(); ++i)
                if (lower(c->lines[i].name) == lower(name))
                    line = static_cast<int>(i);
            if (line < 0)
                return fail("no 'branch = " + name + "' line in chain '" + c->name + "' says where this branch leaves");
        } else if (kind == "preset") {
            section = Section::Preset;
            SceneSpec& s = ensureScene();
            s.presets.emplace_back();
            preset = static_cast<int>(s.presets.size()) - 1;
            s.presets.back().name = name.empty() ? "Preset " + std::to_string(s.presets.size()) : name;
            s.presets.back().where = where;
        } else if (kind == "control") {
            section = Section::Control;
            if (name.empty())
                return fail("a control needs a name: [control Wah]");
            config.controls.emplace_back();
            control = static_cast<int>(config.controls.size()) - 1;
            config.controls.back().name = name;
            config.controls.back().where = where;
        } else if (kind == "trigger") {
            section = Section::Trigger;
            config.triggers.emplace_back();
            trigger = static_cast<int>(config.triggers.size()) - 1;
            config.triggers.back().name = name.empty() ? "trigger " + std::to_string(config.triggers.size()) : name;
            config.triggers.back().where = where;
        } else if (kind == "group") {
            section = Section::Group;
            if (name.empty())
                return fail("a group needs a name: [group Reset effects]");
            config.groups.emplace_back();
            group = static_cast<int>(config.groups.size()) - 1;
            config.groups.back().name = name;
            config.groups.back().where = where;
        } else {
            return fail("unknown section [" + kind + "]");
        }
        return true;
    }

    bool audio(const std::string& key, const std::string& value) {
        double n = 0;
        const bool isNum = parseNumber(value, n);
        if (key == "device") config.audio.device = value;
        else if (key == "rate" && isNum) config.audio.rate = static_cast<unsigned>(n);
        else if (key == "period" && isNum) config.audio.period = static_cast<unsigned>(n);
        else if (key == "periods" && isNum) config.audio.periods = static_cast<unsigned>(n);
        else if ((key == "input" || numbered(key, "in")) && isNum && n >= 1) {
            const size_t k = key == "input" ? 1 : numbered(key, "in");
            setChannel(config.audio.inputChannels, k - 1, static_cast<unsigned>(n) - 1);
            if (k > audioIns)
                audioIns = k, audioInsWhere = where;
        } else if (numbered(key, "out") && isNum && n >= 1) {
            const size_t k = numbered(key, "out");
            setChannel(config.audio.outputChannels, k - 1, static_cast<unsigned>(n) - 1);
            if (k > audioOuts)
                audioOuts = k, audioOutsWhere = where;
        }
        else if (key == "priority" && isNum) config.audio.rtPriority = static_cast<int>(n);
        else if (key == "workers" && isNum && n >= 0 && n <= 8) config.workers = static_cast<int>(n);
        else if (key == "cpu") {
            if (lower(value) == "auto") config.cpuAuto = true;
            else if (lower(value) == "none") { config.cpuAuto = false; config.audio.cpu = -1; }
            else if (isNum) { config.cpuAuto = false; config.audio.cpu = static_cast<int>(n); }
            else return fail("cpu must be auto, none or a CPU number");
        } else if (key == "mix") {
            const std::string v = lower(value);
            if (v != "left" && v != "right" && v != "sum")
                return fail("mix must be left, right or sum");
            config.mix = v == "left" ? OutputMix::Left : v == "right" ? OutputMix::Right : OutputMix::Sum;
        } else return fail("unknown or invalid audio setting '" + key + "'");
        return true;
    }

    bool switching(const std::string& key, const std::string& value) {
        double n = 0;
        const bool isNum = parseNumber(value, n);
        bool b = false;
        if (key == "preroll_ms" && isNum) config.engine.prerollMs = static_cast<float>(n);
        else if (key == "crossfade_ms" && isNum) config.engine.crossfadeMs = static_cast<float>(n);
        else if (key == "tail_cap_ms" && isNum) config.engine.tailCapMs = static_cast<float>(n);
        else if (key == "tail_hold_ms" && isNum) config.engine.tailHoldMs = static_cast<float>(n);
        else if (key == "tail_quiet_db" && isNum) config.engine.tailQuietDb = static_cast<float>(n);
        else if (key == "bypass_fade_ms" && isNum) config.engine.bypassFadeMs = static_cast<float>(n);
        else if (key == "mute_fade_ms" && isNum) config.engine.muteFadeMs = static_cast<float>(n);
        else if (key == "reset_tails_on_warm" && parseBool(value, b)) config.engine.resetTailsOnWarmSwitch = b;
        else if (key == "bypass_mode") {
            if (!parseBypassMode(value, b))
                return fail("bypass_mode is thru or mute");
            config.muteOnBypass = b;
        }
        else if (key == "warm") {
            config.warm = lower(value);
            if (config.warm != "none" && config.warm != "next")
                return fail("warm must be none or next");
        } else return fail("unknown or invalid switching setting '" + key + "'");
        return true;
    }

    bool midi(const std::string& key, const std::string& value) {
        double n = 0;
        bool b = false;
        if (key == "enabled" && parseBool(value, b)) config.midi.enabled = b;
        else if (key == "in") config.midi.in = value;
        else if (key == "out") config.midi.out = value;
        else if (key == "profile") {
            config.midi.profile = lower(value);
            if (config.midi.profile != "eurekaprom" && config.midi.profile != "none")
                return fail("profile must be eurekaprom or none");
        } else if (key == "channel" && parseNumber(value, n) && n >= 1 && n <= 16) config.midi.channel = static_cast<uint8_t>(n - 1);
        else if (key == "forward" && parseBool(value, b)) config.midi.forward = b;
        else return fail("unknown or invalid midi setting '" + key + "'");
        return true;
    }

    // plugin, param, state, name, bypass, midi, branch, tap: lines of a chain.
    bool chainLine(const std::string& key, const std::string& value, const std::string& dir) {
        if (section == Section::Scene && !sceneHasOwnChain) {
            // An old-style scene: one chain of the same name.
            SceneSpec& s = config.scenes[scene];
            if (!s.chains.empty())
                return fail("plugin lines belong in a [chain] section");
            s.chains.emplace_back();
            s.chains.back().name = s.name;
            s.chains.back().where = s.where;
            s.chains.back().lines.emplace_back();
            chain = 0;
            line = 0;
            sceneHasOwnChain = true;
        }
        LineSpec* l = currentLine();
        if (!l)
            return fail("'" + key + "' belongs in a [chain] or [branch] section");
        ChainSpec& c = *currentChain();
        PluginSpec* last = l->plugins.empty() ? nullptr : &l->plugins.back();
        auto needPlugin = [&](const char* what) {
            if (!last)
                fail(std::string(what) + " must follow a plugin line");
            return last != nullptr;
        };
        bool b = false;
        if (key == "plugin") {
            PluginSpec spec;
            std::string err;
            if (!parsePlugin(value, dir, spec, &err))
                return fail(err);
            spec.where = where;
            l->plugins.push_back(spec);
        } else if (key == "param") {
            if (!needPlugin("param"))
                return false;
            const auto sep = value.find_first_of(":=");
            double id = 0, v = 0;
            if (sep == std::string::npos || !parseNumber(trim(value.substr(0, sep)), id) || !parseNumber(trim(value.substr(sep + 1)), v) || id < 0)
                return fail("param is id:value, like param = 54:1 (cue params lists the ids)");
            last->params.push_back({static_cast<uint32_t>(id), v});
        } else if (key == "state") {
            if (!needPlugin("state"))
                return false;
            last->stateFile = resolve(dir, value);
        } else if (key == "controller_state") {
            if (!needPlugin("controller_state"))
                return false;
            last->controllerStateFile = resolve(dir, value);
        } else if (key == "name") {
            if (!needPlugin("name"))
                return false;
            last->label = value;
        } else if (key == "bypass") {
            if (!needPlugin("bypass"))
                return false;
            if (!parseBool(value, b))
                return fail("bypass is yes or no");
            last->bypassed = b;
        } else if (key == "bypass_mode") {
            if (!needPlugin("bypass_mode"))
                return false;
            if (!parseBypassMode(value, b))
                return fail("bypass_mode is thru or mute");
            last->muteOnBypass = b;
            last->bypassModeSet = true;
        } else if (key == "sidechain") {
            if (!needPlugin("sidechain"))
                return false;
            if (value.empty())
                return fail("sidechain names an earlier plug-in of the chain, like sidechain = TwoStorms Gate");
            last->sidechain = value;
        } else if (key == "midi") {
            if (!needPlugin("midi"))
                return false;
            if (!parseBool(value, b))
                return fail("midi is yes or no");
            last->midi = b;
        } else if (key == "branch") {
            if (value.empty())
                return fail("branch needs a name");
            for (const auto& other : c.lines)
                if (lower(other.name) == lower(value))
                    return fail("chain '" + c.name + "' already has a branch named " + value);
            LineSpec branch;
            branch.name = value;
            branch.parent = line;
            branch.after = l->plugins.size();
            c.lines.push_back(branch); // l may now dangle
        } else if (key == "tap") {
            if (value.empty())
                return fail("tap names an output: tap = out2, or the output's name");
            TapSpec t;
            t.output = value;
            t.line = line;
            t.after = l->plugins.size();
            t.set = true;
            t.where = where;
            c.out.push_back(t);
        } else if (key == "input") {
            if (value.empty())
                return fail("input names one of the performance's inputs: input = in2, or its name");
            c.input = value;
        } else {
            return fail("unknown chain setting '" + key + "'");
        }
        return true;
    }

    bool sceneLine(const std::string& key, const std::string& value, const std::string& dir) {
        SceneSpec& s = config.scenes[scene];
        double n = 0;
        if (key == "display") s.display = value;
        else if (key == "pedal" && parseNumber(value, n) && n >= 1 && n <= 12) s.pedal = static_cast<int>(n);
        else if (key == "program" && parseNumber(value, n) && n >= 0 && n <= 127) s.program = static_cast<int>(n);
        else return chainLine(key, value, dir);
        return true;
    }

    bool performanceLine(const std::string& key, const std::string& value) {
        if (key == "inputs" || key == "outputs") {
            auto names = splitList(value);
            if (names.empty())
                return fail(key + " lists names: outputs = FOH, Amp");
            (key == "inputs" ? config.inputs : config.outputs) = names;
        } else
            return fail("unknown performance setting '" + key + "' (inputs, outputs)");
        return true;
    }

    bool presetLine(const std::string& key, const std::string& value) {
        PresetSpec& p = config.scenes[scene].presets[preset];
        double n = 0;
        if (key == "chain") p.chain = value;
        else if (key == "display") p.display = value;
        else if (key == "pedal" && parseNumber(value, n) && n >= 1 && n <= 12) p.pedal = static_cast<int>(n);
        else if (key == "program" && parseNumber(value, n) && n >= 0 && n <= 127) p.program = static_cast<int>(n);
        else if (key == "do") p.actions.push_back({value, where});
        else if (key == "warm") p.warm = value;
        else return fail("unknown or invalid preset setting '" + key + "'");
        return true;
    }

    bool controlLine(const std::string& key, const std::string& value) {
        ControlSpec& c = config.controls[control];
        double n = 0, a = 0, b = 0;
        bool flag = false;
        if (key == "type") {
            const std::string v = lower(value);
            if (v == "toggle") c.type = ControlSpec::Type::Toggle;
            else if (v == "discrete") c.type = ControlSpec::Type::Discrete;
            else if (v == "continuous") c.type = ControlSpec::Type::Continuous;
            else return fail("type is toggle, discrete or continuous");
            c.typeSet = true;
        } else if (key == "values") {
            c.labels = splitList(value);
            if (c.labels.size() < 2)
                return fail("values lists at least two names: values = Off, Pedal, Auto");
        } else if (key == "range") {
            if (!parseRange(value, a, b) || a == b)
                return fail("range is low..high, like range = 0..10");
            c.min = std::min(a, b);
            c.max = std::max(a, b);
            c.rangeSet = true;
        } else if (key == "min" && parseNumber(value, n)) { c.min = n; c.rangeSet = true; }
        else if (key == "max" && parseNumber(value, n)) { c.max = n; c.rangeSet = true; }
        else if (key == "step" && parseNumber(value, n) && n > 0) { c.step = n; c.stepSet = true; }
        else if (key == "default") {
            // A number, or one of the values' names, resolved when the control is finished.
            if (parseNumber(value, n))
                c.def = n;
            else
                c.defText = value;
            c.defSet = true;
        } else if (key == "wrap" && parseBool(value, flag)) c.wrap = flag;
        else if (key == "pedal" && parseNumber(value, n) && n >= 1 && n <= 12) c.pedal = static_cast<int>(n);
        else if (key == "follows") c.follows = {value, where};
        else if (key == "enable") c.enables.push_back({value, where});
        else if (key == "drive") c.drives.push_back({value, where});
        else if (key == "mute") c.mutes.push_back({value, where});
        else return fail("unknown or invalid control setting '" + key + "'");
        return true;
    }

    bool triggerLine(const std::string& key, const std::string& value) {
        TriggerSpec& t = config.triggers[trigger];
        if (key == "on") t.inputs.push_back({value, where});
        else if (key == "do") t.actions.push_back({value, where});
        else if (key == "when") t.when = {value, where};
        else if (key == "scene") t.scene = value;
        else return fail("unknown trigger setting '" + key + "' (on, do, when, scene)");
        return true;
    }

    // "4" (pedal 4), "18" or "value 1": EurekaPROM's LED numbers past the
    // pedals, or their names. 0 if none.
    static int fcbLed(const std::string& text) {
        static const char* names[] = {"expr a", "expr b", "sw1", "sw2", "bank", "select", "number",
                                      "value 1", "value 2", "dir sel", "midi fn", "midi ch", "config"};
        double n = 0;
        if (parseNumber(text, n))
            return n >= 1 && n <= 23 && n == static_cast<int>(n) ? static_cast<int>(n) : 0;
        std::string t;
        for (char c : lower(text))
            if (c != ' ' && c != '-' && c != '_')
                t += c;
        for (int i = 0; i < 13; ++i) {
            std::string name;
            for (const char* c = names[i]; *c; ++c)
                if (*c != ' ')
                    name += *c;
            if (t == name)
                return 11 + i;
        }
        return 0;
    }

    bool fcbLine(const std::string& key, const std::string& value) {
        if (key.rfind("led", 0) == 0) {
            const int led = fcbLed(trim(key.substr(3)));
            if (!led)
                return fail("unknown LED '" + trim(key.substr(3)) + "' (a pedal 1-10, 11-23, or expr a, expr b, sw1, sw2, bank, select, number, value 1, value 2, dir sel, midi fn, midi ch, config)");
            config.fcb.leds.push_back({led, {value, where}});
        } else if (key == "display")
            config.fcb.displays.push_back({value, where});
        else
            return fail("unknown fcb setting '" + key + "' (led 1 .. led 10, led value 1, display)");
        return true;
    }

    bool parse(const std::string& text, const std::string& path) {
        const std::string dir = dirOf(path);
        std::istringstream in(text);
        std::string raw;
        int lineNo = 0;
        while (std::getline(in, raw)) {
            ++lineNo;
            where = path + ":" + std::to_string(lineNo);
            // ';' starts a comment anywhere, '#' only at the start of a line
            // (it separates a plug-in file from the id inside it).
            std::string l = raw;
            const auto semi = l.find(';');
            if (semi != std::string::npos)
                l = l.substr(0, semi);
            l = trim(l);
            if (l.empty() || l[0] == '#')
                continue;
            if (l.front() == '[') {
                if (l.back() != ']')
                    return fail("unclosed section header");
                if (!header(l.substr(1, l.size() - 2)))
                    return false;
                continue;
            }
            const auto eq = l.find('=');
            if (eq == std::string::npos)
                return fail("expected key = value");
            const std::string key = lower(trim(l.substr(0, eq)));
            const std::string value = trim(l.substr(eq + 1));
            if (key == "include") {
                if (!include(resolve(dir, value)))
                    return false;
                where = path + ":" + std::to_string(lineNo);
                continue;
            }
            bool ok = true;
            switch (section) {
            case Section::None: ok = fail("setting outside a section"); break;
            case Section::Audio: ok = audio(key, value); break;
            case Section::Switching: ok = switching(key, value); break;
            case Section::Midi: ok = midi(key, value); break;
            case Section::Performance: ok = performanceLine(key, value); break;
            case Section::Scene: ok = sceneLine(key, value, dir); break;
            case Section::Chain:
            case Section::Branch: ok = chainLine(key, value, dir); break;
            case Section::Preset: ok = presetLine(key, value); break;
            case Section::Control: ok = controlLine(key, value); break;
            case Section::Trigger: ok = triggerLine(key, value); break;
            case Section::Group:
                if (key == "do") config.groups[group].actions.push_back({value, where});
                else ok = fail("a group only has do = lines");
                break;
            case Section::Fcb: ok = fcbLine(key, value); break;
            }
            if (!ok)
                return false;
        }
        return true;
    }

    bool include(const std::string& path) {
        if (depth >= 8)
            return fail("includes nested too deeply (is a file including itself?)");
        std::ifstream f(path);
        if (!f)
            return fail("cannot open included file " + path);
        std::stringstream ss;
        ss << f.rdbuf();
        ++depth;
        const bool ok = parse(ss.str(), path);
        --depth;
        return ok;
    }
};

bool finishControl(ControlSpec& c, std::string* error) {
    const std::string& defLabel = c.defText;
    auto fail = [&](const std::string& what) {
        *error = c.where + ": control '" + c.name + "': " + what;
        return false;
    };
    if (!c.labels.empty()) {
        if (!c.typeSet || c.type == ControlSpec::Type::Toggle)
            c.type = ControlSpec::Type::Discrete;
        if (c.type == ControlSpec::Type::Continuous)
            return fail("a continuous control cannot have named values");
        c.min = 0;
        c.max = static_cast<double>(c.labels.size() - 1);
        c.step = 1;
    } else if (c.type == ControlSpec::Type::Toggle || (!c.typeSet && !c.rangeSet)) {
        // No range and no values: an on/off switch.
        c.type = ControlSpec::Type::Toggle;
        c.min = 0;
        c.max = 1;
        c.step = 1;
        c.labels = {"off", "on"};
    } else {
        if (c.min >= c.max)
            return fail("min must be below max");
        if (c.type == ControlSpec::Type::Continuous && !c.stepSet)
            c.step = (c.max - c.min) / 10.0;
    }
    if (!defLabel.empty()) {
        bool found = false;
        for (size_t i = 0; i < c.labels.size(); ++i)
            if (lower(c.labels[i]) == lower(defLabel)) {
                c.def = c.min + static_cast<double>(i) * c.step;
                found = true;
            }
        if (!found)
            return fail("default '" + defLabel + "' is not one of its values");
        c.defText.clear();
    } else if (!c.defSet) {
        c.def = c.min;
    }
    if (c.def < c.min || c.def > c.max)
        return fail("default is outside its range");
    return true;
}

} // namespace

std::string expandPath(std::string p) {
    const char* home = std::getenv("HOME");
    if (!home)
        return p;
    if (!p.empty() && p[0] == '~')
        p = std::string(home) + p.substr(1);
    const auto pos = p.find("$HOME");
    if (pos != std::string::npos)
        p.replace(pos, 5, home);
    return p;
}

std::vector<const PluginSpec*> chainPlugins(const ChainSpec& chain) {
    std::vector<const PluginSpec*> out;
    for (const auto& l : chain.lines)
        for (const auto& p : l.plugins)
            out.push_back(&p);
    return out;
}

bool chainSidechains(const ChainSpec& chain, const std::vector<std::string>& labels, std::vector<std::pair<size_t, size_t>>& feeds,
                     std::string* error) {
    feeds.clear();
    const auto plugins = chainPlugins(chain);
    for (size_t slot = 0; slot < plugins.size() && slot < labels.size(); ++slot) {
        const PluginSpec& ps = *plugins[slot];
        if (ps.sidechain.empty())
            continue;
        std::string label = trim(ps.sidechain);
        int nth = 0;
        double n = 0;
        const auto colon = label.rfind(':');
        if (colon != std::string::npos && parseNumber(trim(label.substr(colon + 1)), n) && n >= 1) {
            nth = static_cast<int>(n);
            label = trim(label.substr(0, colon));
        }
        int source = -1, count = 0;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (lower(labels[i]) != lower(label))
                continue;
            ++count;
            if (nth ? count == nth : i < slot)
                source = static_cast<int>(i);
            if (nth && count == nth)
                break;
        }
        const std::string where = ps.where + ": sidechain = " + ps.sidechain + ": ";
        if (source < 0) {
            *error = where + "no plug-in named '" + label + "'" + (nth ? " (number " + std::to_string(nth) + ")" : " before it") + " in chain " + chain.name;
            return false;
        }
        if (static_cast<size_t>(source) >= slot) {
            *error = where + "it comes after the plug-in it would feed; a side-chain comes from an earlier plug-in (main line first, then each branch)";
            return false;
        }
        feeds.push_back({slot, static_cast<size_t>(source)});
    }
    return true;
}

bool parseConfigText(const std::string& text, const std::string& path, Config& config, std::string* error) {
    Parser parser{config, error};
    if (!parser.parse(text, path))
        return false;
    if (parser.audioIns > config.inputs.size()) {
        *error = parser.audioInsWhere + ": in" + std::to_string(parser.audioIns) + ": the performance has " + std::to_string(config.inputs.size()) +
                 " input(s); [performance] inputs = names them";
        return false;
    }
    if (parser.audioOuts > config.outputs.size()) {
        *error = parser.audioOutsWhere + ": out" + std::to_string(parser.audioOuts) + ": the performance has " + std::to_string(config.outputs.size()) +
                 " output(s); [performance] outputs = names them";
        return false;
    }
    config.path = path;
    if (config.scenes.empty()) {
        *error = path + ": no [scene] or [chain] sections";
        return false;
    }
    return finishConfig(config, error);
}

bool finishConfig(Config& config, std::string* error) {
    // Inputs and outputs: at least one of each, each name once, and a
    // channel for each (the ones not given take the channel of their place).
    for (const auto* names : {&config.inputs, &config.outputs}) {
        const std::string what = names == &config.inputs ? "input" : "output";
        if (names->empty()) {
            *error = config.path + ": a performance needs at least one " + what;
            return false;
        }
        if (names->size() > kMaxPorts) {
            *error = config.path + ": " + std::to_string(names->size()) + " " + what + "s; Cue plays at most " + std::to_string(kMaxPorts);
            return false;
        }
        for (size_t i = 0; i < names->size(); ++i)
            for (size_t k = 0; k < i; ++k)
                if (lower((*names)[i]) == lower((*names)[k])) {
                    *error = config.path + ": two " + what + "s are named " + (*names)[i];
                    return false;
                }
    }
    auto fit = [](std::vector<unsigned>& channels, size_t n) {
        if (channels.size() > n)
            channels.resize(n);
        while (channels.size() < n)
            channels.push_back(static_cast<unsigned>(channels.size()));
    };
    fit(config.audio.inputChannels, config.inputs.size());
    fit(config.audio.outputChannels, config.outputs.size());
    // An input or output by name, or as in2 / out2.
    auto port = [](const std::vector<std::string>& names, const std::string& text, const char* prefix) {
        for (size_t k = 0; k < names.size(); ++k)
            if (lower(names[k]) == lower(text))
                return static_cast<int>(k);
        const size_t n = numbered(lower(text), prefix);
        return n >= 1 && n <= names.size() ? static_cast<int>(n) - 1 : -1;
    };
    if (config.scenes.empty()) {
        *error = config.path + ": no scenes";
        return false;
    }
    for (auto& s : config.scenes) {
        if (s.chains.empty()) {
            *error = s.where + ": scene '" + s.name + "' has no chains";
            return false;
        }
        // A scene without presets gets one per chain.
        if (s.presets.empty())
            for (const auto& c : s.chains) {
                PresetSpec p;
                p.name = c.name;
                p.chain = c.name;
                p.where = c.where;
                s.presets.push_back(p);
            }
        for (auto& p : s.presets) {
            if (p.chain.empty()) {
                // The chain of the same name, or the scene's first chain.
                p.chain = s.chains.front().name;
                bool named = false;
                for (const auto& c : s.chains)
                    if (lower(c.name) == lower(p.name)) {
                        p.chain = c.name;
                        named = true;
                    }
                if (!named && s.chains.size() > 1)
                    config.warnings.push_back(p.where + ": preset '" + p.name + "' has no 'chain =' line and no chain of its name, so it plays the first chain, " + p.chain);
            }
            bool found = false, warmFound = false;
            for (const auto& c : s.chains) {
                found = found || lower(c.name) == lower(p.chain);
                warmFound = warmFound || lower(c.name) == lower(p.warm);
            }
            if (!found) {
                *error = p.where + ": preset '" + p.name + "': scene '" + s.name + "' has no chain named " + p.chain;
                return false;
            }
            if (!p.warm.empty() && lower(p.warm) != "next" && lower(p.warm) != "none" && !warmFound) {
                *error = p.where + ": preset '" + p.name + "': warm is next, none or a chain of scene '" + s.name + "', and it has no chain named " + p.warm;
                return false;
            }
        }
        for (auto& c : s.chains) {
            c.inputIndex = 0;
            if (!c.input.empty()) {
                const int i = port(config.inputs, c.input, "in");
                if (i < 0) {
                    *error = c.where + ": chain '" + c.name + "': input = " + c.input + ": the performance has no such input (inputs: " +
                             joined(config.inputs) + ")";
                    return false;
                }
                c.inputIndex = static_cast<size_t>(i);
                c.input = config.inputs[c.inputIndex];
            }
            std::vector<TapSpec> taps(config.outputs.size());
            for (size_t o = 0; o < taps.size(); ++o) {
                taps[o].output = config.outputs[o];
                taps[o].after = c.lines[0].plugins.size();
            }
            for (const TapSpec& t : c.out) {
                const int o = port(config.outputs, t.output, "out");
                if (o < 0) {
                    *error = (t.where.empty() ? c.where : t.where) + ": chain '" + c.name + "': tap = " + t.output +
                             ": the performance has no such output (outputs: " + joined(config.outputs) + ")";
                    return false;
                }
                TapSpec& to = taps[static_cast<size_t>(o)];
                to = t;
                to.output = config.outputs[static_cast<size_t>(o)];
                to.set = true;
                if (to.line < 0 || static_cast<size_t>(to.line) >= c.lines.size())
                    to.line = 0;
                to.after = std::min(to.after, c.lines[static_cast<size_t>(to.line)].plugins.size());
            }
            c.out = std::move(taps);
            for (auto& l : c.lines)
                for (auto& p : l.plugins)
                    if (!p.bypassModeSet)
                        p.muteOnBypass = config.muteOnBypass;
        }
    }
    for (auto& c : config.controls)
        if (!finishControl(c, error))
            return false;
    if (config.midi.out.empty() && lower(config.midi.in) != "any")
        config.midi.out = config.midi.in;
    return true;
}

bool loadConfig(const std::string& path, Config& config, std::string* error) {
    std::ifstream f(path);
    if (!f) {
        *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseConfigText(ss.str(), path, config, error);
}

} // namespace cue
