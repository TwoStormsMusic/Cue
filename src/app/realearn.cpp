#include "realearn.h"

#include "midi/eurekaprom.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace cue {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

bool same(const std::string& a, const std::string& b) { return lower(trim(a)) == lower(trim(b)); }

bool startsWithNoCase(const std::string& s, const std::string& p) { return lower(s.substr(0, p.size())) == lower(p); }

bool number(const std::string& s, double& v) {
    const std::string t = trim(s);
    if (t.empty())
        return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == 0;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!trim(item).empty())
            out.push_back(trim(item));
    return out;
}

const char* kAbc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::vector<uint8_t>& data) {
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        out += kAbc[v >> 18 & 63];
        out += kAbc[v >> 12 & 63];
        out += i + 1 < data.size() ? kAbc[v >> 6 & 63] : '=';
        out += i + 2 < data.size() ? kAbc[v & 63] : '=';
    }
    return out;
}

bool unbase64(const std::string& text, std::vector<uint8_t>& out) {
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || std::isspace(static_cast<unsigned char>(c)))
            continue;
        const char* p = std::strchr(kAbc, c);
        if (!p || !c)
            return false;
        acc = acc << 6 | static_cast<uint32_t>(p - kAbc);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return true;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size() ? b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<uint32_t>(b[at + 3]) << 24 : 0;
}

// The value EurekaPROM sends with CC 104 for a pedal, and the LED value for it.
int pedalValue(int pedal) { return pedal == 10 ? 0 : pedal == eurekaprom::kUp ? 10 : pedal == eurekaprom::kDown ? 11 : pedal; }

// ---------------------------------------------------------------- the performance's own mappings, as INI text

std::string mappingsIni(const Config& c) {
    std::ostringstream o;
    for (const ControlSpec& ct : c.controls) {
        o << "\n[control " << ct.name << "]\n";
        if (ct.type == ControlSpec::Type::Toggle) {
            o << "type = toggle\n";
        } else if (ct.type == ControlSpec::Type::Continuous) {
            o << "type = continuous\nrange = " << num(ct.min) << ".." << num(ct.max) << "\nstep = " << num(ct.step) << "\n";
        } else if (!ct.labels.empty()) {
            o << "values = ";
            for (size_t i = 0; i < ct.labels.size(); ++i)
                o << (i ? ", " : "") << ct.labels[i];
            o << "\n";
        } else {
            o << "type = discrete\nrange = " << num(ct.min) << ".." << num(ct.max) << "\nstep = " << num(ct.step) << "\n";
        }
        o << "default = " << num(ct.def) << "\nwrap = " << (ct.wrap ? "yes" : "no") << "\n";
        if (ct.pedal)
            o << "pedal = " << ct.pedal << "\n";
        if (!ct.follows.text.empty())
            o << "follows = " << ct.follows.text << "\n";
        for (const auto& e : ct.enables)
            o << "enable = " << e.text << "\n";
        for (const auto& d : ct.drives)
            o << "drive = " << d.text << "\n";
        for (const auto& m : ct.mutes)
            o << "mute = " << m.text << "\n";
    }
    for (const TriggerSpec& t : c.triggers) {
        o << "\n[trigger " << t.name << "]\n";
        if (!t.scene.empty())
            o << "scene = " << t.scene << "\n";
        if (!t.when.text.empty())
            o << "when = " << t.when.text << "\n";
        for (const auto& i : t.inputs)
            o << "on = " << i.text << "\n";
        for (const auto& a : t.actions)
            o << "do = " << a.text << "\n";
    }
    for (const GroupSpec& g : c.groups) {
        o << "\n[group " << g.name << "]\n";
        for (const auto& a : g.actions)
            o << "do = " << a.text << "\n";
    }
    if (!c.fcb.leds.empty() || !c.fcb.displays.empty()) {
        o << "\n[fcb]\n";
        for (const auto& [led, when] : c.fcb.leds)
            o << "led " << led << " = " << when.text << "\n";
        for (const auto& d : c.fcb.displays)
            o << "display = " << d.text << "\n";
    }
    const EngineSettings& e = c.engine;
    o << "\n[switching]\npreroll_ms = " << num(e.prerollMs) << "\ncrossfade_ms = " << num(e.crossfadeMs) << "\ntail_cap_ms = " << num(e.tailCapMs)
      << "\ntail_hold_ms = " << num(e.tailHoldMs) << "\ntail_quiet_db = " << num(e.tailQuietDb) << "\nbypass_fade_ms = " << num(e.bypassFadeMs)
      << "\nmute_fade_ms = " << num(e.muteFadeMs) << "\nreset_tails_on_warm = " << (e.resetTailsOnWarmSwitch ? "yes" : "no") << "\nwarm = " << c.warm
      << "\nbypass_mode = " << (c.muteOnBypass ? "mute" : "thru") << "\n";
    o << "\n[midi]\nprofile = " << c.midi.profile << "\nchannel = " << c.midi.channel + 1 << "\nforward = " << (c.midi.forward ? "yes" : "no") << "\n";
    o << "\n[audio]\nmix = " << (c.mix == OutputMix::Left ? "left" : "sum") << "\n";
    return o.str();
}

// ---------------------------------------------------------------- conditions as ReaLearn expressions

// The ReaLearn layout: parameter 0 is the chain, each discrete control a parameter.
struct Layout {
    const Config& config;
    struct Chain {
        std::string scene, name;
        double value = 0;
        size_t scene_index = 0;
    };
    std::vector<Chain> chains;
    struct Control {
        const ControlSpec* spec = nullptr;
        int param = -1; // -1: not in ReaLearn (continuous)
        int count = 0;  // values
    };
    std::vector<Control> controls;

