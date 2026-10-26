#include "show.h"

#include "midi/eurekaprom.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>

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

bool same(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool toNumber(const std::string& s, double& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

// Finds " word " (any case) and returns its position, the last one if last.
size_t findWord(const std::string& text, const std::string& word, bool last = false) {
    const std::string l = lower(text), w = " " + lower(word) + " ";
    return last ? l.rfind(w) : l.find(w);
}

std::string format(double v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 1e-9)
        std::snprintf(buf, sizeof buf, "%.0f", v);
    else
        std::snprintf(buf, sizeof buf, "%.3g", v);
    return buf;
}

// An input a trigger listens to.
struct Input {
    enum class Kind { Pedal, PedalRelease, Expression, CC, Program, Note, Key } kind = Kind::CC;
    int number = 0;   // pedal 1-12, expression 1-2, cc/program/note number, key character
    int value = -1;   // cc value to match, -1 = any
    int channel = -1; // 0-based, -1 = any
};

bool parseInput(const std::string& text, Input& in, std::string* error) {
    std::istringstream ss(text);
    std::vector<std::string> w;
    for (std::string s; ss >> s;)
        w.push_back(s);
    auto bad = [&](const std::string& why) {
        *error = "input '" + text + "': " + why;
        return false;
    };
    if (w.empty())
        return bad("empty");
    const std::string kind = lower(w[0]);
    auto number = [&](size_t i, int lo, int hi, int& out) {
        double n = 0;
        if (i >= w.size() || !toNumber(w[i], n) || n < lo || n > hi)
            return false;
        out = static_cast<int>(n);
        return true;
    };
    size_t next = 2;
    if (kind == "key") {
        if (w.size() != 2 || w[1].size() != 1)
            return bad("key takes one character, like key t");
        in.kind = Input::Kind::Key;
        in.number = static_cast<unsigned char>(w[1][0]);
        return true;
    }
    if (kind == "pedal") {
        in.kind = Input::Kind::Pedal;
        const std::string p = w.size() > 1 ? lower(w[1]) : "";
        if (p == "up") in.number = eurekaprom::kUp;
        else if (p == "down") in.number = eurekaprom::kDown;
        else if (!number(1, 1, 10, in.number)) return bad("pedal is 1 to 10, up or down");
        if (w.size() > 2) {
            const std::string how = lower(w[2]);
            if (how == "release" || how == "released")
                in.kind = Input::Kind::PedalRelease;
            else if (how != "press" && how != "pressed")
                return bad("after the pedal number: press or release");
        }
        return w.size() <= 3 || bad("too many words");
    }
    if (kind == "expression") {
        in.kind = Input::Kind::Expression;
        const std::string e = w.size() > 1 ? lower(w[1]) : "";
        if (e == "left" || e == "a") in.number = 1;
        else if (e == "right" || e == "b") in.number = 2;
        else if (!number(1, 1, 2, in.number)) return bad("expression is 1 (left) or 2 (right)");
        return w.size() == 2 || bad("too many words");
    }
    if (kind == "cc") in.kind = Input::Kind::CC;
    else if (kind == "program" || kind == "pc") in.kind = Input::Kind::Program;
    else if (kind == "note") in.kind = Input::Kind::Note;
    else return bad("inputs are pedal, expression, cc, program, note or key");
    if (!number(1, 0, 127, in.number))
        return bad("needs a number 0-127");
    // cc 64 = 127, cc 64 127; then optionally channel N.
    if (in.kind == Input::Kind::CC && next < w.size() && w[next] == "=")
        ++next;
    if (in.kind == Input::Kind::CC && next < w.size() && lower(w[next]) != "channel" && lower(w[next]) != "ch") {
        if (!number(next, 0, 127, in.value))
            return bad("the value is 0-127");
        ++next;
    }
    if (next < w.size()) {
        const std::string c = lower(w[next]);
        int ch = 0;
        if ((c != "channel" && c != "ch") || !number(next + 1, 1, 16, ch) || next + 2 != w.size())
            return bad("ends with channel N, 1-16");
        in.channel = ch - 1;
    }
    return true;
}

std::string describe(const Input& in) {
    switch (in.kind) {
    case Input::Kind::Pedal:
    case Input::Kind::PedalRelease: {
        const std::string p = in.number == eurekaprom::kUp ? "up" : in.number == eurekaprom::kDown ? "down" : std::to_string(in.number);
        return "pedal " + p + (in.kind == Input::Kind::PedalRelease ? " release" : "");
    }
    case Input::Kind::Expression: return "expression " + std::to_string(in.number);
    case Input::Kind::CC: return "cc " + std::to_string(in.number) + (in.value >= 0 ? " = " + std::to_string(in.value) : "");
    case Input::Kind::Program: return "program " + std::to_string(in.number);
    case Input::Kind::Note: return "note " + std::to_string(in.number);
    case Input::Kind::Key: return std::string("key ") + static_cast<char>(in.number);
    }
    return "?";
}

// An event that arrived, to match against inputs.
struct Event {
    Input::Kind kind;
    int number = 0;
    int value = 0;    // 0-127 for continuous inputs
    int channel = -1;
};

bool matches(const Input& in, const Event& e) {
    if (in.kind != e.kind || in.number != e.number)
        return false;
    if (in.channel >= 0 && e.channel >= 0 && in.channel != e.channel)
        return false;
    return in.value < 0 || in.value == e.value;
}

} // namespace

struct Show::Impl {
    explicit Impl(ShowOutput& o) : out(o) {}

    // ---------------------------------------------------------------- model

    struct Control {
        std::string name;
        ControlSpec::Type type;
        double min, max, step, def, value;
        bool wrap;
        std::vector<std::string> labels;
        int pedal;
        bool hasFollow = false;
        Input follow;
        std::string followText;
        double drivenValue = std::numeric_limits<double>::quiet_NaN(); // last value sent to drives
    };

    // A condition, as a tree of nodes.
    struct Node {
        enum class Op { True, Truthy, Cmp, Not, And, Or, PresetIs, SceneIs } op = Op::True;
        int a = -1, b = -1;   // children
        int index = -1;       // control, preset or scene
        std::string cmp;      // = != < <= > >=
        double value = 0;
    };
    struct Condition {
        std::vector<Node> nodes;
        int root = -1;
        bool always() const { return root < 0; }
    };