    explicit Layout(const Config& c) : config(c) {
        for (size_t s = 0; s < c.scenes.size(); ++s)
            for (const ChainSpec& ch : c.scenes[s].chains)
                chains.push_back({c.scenes[s].name, ch.name, (chains.size() + 1) / 100.0, s});
        int next = 1;
        for (const ControlSpec& ct : c.controls) {
            Control k;
            k.spec = &ct;
            if (ct.type == ControlSpec::Type::Toggle)
                k.count = 2;
            else if (!ct.labels.empty())
                k.count = static_cast<int>(ct.labels.size());
            else if (ct.type == ControlSpec::Type::Discrete && ct.step > 0)
                k.count = static_cast<int>(std::lround((ct.max - ct.min) / ct.step)) + 1;
            if (k.count >= 2 && k.count <= 100 && ct.type != ControlSpec::Type::Continuous)
                k.param = next++;
            controls.push_back(k);
        }
    }

    const Control* control(const std::string& name) const {
        for (const auto& k : controls)
            if (same(k.spec->name, name))
                return &k;
        return nullptr;
    }

    // The longest control name at the start of text, and the rest.
    const Control* leadingControl(const std::string& text, std::string& rest) const {
        const Control* best = nullptr;
        for (const auto& k : controls) {
            const std::string& n = k.spec->name;
            if (startsWithNoCase(text, n) && (text.size() == n.size() || text[n.size()] == ' ' || text[n.size()] == '=' || text[n.size()] == '!' ||
                                              text[n.size()] == '<' || text[n.size()] == '>'))
                if (!best || n.size() > best->spec->name.size())
                    best = &k;
        }
        if (best)
            rest = trim(text.substr(best->spec->name.size()));
        return best;
    }

    // A control's value (a name, on/off or a number) as its index, or -1.
    static int index(const Control& k, const std::string& value) {
        const ControlSpec& ct = *k.spec;
        const std::string v = trim(value);
        for (size_t i = 0; i < ct.labels.size(); ++i)
            if (same(ct.labels[i], v))
                return static_cast<int>(i);
        if (k.count == 2 && (same(v, "on") || same(v, "yes")))
            return 1;
        if (k.count == 2 && (same(v, "off") || same(v, "no")))
            return 0;
        double n = 0;
        if (number(v, n) && ct.step > 0) {
            const long i = std::lround((n - ct.min) / ct.step);
            if (i >= 0 && i < k.count)
                return static_cast<int>(i);
        }
        return -1;
    }

    static std::string p(int param) { return "p[" + std::to_string(param) + "]"; }
    static std::string is(const std::string& var, double v) { return "abs(" + var + " - " + num(v) + ") < 0.005"; }

    const Chain* chain(const std::string& name) const {
        for (const auto& c : chains)
            if (same(c.name, name))
                return &c;
        return nullptr;
    }

    // The chains of a scene as a range of parameter 0.
    std::string sceneRange(size_t scene) const {
        double lo = 2, hi = -1;
        for (const auto& c : chains)
            if (c.scene_index == scene) {
                lo = std::min(lo, c.value);
                hi = std::max(hi, c.value);
            }
        return "p[0] > " + num(lo - 0.005) + " && p[0] < " + num(hi + 0.005);
    }

    // A Cue condition as a ReaLearn expression over p[]. local: the control a
    // bare value belongs to. Empty if it cannot be written that way.
    std::string condition(std::string text, const Control* local) const {
        text = trim(text);
        if (text.empty())
            return "";
        // Split at the top level: or, then and.
        for (const char* op : {" or ", " and "}) {
            int depth = 0;
            std::vector<std::string> parts;
            size_t from = 0;
            const std::string l = lower(text);
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '(')
                    ++depth;
                else if (text[i] == ')')
                    --depth;
                else if (depth == 0 && l.compare(i, std::strlen(op), op) == 0) {
                    parts.push_back(text.substr(from, i - from));
                    from = i + std::strlen(op);
                    i = from - 1;
                }
            }
            if (!parts.empty()) {
                parts.push_back(text.substr(from));
                std::string out;
                for (const auto& part : parts) {
                    const std::string e = condition(part, local);
                    if (e.empty())
                        return "";
                    out += (out.empty() ? "" : std::string(op) == " or " ? " || " : " && ") + ("(" + e + ")");
                }
                return out;
            }
        }
        if (startsWithNoCase(text, "not ")) {
            const std::string e = condition(text.substr(4), local);
            return e.empty() ? "" : "!(" + e + ")";
        }
        if (text.front() == '(' && text.back() == ')')
            return condition(text.substr(1, text.size() - 2), local);
        if (same(text, "always"))
            return "1";
        for (const char* what : {"preset", "chain", "scene"}) {
            const std::string w = what;
            if (!startsWithNoCase(text, w + " "))
                continue;
            std::string rest = trim(text.substr(w.size()));
            bool negate = false;
            if (startsWithNoCase(rest, "is not ")) {
                negate = true;
                rest = rest.substr(7);
            } else if (startsWithNoCase(rest, "!=")) {
                negate = true;
                rest = rest.substr(2);
            } else if (startsWithNoCase(rest, "is ")) {
                rest = rest.substr(3);
            } else if (startsWithNoCase(rest, "=")) {
                rest = rest.substr(1);
            }
            std::string e;
            if (w == "scene") {
                for (size_t s = 0; s < config.scenes.size(); ++s)
                    if (same(config.scenes[s].name, rest))
                        e = sceneRange(s);
            } else if (const Chain* c = chain(rest)) {
                e = is("p[0]", c->value);
            }
            if (e.empty())
                return "";
            return negate ? "!(" + e + ")" : e;
        }
        std::string rest;
        const Control* k = leadingControl(text, rest);
        if (!k) {
            // A bare value of the local control.
            if (!local || local->param < 0)
                return "";
            const int i = index(*local, text);
            return i < 0 ? "" : is(p(local->param), i / 100.0);
        }
        if (k->param < 0)
            return "";
        const std::string var = p(k->param);
        if (rest.empty())
            return var + " > 0.005";
        bool negate = false;
        std::string value = rest;
        if (startsWithNoCase(rest, "is not ")) {
            negate = true;
            value = rest.substr(7);
        } else if (startsWithNoCase(rest, "!=")) {
            negate = true;
            value = rest.substr(2);
        } else if (startsWithNoCase(rest, "is ")) {
            value = rest.substr(3);
        } else if (startsWithNoCase(rest, "==")) {
            value = rest.substr(2);
        } else if (startsWithNoCase(rest, "=")) {
            value = rest.substr(1);
        } else {
            for (const char* cmp : {">=", "<=", ">", "<"}) {
                if (!startsWithNoCase(rest, cmp))
                    continue;
                double n = 0;
                if (!number(rest.substr(std::strlen(cmp)), n) || k->spec->step <= 0)
                    return "";
                return var + " " + cmp + " " + num((n - k->spec->min) / k->spec->step / 100.0);
            }
        }
        const int i = index(*k, value);
        if (i < 0)
            return "";
        const std::string e = is(var, i / 100.0);
        return negate ? "!(" + e + ")" : e;
    }
};

// ---------------------------------------------------------------- the ReaLearn instance

struct Builder {
    const Config& config;
    const Layout& layout;
    const std::vector<RppChain>& chains;
    const std::vector<RppFx>& fx;
    std::vector<std::string>& notes;
    json mappings = json::array();
    std::set<std::string> left; // what was left out, once each
    uint8_t channel;

    void leave(const std::string& what) {
        if (left.insert(what).second)
            notes.push_back("pedals: " + what);
    }

    json mapping(const std::string& name, const std::string& group, json source, json mode, json target, bool control = true, bool feedback = false,
                 const std::string& when = "") {
        json m;
        m["id"] = "cue-" + std::to_string(mappings.size() + 1);
        m["name"] = name;
        m["groupId"] = group;
        m["source"] = std::move(source);
        mode["outOfRangeBehavior"] = mode.value("outOfRangeBehavior", "ignore");
        m["mode"] = std::move(mode);
        m["target"] = std::move(target);
        m["controlIsEnabled"] = control;
        m["feedbackIsEnabled"] = feedback;
        if (!when.empty()) {
            m["activationType"] = "expression";
            m["eelCondition"] = when;
        }
        mappings.push_back(m);
        return m;
    }

    json cc(int number) const { return {{"channel", channel}, {"number", number}}; }
    static json param(int index) { return {{"category", "reaper"}, {"reaperSourceType", "realearn-parameter"}, {"parameterIndex", index}}; }
    static json setParam(int index) { return {{"fxAnchor", "this"}, {"paramIndex", index}}; }
    static json fixedSource(double v) { return {{"minSourceValue", v}, {"maxSourceValue", v}}; }

    std::string trackOf(const std::string& scene, const std::string& chain) const {
        for (const auto& c : chains)
            if (same(c.scene, scene) && same(c.name, chain))
                return c.trackGuid;
        return "";
    }

    // The plug-ins a mapping names: "Wah", "Punk/Wah", "TwoStorms Wah:2".
    std::vector<const RppFx*> plugins(const std::string& text) const {
        std::vector<const RppFx*> out;
        for (const std::string& item : splitList(text)) {
            std::string chain, label = item;
            if (auto slash = item.find('/'); slash != std::string::npos) {
                chain = trim(item.substr(0, slash));
                label = trim(item.substr(slash + 1));
            }
            int nth = 0;
            if (auto colon = label.rfind(':'); colon != std::string::npos) {
                double n = 0;
                if (number(label.substr(colon + 1), n) && n >= 1) {
                    nth = static_cast<int>(n);
                    label = trim(label.substr(0, colon));
                }
            }
            std::map<std::string, int> seen; // chain track -> count so far
            for (const RppFx& f : fx) {
                if (!same(f.label, label) || (!chain.empty() && !same(f.chain, chain)))
                    continue;
                const int count = ++seen[f.trackGuid];
                if (!nth || count == nth)
                    out.push_back(&f);
            }
        }
        return out;
    }