    struct Action {
        enum class Type { Set, Step, Toggle, Default, Group, Preset, PresetStep, Scene, SceneStep, Bypass, Enable, TogglePlugin, Param, Mute, Midi } type;
        int index = -1;              // control, group, preset or scene
        double value = 0;            // Set: value; Step: amount (+/-); Mute: 1 on, 0 off, -1 toggle
        std::vector<int> plugins;    // indexes into plugins
        uint32_t paramId = 0;
        std::vector<uint8_t> bytes;  // Midi
        std::string text, where;
    };

    static constexpr int kWarmNext = -2, kWarmNone = -1;
    struct Preset {
        std::string name, display;
        int scene = 0, chain = 0, pedal = 0, program = -1;
        int warm = kWarmNext; // kWarmNext, kWarmNone or the engine chain kept warm while it plays
        std::vector<Action> actions;
    };
    struct Scene {
        std::string name, display;
        int pedal = 0, program = -1;
        int firstPreset = 0, numPresets = 0;
    };
    struct Trigger {
        std::string name;
        std::vector<Input> inputs;
        int scene = -1; // -1 = everywhere
        Condition when;
        std::vector<Action> actions;
    };
    struct Group {
        std::string name;
        std::vector<Action> actions;
    };
    struct Enable {
        std::vector<int> plugins;
        Condition when;
        int control;
    };
    struct Drive {
        int control;
        std::vector<int> plugins;
        uint32_t paramId;
        double from, to;
    };
    struct Display {
        std::string text;
        Condition when;
    };

    ShowOutput& out;
    mutable std::mutex mutex;
    Config config;
    std::vector<ShowPlugin> plugins;
    std::vector<Control> controls;
    std::vector<Preset> presets;
    std::vector<Scene> scenes;
    std::vector<Trigger> triggers;
    std::vector<Group> groups;
    std::vector<Enable> enables;
    std::vector<Drive> drives;
    std::vector<Condition> mutes;
    std::vector<std::pair<int, Condition>> ledRules;
    std::vector<Display> displays;
    std::vector<std::string> warnings;

    // ---------------------------------------------------------------- state

    int preset = -1, scene = -1, activeChain = -1, warmChain = -2;
    bool manualMute = false;
    std::vector<int> bypassed; // per plug-in: 1 bypassed, 0 running, -1 not yet sent
    int sentMute = -1;
    int sentLeds[eurekaprom::kLastLed + 1];
    std::string sentDisplay = "\x01"; // nothing sent yet
    int runDepth = 0;

    // ---------------------------------------------------------------- names

    int findControl(const std::string& name) const {
        for (size_t i = 0; i < controls.size(); ++i)
            if (same(controls[i].name, name))
                return static_cast<int>(i);
        return -1;
    }
    int findGroup(const std::string& name) const {
        for (size_t i = 0; i < groups.size(); ++i)
            if (same(groups[i].name, name))
                return static_cast<int>(i);
        return -1;
    }
    int findPreset(const std::string& name) const {
        for (size_t i = 0; i < presets.size(); ++i)
            if (same(presets[i].name, name))
                return static_cast<int>(i);
        return -1;
    }
    int findScene(const std::string& name) const {
        for (size_t i = 0; i < scenes.size(); ++i)
            if (same(scenes[i].name, name))
                return static_cast<int>(i);
        return -1;
    }

    // "Wah", "Punk/Wah", "*/Wah", "Wah:2" (the second Wah of each chain).
    bool resolvePlugins(const std::string& ref, std::vector<int>& result, std::string* error) const {
        std::string label = trim(ref), chainName;
        const auto slash = label.find('/');
        if (slash != std::string::npos) {
            chainName = trim(label.substr(0, slash));
            label = trim(label.substr(slash + 1));
            if (chainName == "*")
                chainName.clear();
        }
        int nth = 0;
        const auto colon = label.rfind(':');
        double n = 0;
        if (colon != std::string::npos && toNumber(trim(label.substr(colon + 1)), n) && n >= 1) {
            nth = static_cast<int>(n);
            label = trim(label.substr(0, colon));
        }
        int chainIndex = -1;
        if (!chainName.empty()) {
            for (size_t i = 0; i < chainNames.size(); ++i)
                if (same(chainNames[i], chainName))
                    chainIndex = static_cast<int>(i);
            if (chainIndex < 0) {
                *error = "no chain named " + chainName;
                return false;
            }
        }
        result.clear();
        std::vector<int> count(chainNames.size(), 0);
        for (size_t i = 0; i < plugins.size(); ++i) {
            const ShowPlugin& p = plugins[i];
            if ((chainIndex >= 0 && p.chain != chainIndex) || !same(p.label, label))
                continue;
            ++count[p.chain];
            if (nth == 0 || count[p.chain] == nth)
                result.push_back(static_cast<int>(i));
        }
        if (result.empty()) {
            *error = "no plug-in named '" + label + "'" + (nth ? " (number " + std::to_string(nth) + ")" : "") +
                     (chainIndex >= 0 ? " in chain " + chainName : "") + "; give one a name with `name = " + label + "` after its plugin line";
            return false;
        }
        return true;
    }
    std::vector<std::string> chainNames;

    // ---------------------------------------------------------------- conditions

    struct Cursor {
        const std::string& s;
        size_t pos = 0;
        void skip() {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
                ++pos;
        }
        bool end() {
            skip();
            return pos >= s.size();
        }
        // Matches text (any case) at the cursor, ending at a word boundary.
        bool word(const std::string& w) {
            skip();
            if (s.size() - pos < w.size() || !same(s.substr(pos, w.size()), w))
                return false;
            const size_t e = pos + w.size();
            if (e < s.size() && std::isalnum(static_cast<unsigned char>(s[e])) && std::isalnum(static_cast<unsigned char>(w.back())))
                return false;
            pos = e;
            return true;
        }
        // The longest of names at the cursor; -1 if none.
        int longest(const std::vector<std::string>& names) {
            skip();
            int best = -1;
            size_t bestLen = 0;
            for (size_t i = 0; i < names.size(); ++i) {
                const std::string& n = names[i];
                if (n.size() <= bestLen)
                    continue;
                const size_t save = pos;
                if (word(n)) {
                    best = static_cast<int>(i);
                    bestLen = n.size();
                }
                pos = save;
            }
            if (best >= 0)
                pos += bestLen;
            return best;
        }
        bool number(double& v) {
            skip();
            const char* start = s.c_str() + pos;
            char* end = nullptr;
            v = std::strtod(start, &end);
            if (end == start)
                return false;
            pos += static_cast<size_t>(end - start);
            return true;
        }
        std::string op() {
            skip();
            for (const char* o : {"==", "!=", "<=", ">=", "=", "<", ">"})
                if (s.compare(pos, std::strlen(o), o) == 0) {
                    pos += std::strlen(o);
                    return std::string(o) == "==" ? "=" : o;
                }
            const size_t save = pos;
            if (word("is")) {
                if (word("not"))
                    return "!=";
                return "=";
            }
            pos = save;
            return "";
        }
    };