    // One action, run when the source fires. Returns false when REAPER has no
    // equivalent. Groups run their actions.
    bool action(const std::string& text, const std::string& name, const std::string& group, const json& source, const json& sourceMode,
                const std::string& when, int depth = 0) {
        const std::string a = trim(text);
        if (startsWithNoCase(a, "group ")) {
            if (depth > 8)
                return false;
            for (const GroupSpec& g : config.groups)
                if (same(g.name, a.substr(6))) {
                    bool all = true;
                    for (const auto& ga : g.actions)
                        all = action(ga.text, name, group, source, sourceMode, when, depth + 1) && all;
                    return all;
                }
            return false;
        }
        for (const char* what : {"chain ", "preset "})
            if (startsWithNoCase(a, what))
                if (const auto* c = layout.chain(a.substr(std::strlen(what)))) {
                    json mode = sourceMode;
                    mode["minTargetValue"] = mode["maxTargetValue"] = c->value;
                    mapping(name, group, source, mode, setParam(0), true, false, when);
                    return true;
                }
        for (const char* what : {"enable ", "bypass ", "toggle "})
            if (startsWithNoCase(a, what)) {
                const auto targets = plugins(a.substr(std::strlen(what)));
                if (targets.empty())
                    return false;
                for (const RppFx* f : targets) {
                    json mode = sourceMode;
                    if (what[0] == 't')
                        mode["type"] = 2; // toggle
                    else
                        mode["minTargetValue"] = mode["maxTargetValue"] = what[0] == 'e' ? 1.0 : 0.0;
                    mapping(name, group, source, mode, {{"type", 12}, {"trackGUID", f->trackGuid}, {"fxAnchor", "id"}, {"fxGUID", f->fxGuid}}, true,
                            false, when);
                }
                return true;
            }
        if (startsWithNoCase(a, "midi ")) {
            json mode = sourceMode;
            mapping(name, group, source, mode, {{"type", 29}, {"sendMidiDestination", "feedback-output"}, {"rawMidiPattern", trim(a.substr(5))}}, true,
                    false, when);
            return true;
        }
        // Control = value.
        const auto eq = a.find('=');
        if (eq == std::string::npos)
            return false;
        const Layout::Control* k = layout.control(a.substr(0, eq));
        if (!k || k->param < 0)
            return false;
        const std::string v = trim(a.substr(eq + 1));
        json mode = sourceMode;
        if (same(v, "next") || same(v, "previous")) {
            mode["type"] = 1; // a press steps it
            mode["minStepSize"] = mode["maxStepSize"] = 0.01;
            mode["minTargetValue"] = 0.0;
            mode["maxTargetValue"] = (k->count - 1) / 100.0;
            mode["rotateIsEnabled"] = k->spec->wrap;
            if (same(v, "previous"))
                mode["reverseIsEnabled"] = true;
        } else if (same(v, "toggle")) {
            mode["type"] = 2;
            mode["minTargetValue"] = 0.0;
            mode["maxTargetValue"] = (k->count - 1) / 100.0;
        } else {
            int i = same(v, "default") ? static_cast<int>(std::lround((k->spec->def - k->spec->min) / (k->spec->step > 0 ? k->spec->step : 1)))
                                       : Layout::index(*k, v);
            if (i < 0)
                return false;
            mode["minTargetValue"] = mode["maxTargetValue"] = i / 100.0;
        }
        mapping(name, group, source, mode, setParam(k->param), true, false, when);
        return true;
    }

    // LED on and off for a pedal, following a condition on one parameter (as
    // y, the parameter's value, which a feedback transformation turns into x):
    // EurekaPROM lights an LED on CC 106 and puts it out on CC 107, both with
    // the LED's number; a value past the LEDs is ignored, which is how one
    // mapping can stay quiet while the other speaks.
    void led(int ledNumber, int paramIndex, const std::string& litExpr, const std::string& name, const std::string& group, const std::string& when) {
        const double v = pedalValue(ledNumber) / 127.0;
        for (bool on : {true, false}) {
            json mode = {{"minSourceValue", v}, {"maxSourceValue", 1.0}, {"outOfRangeBehavior", "ignore"},
                         {"eelFeedbackTransformation", "x = (" + litExpr + ") ? " + (on ? "0" : "1") + " : " + (on ? "1" : "0") + ";"}};
            mapping(name + (on ? " LED on" : " LED off"), group, cc(on ? eurekaprom::kLedOnCC : eurekaprom::kLedOffCC), mode, setParam(paramIndex), false,
                    true, when);
        }
    }

    static std::string hex(uint8_t b) {
        char s[4];
        std::snprintf(s, sizeof s, "%02X", b);
        return s;
    }

    // Inputs a trigger listens to, as ReaLearn sources.
    bool input(const std::string& text, json& source, json& mode) const {
        std::string t = lower(trim(text));
        if (t.size() > 8 && t.compare(t.size() - 8, 8, " release") == 0)
            return false;
        double n = 0;
        if (startsWithNoCase(t, "pedal ")) {
            const std::string p = trim(t.substr(6));
            const int pedal = p == "up" ? eurekaprom::kUp : p == "down" ? eurekaprom::kDown : number(p, n) ? static_cast<int>(n) : 0;
            if (pedal < 1 || pedal > 12)
                return false;
            source = cc(eurekaprom::kPressCC);
            mode = fixedSource(pedalValue(pedal) / 127.0);
            return true;
        }
        int ch = -1;
        if (auto at = t.find(" channel "); at != std::string::npos) {
            if (!number(t.substr(at + 9), n))
                return false;
            ch = static_cast<int>(n) - 1;
            t = trim(t.substr(0, at));
        }
        auto withChannel = [&](json s) {
            if (ch >= 0)
                s["channel"] = ch;
            else
                s.erase("channel");
            return s;
        };
        if (startsWithNoCase(t, "cc ")) {
            std::string rest = trim(t.substr(3));
            std::string value;
            if (auto eq = rest.find('='); eq != std::string::npos) {
                value = trim(rest.substr(eq + 1));
                rest = trim(rest.substr(0, eq));
            } else if (auto sp = rest.find(' '); sp != std::string::npos) {
                value = trim(rest.substr(sp + 1));
                rest = trim(rest.substr(0, sp));
            }
            if (!number(rest, n))
                return false;
            source = withChannel({{"number", static_cast<int>(n)}});
            mode = json::object();
            if (!value.empty()) {
                if (!number(value, n))
                    return false;
                mode = fixedSource(n / 127.0);
            }
            return true;
        }
        if (startsWithNoCase(t, "program ") && number(t.substr(8), n)) {
            source = withChannel({{"type", 5}});
            mode = fixedSource(n / 127.0);
            return true;
        }
        if (startsWithNoCase(t, "note ") && number(t.substr(5), n)) {
            source = withChannel({{"type", 1}, {"number", static_cast<int>(n)}});
            mode = {{"minSourceValue", 1 / 127.0}};
            return true;
        }
        return false;
    }

    void build() {
        const bool eureka = config.midi.profile == "eurekaprom";
        std::set<int> ledsSet; // LEDs an [fcb] line drives
        for (const auto& [n, when] : config.fcb.leds)
            ledsSet.insert(n);
        std::set<int> pedalsUsed;

        // Chains: selecting one, its track, its actions, its LED and display.
        size_t chainIndex = 0;
        for (size_t s = 0; s < config.scenes.size(); ++s) {
            const SceneSpec& scene = config.scenes[s];
            const std::string inScene = config.scenes.size() > 1 ? layout.sceneRange(s) : "";
            for (size_t k = 0; k < scene.chains.size(); ++k, ++chainIndex) {
                const ChainSpec& chain = scene.chains[k];
                const PresetSpec& preset = scene.presets.at(k);
                const double v = layout.chains[chainIndex].value;
                const std::string track = trackOf(scene.name, chain.name);
                const std::string group = "chains";
                if (preset.pedal && eureka) {
                    pedalsUsed.insert(preset.pedal);
                    json mode = fixedSource(pedalValue(preset.pedal) / 127.0);
                    mode["minTargetValue"] = mode["maxTargetValue"] = v;
                    mapping("Pedal " + std::to_string(preset.pedal) + ": " + chain.name, group, cc(eurekaprom::kPressCC), mode, setParam(0), true, false,
                            inScene);
                    if (!ledsSet.count(preset.pedal))
                        led(preset.pedal, 0, Layout::is("y", v), chain.name, "leds", inScene);
                }
                if (preset.program >= 0) {
                    json mode = fixedSource(preset.program / 127.0);
                    mode["minTargetValue"] = mode["maxTargetValue"] = v;
                    mapping("Program " + std::to_string(preset.program) + ": " + chain.name, group, {{"channel", channel}, {"type", 5}}, mode,
                            setParam(0));
                }
                if (!track.empty()) {
                    // In range: playing, so not muted; out of range: muted.
                    json mode = fixedSource(v);
                    mode["outOfRangeBehavior"] = "min";
                    mode["reverseIsEnabled"] = true;
                    mapping(chain.name + " plays", group, param(0), mode, {{"type", 7}, {"trackGUID", track}});
                }
                json when = fixedSource(v);
                for (const auto& a : preset.actions)
                    if (!action(a.text, chain.name + ": " + a.text, "chains", param(0), when, ""))
                        leave("\"" + a.text + "\" when " + chain.name + " is selected");
                if (eureka) {
                    std::string text = !preset.display.empty() ? preset.display : !scene.display.empty() ? scene.display : std::to_string(chainIndex + 1);
                    int part = 0;
                    for (const MidiMessage& m : eurekaprom::displayText(channel, text))
                        mapping(chain.name + " display " + std::to_string(++part), "display", param(0), fixedSource(v),
                                {{"type", 29},
                                 {"sendMidiDestination", "feedback-output"},
                                 {"rawMidiPattern", hex(m.status) + " " + hex(m.data1) + " " + hex(m.data2)}});
                }
            }
            if (scene.pedal && eureka && !scene.chains.empty()) {
                pedalsUsed.insert(scene.pedal);
                json mode = fixedSource(pedalValue(scene.pedal) / 127.0);
                mode["minTargetValue"] = mode["maxTargetValue"] = layout.chains[chainIndex - scene.chains.size()].value;
                mapping("Pedal " + std::to_string(scene.pedal) + ": scene " + scene.name, "chains", cc(eurekaprom::kPressCC), mode, setParam(0));
            }
            if (scene.program >= 0)
                leave("scene program changes");
        }

        // Controls.
        for (const Layout::Control& k : layout.controls) {
            const ControlSpec& ct = *k.spec;
            if (k.param < 0) {
                leave("control " + ct.name + " (continuous controls are not in REAPER)");
                continue;
            }
            const std::string var = "x";
            if (ct.pedal && eureka) {
                pedalsUsed.insert(ct.pedal);
                json mode = fixedSource(pedalValue(ct.pedal) / 127.0);
                mode["type"] = 1;
                mode["minStepSize"] = mode["maxStepSize"] = 0.01;
                mode["minTargetValue"] = 0.0;
                mode["maxTargetValue"] = (k.count - 1) / 100.0;
                mode["rotateIsEnabled"] = ct.wrap;
                mapping("Pedal " + std::to_string(ct.pedal) + ": " + ct.name, "controls", cc(eurekaprom::kPressCC), mode, setParam(k.param));
                if (!ledsSet.count(ct.pedal))
                    led(ct.pedal, k.param, "y > 0.005", ct.name, "leds", "");
            }
            if (!ct.follows.text.empty())
                leave("control " + ct.name + " follows " + ct.follows.text);
            for (const auto& e : ct.enables) {
                std::string what = e.text, when;
                const std::string l = lower(e.text);
                if (auto at = l.find(" when "); at != std::string::npos) {
                    what = e.text.substr(0, at);
                    when = e.text.substr(at + 6);
                }
                std::string expr = when.empty() ? Layout::p(k.param) + " > 0.005" : layout.condition(when, &k);
                const std::string own = Layout::p(k.param);
                bool onlyOwn = !expr.empty();
                for (size_t at = expr.find("p["); onlyOwn && at != std::string::npos; at = expr.find("p[", at + 1))
                    onlyOwn = expr.compare(at, own.size(), own) == 0;
                if (!onlyOwn) {
                    leave("control " + ct.name + ": enable = " + e.text + " (its condition reads more than this control)");
                    continue;
                }
                for (size_t at; (at = expr.find(own)) != std::string::npos;)
                    expr.replace(at, own.size(), var);
                const auto targets = plugins(what);
                if (targets.empty())
                    leave("control " + ct.name + ": enable = " + e.text + " (no such plug-in)");
                for (const RppFx* f : targets)
                    mapping(ct.name + " switches " + f->chain + "/" + f->label, "controls", param(k.param),
                            {{"eelControlTransformation", "y = (" + expr + ") ? 1 : 0;"}},
                            {{"type", 12}, {"trackGUID", f->trackGuid}, {"fxAnchor", "id"}, {"fxGUID", f->fxGuid}});
            }
            for (const auto& d : ct.drives)
                leave("control " + ct.name + ": drive = " + d.text + " (REAPER numbers parameters differently)");
            for (const auto& m : ct.mutes)
                leave("control " + ct.name + ": mute = " + m.text + " (muting the outputs is Cue's)");
        }

        // Triggers.
        for (const TriggerSpec& t : config.triggers) {
            std::string when;
            if (!t.when.text.empty()) {
                when = layout.condition(t.when.text, nullptr);
                if (when.empty()) {
                    leave("trigger " + t.name + " (its condition)");
                    continue;
                }
            }
            if (!t.scene.empty())
                for (size_t s = 0; s < config.scenes.size(); ++s)
                    if (same(config.scenes[s].name, t.scene))
                        when = when.empty() ? layout.sceneRange(s) : "(" + when + ") && (" + layout.sceneRange(s) + ")";
            for (const auto& in : t.inputs) {
                json source, mode;
                if (!input(in.text, source, mode)) {
                    leave("trigger " + t.name + ": on = " + in.text);
                    continue;
                }
                if (startsWithNoCase(in.text, "pedal ")) {
                    double n = 0;
                    if (number(in.text.substr(6), n))
                        pedalsUsed.insert(static_cast<int>(n));
                }
                for (const auto& a : t.actions)
                    if (!action(a.text, t.name + ": " + a.text, "triggers", source, mode, when))
                        leave("trigger " + t.name + ": do = " + a.text);
            }
        }

        // Up and down step through the chains, unless something else uses them.
        if (eureka && layout.chains.size() > 1)
            for (int pedal : {eurekaprom::kUp, eurekaprom::kDown}) {
                if (pedalsUsed.count(pedal))
                    continue;
                json mode = fixedSource(pedalValue(pedal) / 127.0);
                mode["type"] = 1;
                mode["minStepSize"] = mode["maxStepSize"] = 0.01;
                mode["minTargetValue"] = layout.chains.front().value;
                mode["maxTargetValue"] = layout.chains.back().value;
                if (pedal == eurekaprom::kDown)
                    mode["reverseIsEnabled"] = true;
                mapping(pedal == eurekaprom::kUp ? "Up: next chain" : "Down: previous chain", "chains", cc(eurekaprom::kPressCC), mode, setParam(0));
            }

        // [fcb] LEDs that follow one parameter.
        for (const auto& [n, when] : config.fcb.leds) {
            std::string expr = layout.condition(when.text, nullptr);
            int param = -1;
            bool one = !expr.empty();
            for (size_t at = expr.find("p["); one && at != std::string::npos; at = expr.find("p[", at + 1)) {
                const int i = std::atoi(expr.c_str() + at + 2);
                one = param < 0 || param == i;
                param = i;
            }
            if (!one || param < 0) {
                leave("led " + std::to_string(n) + " = " + when.text);
                continue;
            }
            const std::string var = Layout::p(param);
            for (size_t at; (at = expr.find(var)) != std::string::npos;)
                expr.replace(at, var.size(), "y");
            led(n, param, expr, "LED " + std::to_string(n), "leds", "");
        }
        if (!config.fcb.displays.empty())
            leave("[fcb] display lines (the display shows each chain's own)");
        if (!eureka && (!config.controls.empty() || !config.triggers.empty()))
            leave("pedals of controller profile " + config.midi.profile + " (only EurekaPROM's are written)");
    }
};

} // namespace