    // A value of control c: one of its names, a number, on/off, min/max.
    bool controlValue(Cursor& cur, int c, double& v) const {
        const Control& k = controls[c];
        const int label = cur.longest(k.labels);
        if (label >= 0) {
            v = k.min + label * k.step;
            return true;
        }
        if (cur.number(v))
            return true;
        if (cur.word("on") || cur.word("max")) {
            v = k.max;
            return true;
        }
        if (cur.word("off") || cur.word("min")) {
            v = k.min;
            return true;
        }
        return false;
    }

    std::vector<std::string> controlNames() const {
        std::vector<std::string> n;
        for (const auto& c : controls)
            n.push_back(c.name);
        return n;
    }
    std::vector<std::string> presetNames() const {
        std::vector<std::string> n;
        for (const auto& p : presets)
            n.push_back(p.name);
        return n;
    }
    std::vector<std::string> sceneNames() const {
        std::vector<std::string> n;
        for (const auto& s : scenes)
            n.push_back(s.name);
        return n;
    }

    int add(Condition& c, Node n) const {
        c.nodes.push_back(n);
        return static_cast<int>(c.nodes.size()) - 1;
    }

    int parseOr(Cursor& cur, Condition& c, int context, std::string* error) const {
        int left = parseAnd(cur, c, context, error);
        while (left >= 0 && cur.word("or")) {
            const int right = parseAnd(cur, c, context, error);
            if (right < 0)
                return -1;
            Node n;
            n.op = Node::Op::Or;
            n.a = left;
            n.b = right;
            left = add(c, n);
        }
        return left;
    }
    int parseAnd(Cursor& cur, Condition& c, int context, std::string* error) const {
        int left = parseUnary(cur, c, context, error);
        while (left >= 0 && cur.word("and")) {
            const int right = parseUnary(cur, c, context, error);
            if (right < 0)
                return -1;
            Node n;
            n.op = Node::Op::And;
            n.a = left;
            n.b = right;
            left = add(c, n);
        }
        return left;
    }
    int parseUnary(Cursor& cur, Condition& c, int context, std::string* error) const {
        if (cur.word("not")) {
            const int inner = parseUnary(cur, c, context, error);
            if (inner < 0)
                return -1;
            Node n;
            n.op = Node::Op::Not;
            n.a = inner;
            return add(c, n);
        }
        cur.skip();
        if (cur.pos < cur.s.size() && cur.s[cur.pos] == '(') {
            ++cur.pos;
            const int inner = parseOr(cur, c, context, error);
            cur.skip();
            if (inner < 0)
                return -1;
            if (cur.pos >= cur.s.size() || cur.s[cur.pos] != ')') {
                *error = "missing )";
                return -1;
            }
            ++cur.pos;
            return inner;
        }
        return parseAtom(cur, c, context, error);
    }
    int parseAtom(Cursor& cur, Condition& c, int context, std::string* error) const {
        Node n;
        const size_t start = cur.pos;
        if (cur.word("always") || cur.word("true")) {
            n.op = Node::Op::True;
            return add(c, n);
        }
        for (const char* what : {"preset", "chain", "scene"}) {
            const size_t save = cur.pos;
            if (!cur.word(what))
                continue;
            const std::string op = cur.op();
            if (op != "=" && op != "!=") {
                cur.pos = save; // a control may be called "Preset ..."
                break;
            }
            const bool isPreset = std::string(what) != "scene"; // a chain is selected by its preset
            const int index = cur.longest(isPreset ? presetNames() : sceneNames());
            if (index < 0) {
                *error = std::string("no ") + what + " named '" + trim(cur.s.substr(cur.pos)) + "'";
                return -1;
            }
            n.op = isPreset ? Node::Op::PresetIs : Node::Op::SceneIs;
            n.index = index;
            const int node = add(c, n);
            if (op == "=")
                return node;
            Node notNode;
            notNode.op = Node::Op::Not;
            notNode.a = node;
            return add(c, notNode);
        }
        const int control = cur.longest(controlNames());
        if (control >= 0) {
            const size_t afterName = cur.pos;
            std::string op = cur.op();
            double v = 0;
            if (op.empty()) {
                // "Effect 1 on", "Wah Auto": the value without an operator.
                if (controlValue(cur, control, v)) {
                    op = "=";
                } else {
                    cur.pos = afterName;
                    n.op = Node::Op::Truthy;
                    n.index = control;
                    return add(c, n);
                }
            } else if (!controlValue(cur, control, v)) {
                *error = "'" + trim(cur.s.substr(cur.pos)) + "' is not a value of " + controls[control].name;
                return -1;
            }
            n.op = Node::Op::Cmp;
            n.index = control;
            n.cmp = op;
            n.value = v;
            return add(c, n);
        }
        if (context >= 0) {
            // Inside a control: "Auto" means this control is Auto.
            std::string op = cur.op();
            double v = 0;
            if (controlValue(cur, context, v)) {
                n.op = Node::Op::Cmp;
                n.index = context;
                n.cmp = op.empty() ? "=" : op;
                n.value = v;
                return add(c, n);
            }
        }
        cur.pos = start;
        *error = "'" + trim(cur.s.substr(cur.pos)) + "' is not a control" + (context >= 0 ? " or a value of " + controls[context].name : "");
        return -1;
    }

    bool parseCondition(const std::string& text, Condition& c, int context, std::string* error) const {
        c = Condition{};
        if (trim(text).empty())
            return true;
        Cursor cur{text};
        c.root = parseOr(cur, c, context, error);
        if (c.root < 0)
            return false;
        if (!cur.end()) {
            *error = "did not understand '" + trim(text.substr(cur.pos)) + "'";
            return false;
        }
        return true;
    }

    bool eval(const Condition& c, int node) const {
        const Node& n = c.nodes[node];
        switch (n.op) {
        case Node::Op::True: return true;
        case Node::Op::Truthy: {
            const Control& k = controls[n.index];
            return std::fabs(k.value - k.min) > 1e-9;
        }
        case Node::Op::Cmp: {
            const Control& k = controls[n.index];
            const double eps = 1e-6 * std::max(1.0, k.max - k.min);
            const double v = k.value;
            if (n.cmp == "=") return std::fabs(v - n.value) <= eps;
            if (n.cmp == "!=") return std::fabs(v - n.value) > eps;
            if (n.cmp == "<") return v < n.value - eps;
            if (n.cmp == "<=") return v <= n.value + eps;
            if (n.cmp == ">") return v > n.value + eps;
            if (n.cmp == ">=") return v >= n.value - eps;
            return false;
        }
        case Node::Op::Not: return !eval(c, n.a);
        case Node::Op::And: return eval(c, n.a) && eval(c, n.b);
        case Node::Op::Or: return eval(c, n.a) || eval(c, n.b);
        case Node::Op::PresetIs: return preset == n.index;
        case Node::Op::SceneIs: return scene == n.index;
        }
        return false;
    }
    bool eval(const Condition& c) const { return c.always() || eval(c, c.root); }

    // ---------------------------------------------------------------- actions

    bool parseAction(const ConfigText& t, Action& a, std::string* error) const {
        a = Action{};
        a.text = t.text;
        a.where = t.where;
        const std::string text = trim(t.text);
        auto bad = [&](const std::string& why) {
            *error = why;
            return false;
        };

        // <control> = <value>
        const auto eq = text.find('=');
        if (eq != std::string::npos) {
            const int c = findControl(trim(text.substr(0, eq)));
            if (c >= 0) {
                a.index = c;
                const std::string v = lower(trim(text.substr(eq + 1)));
                const Control& k = controls[c];
                double n = 0;
                std::istringstream words(v);
                std::string w0, w1, extra;
                words >> w0 >> w1 >> extra;
                const bool up = w0 == "up" || w0 == "next", down = w0 == "down" || w0 == "previous";
                if ((up || down) && extra.empty() && (w1.empty() || (toNumber(w1, n) && n > 0))) {
                    // next, previous: by the control's step; up 0.1, down 2: by that much.
                    a.type = Action::Type::Step;
                    a.value = (w1.empty() ? k.step : n) * (up ? 1 : -1);
                } else if (v == "toggle") {
                    a.type = Action::Type::Toggle;
                } else if (v == "default") {
                    a.type = Action::Type::Default;
                } else {
                    Cursor cur{v};
                    double value = 0;
                    if (!controlValue(cur, c, value) || !cur.end())
                        return bad("'" + trim(text.substr(eq + 1)) + "' is not a value of " + k.name +
                                   (k.labels.empty() ? "" : " (its values: " + join(k.labels) + ")"));
                    if (value < k.min - 1e-9 || value > k.max + 1e-9)
                        return bad(format(value) + " is outside the range of " + k.name);
                    a.type = Action::Type::Set;
                    a.value = value;
                }
                return true;
            }
        }

        Cursor cur{text};
        auto rest = [&] { return trim(text.substr(cur.pos)); };
        if (cur.word("group")) {
            a.type = Action::Type::Group;
            a.index = findGroup(rest());
            return a.index >= 0 || bad("no group named '" + rest() + "'");
        }
        for (const char* what : {"preset", "chain", "scene"}) {
            if (!cur.word(what))
                continue;
            const bool isPreset = std::string(what) != "scene"; // a chain is selected by its preset
            const std::string r = lower(rest());
            double n = 0;
            if (r == "next" || r == "previous") {
                a.type = isPreset ? Action::Type::PresetStep : Action::Type::SceneStep;
                a.value = r == "next" ? 1 : -1;
                return true;
            }
            a.type = isPreset ? Action::Type::Preset : Action::Type::Scene;
            a.index = isPreset ? findPreset(rest()) : findScene(rest());
            const int count = static_cast<int>(isPreset ? presets.size() : scenes.size());
            if (a.index < 0 && toNumber(r, n) && n >= 1 && n <= count)
                a.index = static_cast<int>(n) - 1;
            return a.index >= 0 || bad(std::string("no ") + what + " named '" + rest() + "'");
        }
        for (auto [word, type] : {std::pair{"bypass", Action::Type::Bypass}, std::pair{"enable", Action::Type::Enable},
                                  std::pair{"toggle", Action::Type::TogglePlugin}}) {
            if (!cur.word(word))
                continue;
            a.type = type;
            return resolvePlugins(rest(), a.plugins, error);
        }
        if (cur.word("param")) {
            // param <plug-in> <id> = <value>
            const std::string r = rest();
            const auto e = r.find('=');
            if (e == std::string::npos)
                return bad("param is: param <plug-in> <id> = <value>");
            std::string left = trim(r.substr(0, e));
            const auto space = left.find_last_of(" \t");
            double id = 0, v = 0;
            if (space == std::string::npos || !toNumber(left.substr(space + 1), id) || id < 0 || !toNumber(trim(r.substr(e + 1)), v))
                return bad("param is: param <plug-in> <id> = <value>, like param Delay 3 = 0.5");
            a.type = Action::Type::Param;
            a.paramId = static_cast<uint32_t>(id);
            a.value = v;
            return resolvePlugins(trim(left.substr(0, space)), a.plugins, error);
        }
        if (cur.word("mute")) {
            const std::string r = lower(rest());
            a.type = Action::Type::Mute;
            a.value = r == "on" || r.empty() ? 1 : r == "off" ? 0 : r == "toggle" ? -1 : -2;
            return a.value > -2 || bad("mute is on, off or toggle");
        }
        if (cur.word("midi")) {
            std::istringstream ss(rest());
            for (std::string b; ss >> b;) {
                char* end = nullptr;
                const long v = std::strtol(b.c_str(), &end, 16);
                if (*end || v < 0 || v > 255)
                    return bad("midi takes hex bytes, like midi B0 6D 0F");
                a.bytes.push_back(static_cast<uint8_t>(v));
            }
            if (a.bytes.empty() || a.bytes.size() > 3 || a.bytes[0] < 0x80)
                return bad("midi takes one message of up to 3 hex bytes, starting with a status byte");
            a.type = Action::Type::Midi;
            return true;
        }
        return bad("did not understand '" + text +
                   "'. Actions: <control> = <value|next|previous|toggle|default>, group <name>, chain (or preset) <name|next|previous>, "
                   "scene <name|next|previous>, bypass/enable/toggle <plug-in>, param <plug-in> <id> = <value>, mute on|off|toggle, "
                   "midi <hex bytes>");
    }