// ---------------------------------------------------------------- writing

std::string cueMappingNotes(const Config& c) {
    json j;
    j["cue_mappings"] = 1;
    json ins = json::array(), outs = json::array();
    for (size_t i = 0; i < c.inputs.size(); ++i)
        ins.push_back({{"name", c.inputs[i]}, {"channel", i < c.audio.inputChannels.size() ? c.audio.inputChannels[i] : i}});
    for (size_t i = 0; i < c.outputs.size(); ++i)
        outs.push_back({{"name", c.outputs[i]}, {"channel", i < c.audio.outputChannels.size() ? c.audio.outputChannels[i] : i}});
    j["inputs"] = ins;
    j["outputs"] = outs;
    json scenes = json::array();
    for (const SceneSpec& s : c.scenes) {
        json sj = {{"name", s.name}, {"display", s.display}, {"pedal", s.pedal}, {"program", s.program}};
        json chains = json::array();
        for (size_t k = 0; k < s.chains.size(); ++k) {
            const PresetSpec& p = s.presets.at(k);
            json on = json::array();
            for (const auto& a : p.actions)
                on.push_back(a.text);
            json branches = json::array();
            for (size_t l = 1; l < s.chains[k].lines.size(); ++l)
                branches.push_back(s.chains[k].lines[l].name);
            chains.push_back({{"name", s.chains[k].name},
                              {"display", p.display},
                              {"pedal", p.pedal},
                              {"program", p.program},
                              {"on_select", on},
                              {"warm", p.warm},
                              {"branches", branches}});
        }
        sj["chains"] = chains;
        scenes.push_back(sj);
    }
    j["scenes"] = scenes;
    j["ini"] = mappingsIni(c);
    const std::string text = j.dump();
    const std::string b = base64(std::vector<uint8_t>(text.begin(), text.end()));
    std::ostringstream o;
    o << "  <NOTES 0 2\n"
      << "    |Made by Cue Converter from the Cue performance " << c.performance << ".\n"
      << "    |Its pedals are in the \"Cue pedals\" track (ReaLearn). The lines below carry them back to Cue: leave them be.\n";
    for (size_t i = 0; i < b.size(); i += 120)
        o << "    |cue:" << b.substr(i, 120) << "\n";
    o << "  >\n";
    return o.str();
}

std::vector<uint8_t> realearnChunk(const std::string& text) {
    std::vector<uint8_t> out;
    put32(out, 1751282284); // ReaLearn's VST2 id
    put32(out, 0xfeed5eee);
    put32(out, 2);
    put32(out, 1);
    put32(out, 0);
    put32(out, 2);
    put32(out, 0);
    put32(out, 0);
    put32(out, static_cast<uint32_t>(text.size()));
    put32(out, 1);
    put32(out, 0);
    return out;
}

bool realearnJson(const std::vector<uint8_t>& body, std::string& text) {
    size_t at = 8;
    const uint32_t nIn = get32(body, at);
    at += 4 + 8ull * nIn;
    const uint32_t nOut = get32(body, at);
    at += 4 + 8ull * nOut;
    const uint32_t size = get32(body, at);
    at += 12;
    if (nIn > 64 || nOut > 64 || at + size > body.size() || size < 2)
        return false;
    text.assign(body.begin() + at, body.begin() + at + size);
    while (!text.empty() && text.back() == 0)
        text.pop_back();
    return !text.empty() && text.front() == '{';
}

std::string realearnTrack(const Config& config, const std::vector<RppChain>& chains, const std::vector<RppFx>& fx, std::vector<std::string>& notes) {
    Layout layout(config);
    Builder b{config, layout, chains, fx, notes, json::array(), {}, config.midi.channel};
    b.build();
    if (b.mappings.empty())
        return "";
    json params = json::object();
    params["0"] = {{"name", "Chain"}, {"value", layout.chains.front().value}};
    for (const auto& k : layout.controls)
        if (k.param >= 0)
            params[std::to_string(k.param)] = {
                {"name", k.spec->name},
                {"value", std::lround((k.spec->def - k.spec->min) / (k.spec->step > 0 ? k.spec->step : 1)) / 100.0}};
    json unit = {{"version", "2.18.2"},
                 {"id", "cuepedal"},
                 {"name", "Cue pedals"},
                 {"controlDeviceId", "1"},
                 {"feedbackDeviceId", "1"},
                 {"groups",
                  json::array({{{"id", "chains"}, {"name", "Chains"}},
                               {{"id", "controls"}, {"name", "Controls"}},
                               {{"id", "triggers"}, {"name", "Triggers"}},
                               {{"id", "leds"}, {"name", "LEDs"}},
                               {{"id", "display"}, {"name", "Display"}}})},
                 {"mappings", b.mappings},
                 {"parameters", params}};
    const std::string text = json({{"mainUnit", unit}, {"additionalUnits", json::array()}}).dump();
    notes.push_back("pedals: ReaLearn listens to and lights REAPER's MIDI device 1; pick your controller in ReaLearn if it is another one");

    auto lines = [](const std::vector<uint8_t>& data) {
        const std::string s = base64(data);
        std::string out;
        for (size_t i = 0; i < s.size(); i += 128)
            out += "        " + s.substr(i, 128) + "\n";
        return out;
    };
    const std::string id = "{C0E0C0E0-0000-4000-8000-00000000C0E0}";
    std::ostringstream o;
    o << "  <TRACK " << id << "\n    NAME \"Cue pedals\"\n    MUTESOLO 0 0 0\n    ISBUS 0 0\n"
      << "    REC 1 6112 1 0 0 0 0 0\n    NCHAN 2\n    FX 1\n    TRACKID " << id << "\n    MAINSEND 0 0\n"
      << "    <FXCHAIN\n      SHOW 0\n      LASTSEL 0\n      DOCKED 0\n      BYPASS 0 0 0\n"
      << "      <VST \"VSTi: Helgobox - ReaLearn & Playtime (Helgoboss)\" helgobox-x64.so 0 \"\" 1751282284<5653546862726C68656C676F626F7872> \"\"\n"
      << lines(realearnChunk(text)) << lines(std::vector<uint8_t>(text.begin(), text.end()))
      << lines(std::vector<uint8_t>{0, 'P', 'r', 'o', 'g', 'r', 'a', 'm', ' ', '1', 0, 0, 0, 0, 0}) << "      >\n"
      << "      FXID {C0E0C0E0-0000-4000-8000-00000000C0E1}\n      WAK 0 0\n    >\n  >\n";
    return o.str();
}

// ---------------------------------------------------------------- reading

CueMappings readCueMappings(const std::vector<std::string>& notesLines) {
    CueMappings m;
    std::string b;
    for (const auto& raw : notesLines) {
        const std::string l = trim(raw);
        if (l.rfind("|cue:", 0) == 0)
            b += l.substr(5);
    }
    std::vector<uint8_t> bytes;
    if (b.empty() || !unbase64(b, bytes))
        return m;
    try {
        const json j = json::parse(bytes.begin(), bytes.end());
        if (j.value("cue_mappings", 0) != 1)
            return m;
        for (const auto& p : j.at("inputs"))
            m.inputs.push_back({p.at("name").get<std::string>(), p.at("channel").get<int>()});
        for (const auto& p : j.at("outputs"))
            m.outputs.push_back({p.at("name").get<std::string>(), p.at("channel").get<int>()});
        for (const auto& sj : j.at("scenes")) {
            CueMappings::Scene s;
            s.name = sj.at("name").get<std::string>();
            s.display = sj.value("display", "");
            s.pedal = sj.value("pedal", 0);
            s.program = sj.value("program", -1);
            for (const auto& cj : sj.at("chains")) {
                CueMappings::Chain c;
                c.name = cj.at("name").get<std::string>();
                c.display = cj.value("display", "");
                c.pedal = cj.value("pedal", 0);
                c.program = cj.value("program", -1);
                c.warm = cj.value("warm", "");
                for (const auto& a : cj.value("on_select", json::array()))
                    c.onSelect.push_back(a.get<std::string>());
                for (const auto& n : cj.value("branches", json::array()))
                    c.branches.push_back(n.get<std::string>());
                s.chains.push_back(c);
            }
            m.scenes.push_back(s);
        }
        m.ini = j.value("ini", "");
        m.found = true;
    } catch (const std::exception&) {
        return CueMappings{};
    }
    return m;
}