    static std::string join(const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v)
            s += (s.empty() ? "" : ", ") + x;
        return s;
    }

    bool compileActions(const std::vector<ConfigText>& in, std::vector<Action>& outActions, const std::string& owner, std::string* error) const {
        for (const auto& t : in) {
            Action a;
            std::string err;
            if (!parseAction(t, a, &err)) {
                *error = t.where + ": " + owner + ": " + err;
                return false;
            }
            outActions.push_back(a);
        }
        return true;
    }

    // ---------------------------------------------------------------- running

    void setControl(int c, double v) {
        Control& k = controls[c];
        v = std::clamp(v, k.min, k.max);
        if (k.type != ControlSpec::Type::Continuous)
            v = k.min + std::round((v - k.min) / k.step) * k.step;
        k.value = std::min(v, k.max);
    }

    void stepControl(int c, double amount) {
        Control& k = controls[c];
        const double eps = 1e-9 * std::max(1.0, k.max - k.min);
        double v = k.value + amount;
        if (amount > 0 && v > k.max + eps) {
            // Reach the end first, then come back to the start.
            v = k.value < k.max - eps ? k.max : (k.wrap ? k.min : k.max);
        } else if (amount < 0 && v < k.min - eps) {
            v = k.value > k.min + eps ? k.min : (k.wrap ? k.max : k.min);
        }
        setControl(c, v);
    }

    void runActions(const std::vector<Action>& actions) {
        if (++runDepth > 8) {
            --runDepth;
            return; // a group that runs itself
        }
        for (const Action& a : actions)
            runAction(a);
        --runDepth;
    }

    void runAction(const Action& a) {
        switch (a.type) {
        case Action::Type::Set: setControl(a.index, a.value); break;
        case Action::Type::Step: stepControl(a.index, a.value); break;
        case Action::Type::Toggle: {
            const Control& k = controls[a.index];
            setControl(a.index, std::fabs(k.value - k.min) > 1e-9 ? k.min : k.max);
            break;
        }
        case Action::Type::Default: setControl(a.index, controls[a.index].def); break;
        case Action::Type::Group: runActions(groups[a.index].actions); break;
        case Action::Type::Preset: select(a.index); break;
        case Action::Type::PresetStep: select(wrapIndex(preset + static_cast<int>(a.value), static_cast<int>(presets.size()))); break;
        case Action::Type::Scene: select(scenes[a.index].firstPreset); break;
        case Action::Type::SceneStep: {
            const int s = wrapIndex(scene + static_cast<int>(a.value), static_cast<int>(scenes.size()));
            select(scenes[s].firstPreset);
            break;
        }
        case Action::Type::Bypass:
        case Action::Type::Enable:
        case Action::Type::TogglePlugin:
            for (int p : a.plugins) {
                const bool now = bypassed[p] == 1;
                const bool want = a.type == Action::Type::Bypass ? true : a.type == Action::Type::Enable ? false : !now;
                sendBypass(p, want);
            }
            break;
        case Action::Type::Param:
            for (int p : a.plugins)
                out.setParam(plugins[p].chain, plugins[p].slot, a.paramId, a.value);
            break;
        case Action::Type::Mute: manualMute = a.value < 0 ? !manualMute : a.value > 0; break;
        case Action::Type::Midi: {
            MidiMessage m;
            m.status = a.bytes[0];
            m.data1 = a.bytes.size() > 1 ? a.bytes[1] : 0;
            m.data2 = a.bytes.size() > 2 ? a.bytes[2] : 0;
            out.sendMidi(m);
            break;
        }
        }
    }

    static std::string text(const Control& k) {
        const long i = std::lround((k.value - k.min) / k.step);
        if (!k.labels.empty() && i >= 0 && i < static_cast<long>(k.labels.size()))
            return k.labels[i];
        return format(k.value);
    }

    static int wrapIndex(int i, int n) { return n <= 0 ? 0 : ((i % n) + n) % n; }

    void sendBypass(int p, bool want) {
        if (bypassed[p] == (want ? 1 : 0))
            return;
        bypassed[p] = want ? 1 : 0;
        out.setBypass(plugins[p].chain, plugins[p].slot, want);
    }

    void select(int index) {
        if (index < 0 || index >= static_cast<int>(presets.size()))
            return;
        const Preset& p = presets[index];
        preset = index;
        scene = p.scene;
        if (p.chain != activeChain) {
            activeChain = p.chain;
            out.switchTo(p.chain);
        }
        runActions(p.actions);
        int warm = p.warm;
        if (warm == kWarmNext) {
            // The next preset that plays another chain.
            warm = -1;
            for (size_t k = 1; k < presets.size(); ++k) {
                const int c = presets[(index + k) % presets.size()].chain;
                if (c != activeChain) {
                    warm = c;
                    break;
                }
            }
        }
        if (warm == activeChain)
            warm = -1;
        // Nothing is warm until a preset first asks for a chain.
        if (warm != (warmChain == -2 ? -1 : warmChain)) {
            warmChain = warm;
            out.setWarm(warm);
        }
    }

    // After anything changed: plug-ins on or off, parameters, mute, LEDs, display.
    void refresh() {
        std::vector<int> want(plugins.size(), -1);
        for (const Enable& e : enables) {
            const bool on = eval(e.when);
            for (int p : e.plugins)
                want[p] = want[p] == 0 ? 0 : (on ? 0 : 1);
        }
        for (size_t p = 0; p < plugins.size(); ++p)
            if (want[p] >= 0)
                sendBypass(static_cast<int>(p), want[p] == 1);

        for (Control& k : controls) {
            if (k.drivenValue == k.value)
                continue;
            k.drivenValue = k.value;
            const int c = static_cast<int>(&k - controls.data());
            for (const Drive& d : drives) {
                if (d.control != c)
                    continue;
                const double t = (k.value - k.min) / (k.max - k.min);
                const double v = d.from + t * (d.to - d.from);
                for (int p : d.plugins)
                    out.setParam(plugins[p].chain, plugins[p].slot, d.paramId, v);
            }
        }

        const bool mute = currentMute();
        if (sentMute != (mute ? 1 : 0)) {
            sentMute = mute ? 1 : 0;
            out.setMute(mute);
        }

        if (config.midi.profile != "eurekaprom")
            return;
        const uint8_t ch = config.midi.channel;
        // The pedals' LEDs always; the others only when a rule drives them.
        for (int led = 1; led <= eurekaprom::kLastLed; ++led) {
            if (led > 10 && !hasLedRule(led))
                continue;
            const int on = ledFor(led) ? 1 : 0;
            if (sentLeds[led] != on) {
                sentLeds[led] = on;
                out.sendMidi(eurekaprom::led(ch, led, on));
            }
        }
        const std::string text = displayFor();
        if (text != sentDisplay) {
            sentDisplay = text;
            double n = 0;
            if (text.size() > 1 && text[0] == '#' && toNumber(text.substr(1), n))
                out.sendMidi(eurekaprom::displayNumber(ch, static_cast<int>(n)));
            else
                for (const auto& m : eurekaprom::displayText(ch, text))
                    out.sendMidi(m);
        }
    }

    bool currentMute() const {
        if (manualMute)
            return true;
        for (const auto& m : mutes)
            if (eval(m))
                return true;
        return false;
    }

    bool hasLedRule(int led) const {
        for (const auto& rule : ledRules)
            if (rule.first == led)
                return true;
        return false;
    }

    bool ledFor(int pedal) const {
        for (const auto& [p, cond] : ledRules)
            if (p == pedal)
                return eval(cond);
        if (pedal > 10)
            return false;
        bool on = false;
        for (const Control& k : controls)
            if (k.pedal == pedal && std::fabs(k.value - k.min) > 1e-9)
                on = true;
        if (preset >= 0 && presets[preset].pedal == pedal)
            on = true;
        if (scene >= 0 && scenes[scene].pedal == pedal)
            on = true;
        return on;
    }

    // Text, or "#N" for a number.
    std::string displayFor() const {
        for (const auto& d : displays)
            if (eval(d.when))
                return d.text;
        if (preset < 0)
            return "";
        if (!presets[preset].display.empty())
            return presets[preset].display;
        if (!scenes[scene].display.empty())
            return scenes[scene].display;
        return "#" + std::to_string(preset + 1);
    }

    void handle(const Event& e) {
        // Every trigger sees the state from before any of them ran.
        std::vector<const Trigger*> fire;
        for (const Trigger& t : triggers) {
            if (t.scene >= 0 && t.scene != scene)
                continue;
            bool hit = false;
            for (const Input& in : t.inputs)
                hit = hit || matches(in, e);
            if (hit && eval(t.when))
                fire.push_back(&t);
        }
        for (const Trigger* t : fire)
            runActions(t->actions);
        for (size_t c = 0; c < controls.size(); ++c) {
            Control& k = controls[c];
            if (!k.hasFollow)
                continue;
            Input any = k.follow;
            any.value = -1;
            if (matches(any, e))
                setControl(static_cast<int>(c), k.min + (k.max - k.min) * e.value / 127.0);
        }
        refresh();
    }

    // ---------------------------------------------------------------- load

    bool load(const Config& cfg, const std::vector<ShowPlugin>& plugs, std::string* error) {
        config = cfg;
        plugins = plugs;
        warnings = cfg.warnings;
        bypassed.clear();
        for (const ShowPlugin& p : plugins)
            bypassed.push_back(p.bypassed ? 1 : 0);
        std::fill(std::begin(sentLeds), std::end(sentLeds), -1);

        // Chains and presets, in performance order.
        int chainBase = 0;
        for (size_t s = 0; s < cfg.scenes.size(); ++s) {
            const SceneSpec& ss = cfg.scenes[s];
            Scene sc;
            sc.name = ss.name;
            sc.display = ss.display;
            sc.pedal = ss.pedal;
            sc.program = ss.program;
            sc.firstPreset = static_cast<int>(presets.size());
            sc.numPresets = static_cast<int>(ss.presets.size());
            for (const auto& c : ss.chains)
                chainNames.push_back(c.name);
            for (const auto& ps : ss.presets) {
                Preset p;
                p.name = ps.name;
                p.display = ps.display;
                p.scene = static_cast<int>(s);
                p.pedal = ps.pedal;
                p.program = ps.program;
                const std::string warm = ps.warm.empty() ? cfg.warm : ps.warm;
                p.warm = same(warm, "none") ? kWarmNone : kWarmNext;
                for (size_t c = 0; c < ss.chains.size(); ++c) {
                    if (same(ss.chains[c].name, ps.chain))
                        p.chain = chainBase + static_cast<int>(c);
                    if (same(ss.chains[c].name, warm) && !same(warm, "next") && !same(warm, "none"))
                        p.warm = chainBase + static_cast<int>(c);
                }
                presets.push_back(p);
            }
            scenes.push_back(sc);
            chainBase += static_cast<int>(ss.chains.size());
        }

        for (const ControlSpec& cs : cfg.controls) {
            if (findControl(cs.name) >= 0) {
                *error = cs.where + ": a second control named " + cs.name;
                return false;
            }
            Control k{cs.name, cs.type, cs.min, cs.max, cs.step, cs.def, cs.def, cs.wrap, cs.labels, cs.pedal};
            if (!cs.follows.text.empty()) {
                std::string err;
                if (!parseInput(cs.follows.text, k.follow, &err)) {
                    *error = cs.follows.where + ": " + err;
                    return false;
                }
                k.hasFollow = true;
                k.followText = cs.follows.text;
            }
            controls.push_back(k);
        }
        for (const GroupSpec& gs : cfg.groups) {
            if (findGroup(gs.name) >= 0) {
                *error = gs.where + ": a second group named " + gs.name;
                return false;
            }
            groups.push_back({gs.name, {}});
        }

        // Actions can name any control, group, preset or scene, so they are
        // compiled once all names are known.
        for (size_t g = 0; g < cfg.groups.size(); ++g)
            if (!compileActions(cfg.groups[g].actions, groups[g].actions, "group " + groups[g].name, error))
                return false;
        {
            size_t k = 0;
            for (const SceneSpec& ss : cfg.scenes)
                for (const PresetSpec& ps : ss.presets)
                    if (!compileActions(ps.actions, presets[k++].actions, "preset " + ps.name, error))
                        return false;
        }

        for (size_t c = 0; c < cfg.controls.size(); ++c) {
            const ControlSpec& cs = cfg.controls[c];
            const int ci = static_cast<int>(c);
            for (const ConfigText& t : cs.enables) {
                Enable e;
                e.control = ci;
                std::string ref = t.text, cond;
                const size_t w = findWord(" " + t.text + " ", "when", true);
                if (w != std::string::npos) {
                    ref = trim(t.text.substr(0, w));
                    cond = trim(t.text.substr(std::min(t.text.size(), w + 4)));
                }
                std::string err;
                if (!resolvePlugins(ref, e.plugins, &err) ||
                    !parseCondition(cond.empty() ? cs.name : cond, e.when, ci, &err)) {
                    *error = t.where + ": control " + cs.name + ": enable: " + err;
                    return false;
                }
                enables.push_back(e);
            }
            for (const ConfigText& t : cs.drives) {
                // <plug-in> param <id> [from <a> to <b>]
                Drive d;
                d.control = ci;
                d.from = cs.min;
                d.to = cs.max;
                std::string err;
                const size_t p = findWord(" " + t.text + " ", "param", true);
                if (p == std::string::npos) {
                    *error = t.where + ": control " + cs.name + ": drive is: <plug-in> param <id> [from <a> to <b>]";
                    return false;
                }
                const std::string ref = trim(t.text.substr(0, p));
                std::istringstream ss(t.text.substr(std::min(t.text.size(), p + 5)));
                std::vector<std::string> w;
                for (std::string s; ss >> s;)
                    w.push_back(s);
                double id = 0, a = 0, b = 0;
                const bool ok = !w.empty() && toNumber(w[0], id) && id >= 0 &&
                                (w.size() == 1 || (w.size() == 5 && same(w[1], "from") && toNumber(w[2], a) && same(w[3], "to") && toNumber(w[4], b)));
                if (!ok) {
                    *error = t.where + ": control " + cs.name + ": drive is: <plug-in> param <id> [from <a> to <b>]";
                    return false;
                }
                d.paramId = static_cast<uint32_t>(id);
                if (w.size() == 5) {
                    d.from = a;
                    d.to = b;
                }
                if (!resolvePlugins(ref, d.plugins, &err)) {
                    *error = t.where + ": control " + cs.name + ": drive: " + err;
                    return false;
                }
                drives.push_back(d);
            }
            for (const ConfigText& t : cs.mutes) {
                Condition m;
                std::string err;
                if (!parseCondition(t.text, m, ci, &err)) {
                    *error = t.where + ": control " + cs.name + ": mute: " + err;
                    return false;
                }
                mutes.push_back(m);
            }
        }

        // Triggers: the ones written out, then the pedal and program shortcuts.
        for (const TriggerSpec& ts : cfg.triggers) {
            Trigger t;
            t.name = ts.name;
            if (!ts.scene.empty() && (t.scene = findScene(ts.scene)) < 0) {
                *error = ts.where + ": trigger " + ts.name + ": no scene named " + ts.scene;
                return false;
            }
            for (const ConfigText& in : ts.inputs) {
                Input i;
                std::string err;
                if (!parseInput(in.text, i, &err)) {
                    *error = in.where + ": trigger " + ts.name + ": " + err;
                    return false;
                }
                t.inputs.push_back(i);
            }
            if (t.inputs.empty()) {
                *error = ts.where + ": trigger " + ts.name + " has no on = line";
                return false;
            }
            std::string err;
            if (!parseCondition(ts.when.text, t.when, -1, &err)) {
                *error = ts.when.where + ": trigger " + ts.name + ": when: " + err;
                return false;
            }
            if (!compileActions(ts.actions, t.actions, "trigger " + ts.name, error))
                return false;
            triggers.push_back(t);
        }
        auto shortcut = [&](const std::string& name, Input in, int sceneScope, Action a) {
            Trigger t;
            t.name = name;
            t.inputs.push_back(in);
            t.scene = sceneScope;
            t.actions.push_back(a);
            triggers.push_back(t);
        };
        auto pedalInput = [](int pedal) {
            Input in;
            in.kind = Input::Kind::Pedal;
            in.number = pedal;
            return in;
        };
        auto programInput = [](int program) {
            Input in;
            in.kind = Input::Kind::Program;
            in.number = program;
            return in;
        };
        for (size_t c = 0; c < controls.size(); ++c)
            if (controls[c].pedal) {
                Action a;
                a.type = Action::Type::Step;
                a.index = static_cast<int>(c);
                a.value = controls[c].step;
                shortcut("control " + controls[c].name, pedalInput(controls[c].pedal), -1, a);
            }
        for (size_t p = 0; p < presets.size(); ++p) {
            Action a;
            a.type = Action::Type::Preset;
            a.index = static_cast<int>(p);
            if (presets[p].pedal)
                shortcut("preset " + presets[p].name, pedalInput(presets[p].pedal), presets[p].scene, a);
            if (presets[p].program >= 0)
                shortcut("preset " + presets[p].name, programInput(presets[p].program), -1, a);
        }
        for (size_t s = 0; s < scenes.size(); ++s) {
            Action a;
            a.type = Action::Type::Scene;
            a.index = static_cast<int>(s);
            if (scenes[s].pedal)
                shortcut("scene " + scenes[s].name, pedalInput(scenes[s].pedal), -1, a);
            if (scenes[s].program >= 0)
                shortcut("scene " + scenes[s].name, programInput(scenes[s].program), -1, a);
        }
        // Up and down step through the presets, unless something else uses them.
        for (int pedal : {eurekaprom::kUp, eurekaprom::kDown}) {
            bool used = false;
            for (const Trigger& t : triggers)
                for (const Input& in : t.inputs)
                    used = used || ((in.kind == Input::Kind::Pedal || in.kind == Input::Kind::PedalRelease) && in.number == pedal);
            if (used)
                continue;
            Action a;
            a.type = Action::Type::PresetStep;
            a.value = pedal == eurekaprom::kUp ? 1 : -1;
            shortcut(pedal == eurekaprom::kUp ? "next preset" : "previous preset", pedalInput(pedal), -1, a);
        }
        warnPedalClashes();

        for (const auto& [pedal, text] : cfg.fcb.leds) {
            Condition c;
            std::string err;
            if (!parseCondition(text.text, c, -1, &err)) {
                *error = text.where + ": led " + std::to_string(pedal) + ": " + err;
                return false;
            }
            ledRules.push_back({pedal, c});
        }
        for (const ConfigText& t : cfg.fcb.displays) {
            Display d;
            const size_t w = findWord(" " + t.text + " ", "when");
            d.text = trim(w == std::string::npos ? t.text : t.text.substr(0, w));
            std::string err;
            if (d.text.size() > 3 || !parseCondition(w == std::string::npos ? "" : t.text.substr(std::min(t.text.size(), w + 4)), d.when, -1, &err)) {
                *error = t.where + ": display: " + (err.empty() ? "the display shows up to 3 characters" : err);
                return false;
            }
            displays.push_back(d);
        }
        for (const Preset& p : presets)
            if (p.display.size() > 3)
                warnings.push_back("preset " + p.name + ": the display shows only " + p.display.substr(0, 2));
        return true;
    }

    void warnPedalClashes() {
        // Two triggers on the same pedal in the same scene both run; that is
        // allowed, but usually a mistake when one is a shortcut.
        for (size_t a = 0; a < triggers.size(); ++a)
            for (size_t b = a + 1; b < triggers.size(); ++b) {
                const Trigger &x = triggers[a], &y = triggers[b];
                if (x.scene >= 0 && y.scene >= 0 && x.scene != y.scene)
                    continue;
                for (const Input& i : x.inputs)
                    for (const Input& j : y.inputs)
                        if (i.kind == Input::Kind::Pedal && j.kind == i.kind && i.number == j.number)
                            warnings.push_back(describe(i) + " runs both " + x.name + " and " + y.name);
            }
    }
};