std::string realearnTriggers(const std::string& text, const std::map<std::string, std::string>& trackChain,
                             const std::map<std::string, std::string>& fxPlugin, bool skipOwn, std::vector<std::string>& notes) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        notes.push_back("ReaLearn: its settings could not be read");
        return "";
    }
    std::map<std::string, std::string> groups;
    std::vector<json> mappings;
    for (const char* unit : {"mainUnit"}) {
        if (!j.contains(unit))
            continue;
        for (const auto& g : j[unit].value("groups", json::array()))
            groups[g.value("id", "")] = g.value("name", "");
        for (const auto& mp : j[unit].value("mappings", json::array()))
            mappings.push_back(mp);
    }
    auto upper = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '{' || c == '}'; }), s.end());
        return s;
    };
    std::ostringstream o;
    std::vector<std::string> formula, other;
    int converted = 0, feedback = 0;
    for (const json& mp : mappings) {
        const std::string id = mp.value("id", "");
        if (skipOwn && id.rfind("cue-", 0) == 0)
            continue;
        if (!mp.value("isEnabled", true))
            continue;
        std::string name = mp.value("name", "");
        const std::string group = groups.count(mp.value("groupId", "")) ? groups[mp.value("groupId", "")] : "";
        const std::string label = (group.empty() ? "" : group + ": ") + (name.empty() ? "mapping " + id : name);
        if (!mp.value("controlIsEnabled", true)) {
            ++feedback; // LEDs and display: Cue drives them itself
            continue;
        }
        const json source = mp.value("source", json::object()), mode = mp.value("mode", json::object()), target = mp.value("target", json::object());
        if (target.value("type", 1) == 29 && target.value("rawMidiPattern", "").find('[') != std::string::npos) {
            ++feedback; // letters on the display
            continue;
        }
        const std::string activation = mp.value("activationType", "always");
        if (activation != "always" || source.value("category", "midi") != "midi" || target.value("category", "reaper") != "reaper") {
            formula.push_back(label);
            continue;
        }
        // The input.
        const double minS = mode.value("minSourceValue", 0.0), maxS = mode.value("maxSourceValue", 1.0);
        const bool fixed = std::abs(minS - maxS) < 1e-6;
        const int v = static_cast<int>(std::lround(minS * 127));
        const int type = source.value("type", 0);
        const int ch = source.contains("channel") && source["channel"].is_number() ? source["channel"].get<int>() : -1;
        const std::string chText = ch >= 0 ? " channel " + std::to_string(ch + 1) : "";
        std::string in;
        if (type == 0 && source.contains("number") && source["number"].is_number()) {
            const int number = source["number"].get<int>();
            if (number == eurekaprom::kPressCC && fixed) {
                const int pedal = eurekaprom::pedalFromValue(static_cast<uint8_t>(v));
                in = pedal == eurekaprom::kUp ? "pedal up" : pedal == eurekaprom::kDown ? "pedal down" : "pedal " + std::to_string(pedal);
            } else {
                in = "cc " + std::to_string(number) + (fixed ? " = " + std::to_string(v) : "") + chText;
            }
        } else if (type == 5 && fixed) {
            in = "program " + std::to_string(v) + chText;
        } else if (type == 1 && source.contains("number")) {
            in = "note " + std::to_string(source["number"].get<int>()) + chText;
        } else {
            other.push_back(label);
            continue;
        }
        // The action.
        const int ttype = target.value("type", 1);
        const double minT = mode.value("minTargetValue", 0.0), maxT = mode.value("maxTargetValue", 1.0);
        const bool fixedT = std::abs(minT - maxT) < 1e-6;
        const auto track = trackChain.find(upper(target.value("trackGUID", "")));
        const auto plugin = fxPlugin.find(upper(target.value("fxGUID", "")));
        std::string act;
        if (ttype == 12 && plugin != fxPlugin.end()) {
            act = mode.value("type", 0) == 2 || !fixedT ? "toggle " : maxT > 0.5 ? "enable " : "bypass ";
            act += plugin->second;
        } else if ((ttype == 6 || ttype == 7 || ttype == 8) && track != trackChain.end()) {
            act = "chain " + track->second; // selecting, unmuting or soloing a chain's track plays it
        } else if (ttype == 29 && target.value("rawMidiPattern", "").find('[') == std::string::npos && !target.value("rawMidiPattern", "").empty()) {
            act = "midi " + target.value("rawMidiPattern", "");
        } else {
            other.push_back(label);
            continue;
        }
        o << "\n[trigger " << (name.empty() ? "ReaLearn " + id : name) << "]\non = " << in << "\ndo = " << act << "\n";
        ++converted;
    }
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 6; ++i)
            s += (i ? ", " : "") + v[i];
        if (v.size() > 6)
            s += " and " + std::to_string(v.size() - 6) + " more";
        return s;
    };
    if (converted)
        notes.push_back("ReaLearn: " + std::to_string(converted) + " mapping(s) became triggers");
    if (!formula.empty())
        notes.push_back("ReaLearn: " + std::to_string(formula.size()) +
                        " mapping(s) use ReaLearn parameters or conditions and were not converted; map these pedals in Cue: " + list(formula));
    if (!other.empty())
        notes.push_back("ReaLearn: " + std::to_string(other.size()) + " mapping(s) have no Cue equivalent: " + list(other));
    if (feedback && !skipOwn)
        notes.push_back("ReaLearn: " + std::to_string(feedback) + " LED or display mapping(s) left out; Cue lights the FCB itself");
    return o.str();
}

} // namespace cue