Show::Show(ShowOutput& out) : d_(std::make_unique<Impl>(out)) {}
Show::~Show() = default;

bool Show::load(const Config& config, const std::vector<ShowPlugin>& plugins, std::string* error) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->load(config, plugins, error);
}

std::vector<std::string> Show::warnings() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->warnings;
}

void Show::start() {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->select(0);
    d_->refresh();
}

void Show::onMidi(const MidiMessage& m) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    Impl& d = *d_;
    bool isPedal = false;
    Event e{Input::Kind::CC, m.data1, m.data2, m.channel()};
    if (d.config.midi.profile == "eurekaprom") {
        const auto in = eurekaprom::decode(m);
        if (in.kind == eurekaprom::Input::Kind::Press || in.kind == eurekaprom::Input::Kind::Release) {
            isPedal = true;
            Event p{in.kind == eurekaprom::Input::Kind::Press ? Input::Kind::Pedal : Input::Kind::PedalRelease, in.pedal, 127, m.channel()};
            d.handle(p);
        } else if (in.kind == eurekaprom::Input::Kind::Expression) {
            d.handle(Event{Input::Kind::Expression, in.expression, in.value, m.channel()});
        }
    }
    if (!isPedal) {
        switch (m.type()) {
        case 0xB0: d.handle(e); break;
        case 0xC0: d.handle(Event{Input::Kind::Program, m.data1, 127, m.channel()}); break;
        case 0x90:
            if (m.data2 > 0)
                d.handle(Event{Input::Kind::Note, m.data1, m.data2, m.channel()});
            break;
        default: break;
        }
    }
    // The pedals' own presses are switches, not something a plug-in should hear.
    if (d.config.midi.forward && !isPedal)
        for (const ShowPlugin& p : d.plugins)
            if (p.midi)
                d.out.forwardMidi(p.chain, p.slot, m);
}

bool Show::onKey(char key) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    Impl& d = *d_;
    bool used = false;
    for (const auto& t : d.triggers)
        for (const auto& in : t.inputs)
            used = used || (in.kind == Input::Kind::Key && in.number == static_cast<unsigned char>(key));
    if (used)
        d.handle(Event{Input::Kind::Key, static_cast<unsigned char>(key), 127, -1});
    return used;
}

void Show::selectPreset(int index) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->select(index);
    d_->refresh();
}

void Show::stepPreset(int delta) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->select(Impl::wrapIndex(d_->preset + delta, static_cast<int>(d_->presets.size())));
    d_->refresh();
}

bool Show::run(const std::string& action, std::string* error) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    Impl::Action a;
    if (!d_->parseAction({action, "command"}, a, error))
        return false;
    d_->runAction(a);
    d_->refresh();
    return true;
}

int Show::preset() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->preset;
}

int Show::scene() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->scene;
}

size_t Show::numPresets() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->presets.size();
}

std::string Show::presetName(int index) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return index >= 0 && index < static_cast<int>(d_->presets.size()) ? d_->presets[index].name : "";
}

double Show::value(const std::string& control) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    const int c = d_->findControl(control);
    return c < 0 ? std::numeric_limits<double>::quiet_NaN() : d_->controls[c].value;
}

std::string Show::valueText(const std::string& control) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    const int c = d_->findControl(control);
    return c < 0 ? "" : Impl::text(d_->controls[c]);
}

std::string Show::sceneName(int index) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return index >= 0 && index < static_cast<int>(d_->scenes.size()) ? d_->scenes[index].name : "";
}

std::vector<std::string> Show::controlNames() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::vector<std::string> out;
    for (const auto& c : d_->controls)
        out.push_back(c.name);
    return out;
}

bool Show::muted() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->currentMute();
}

bool Show::led(int pedal) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->ledFor(pedal);
}

std::string Show::display() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->displayFor();
}

std::string Show::status() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::string s = d_->preset >= 0 ? d_->presets[d_->preset].name : "-";
    if (d_->currentMute())
        s += " (muted)";
    for (const auto& k : d_->controls)
        s += "  " + k.name + ": " + Impl::text(k);
    return s;
}

std::vector<Show::ParamTie> Show::ties(int chain, size_t slot) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::vector<ParamTie> out;
    auto names = [&](const std::vector<int>& plugins) {
        for (int p : plugins)
            if (d_->plugins[p].chain == chain && d_->plugins[p].slot == slot)
                return true;
        return false;
    };
    for (const auto& d : d_->drives)
        if (names(d.plugins)) {
            const auto& k = d_->controls[d.control];
            out.push_back({d.paramId, k.name, k.followText, "", k.pedal});
        }
    for (const auto& t : d_->triggers)
        for (const auto& a : t.actions)
            if (a.type == Impl::Action::Type::Param && names(a.plugins))
                out.push_back({a.paramId, "", "", t.name, 0});
    return out;
}

} // namespace cue
