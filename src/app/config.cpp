#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// Expands a leading ~ and $HOME.
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

bool parseBool(const std::string& v) {
    const std::string l = lower(v);
    return l == "yes" || l == "true" || l == "on" || l == "1";
}

bool parsePlugin(const std::string& value, PluginSpec& spec, std::string* error) {
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
        spec.path = expandPath(trim(hash == std::string::npos ? rest : rest.substr(0, hash)));
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
    *error = "unknown plug-in kind '" + spec.kind + "'";
    return false;
}

} // namespace

bool loadConfig(const std::string& path, Config& config, std::string* error) {
    std::ifstream f(path);
    if (!f) {
        *error = "cannot open " + path;
        return false;
    }
    std::string section;
    SceneSpec* scene = nullptr;
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& what) {
        *error = path + ":" + std::to_string(lineNo) + ": " + what;
        return false;
    };

    while (std::getline(f, line)) {
        ++lineNo;
        const auto comment = line.find_first_of(";#");
        // A '#' inside a clap plug-in id is not a comment: only treat ';' and
        // a '#' at the start of the line as comments.
        if (comment != std::string::npos && (line[comment] == ';' || trim(line.substr(0, comment)).empty()))
            line = line.substr(0, comment);
        line = trim(line);
        if (line.empty())
            continue;

        if (line.front() == '[') {
            if (line.back() != ']')
                return fail("unclosed section header");
            const std::string header = trim(line.substr(1, line.size() - 2));
            const std::string lh = lower(header);
            if (lh.rfind("scene", 0) == 0) {
                section = "scene";
                config.scenes.emplace_back();
                scene = &config.scenes.back();
                scene->name = trim(header.substr(5));
                if (scene->name.empty())
                    scene->name = "Scene " + std::to_string(config.scenes.size());
            } else {
                section = lh;
                scene = nullptr;
            }
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            return fail("expected key = value");
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        auto num = [&](double& out) {
            char* end = nullptr;
            out = std::strtod(value.c_str(), &end);
            return end && *end == '\0';
        };
        double n = 0;

        if (section == "audio") {
            if (key == "device") config.audio.device = value;
            else if (key == "rate" && num(n)) config.audio.rate = static_cast<unsigned>(n);
            else if (key == "period" && num(n)) config.audio.period = static_cast<unsigned>(n);
            else if (key == "periods" && num(n)) config.audio.periods = static_cast<unsigned>(n);
            else if (key == "input" && num(n) && n >= 1) config.audio.inputChannel = static_cast<unsigned>(n) - 1;
            else if (key == "out1" && num(n) && n >= 1) config.audio.outputChannels.at(0) = static_cast<unsigned>(n) - 1;
            else if (key == "out2" && num(n) && n >= 1) config.audio.outputChannels.at(1) = static_cast<unsigned>(n) - 1;
            else if (key == "priority" && num(n)) config.audio.rtPriority = static_cast<int>(n);
            else if (key == "cpu") {
                if (lower(value) == "auto") config.cpuAuto = true;
                else if (lower(value) == "none") { config.cpuAuto = false; config.audio.cpu = -1; }
                else if (num(n)) { config.cpuAuto = false; config.audio.cpu = static_cast<int>(n); }
                else return fail("cpu must be auto, none or a CPU number");
            } else if (key == "mix") {
                const std::string v = lower(value);
                config.mix = v == "left" ? OutputMix::Left : v == "right" ? OutputMix::Right : OutputMix::Sum;
            } else return fail("unknown or invalid audio setting '" + key + "'");
        } else if (section == "switching") {
            if (key == "preroll_ms" && num(n)) config.engine.prerollMs = static_cast<float>(n);
            else if (key == "crossfade_ms" && num(n)) config.engine.crossfadeMs = static_cast<float>(n);
            else if (key == "tail_cap_ms" && num(n)) config.engine.tailCapMs = static_cast<float>(n);
            else if (key == "tail_hold_ms" && num(n)) config.engine.tailHoldMs = static_cast<float>(n);
            else if (key == "tail_quiet_db" && num(n)) config.engine.tailQuietDb = static_cast<float>(n);
            else if (key == "reset_tails_on_warm") config.engine.resetTailsOnWarmSwitch = parseBool(value);
            else if (key == "warm") {
                config.warm = lower(value);
                if (config.warm != "none" && config.warm != "next")
                    return fail("warm must be none or next");
            } else return fail("unknown or invalid switching setting '" + key + "'");
        } else if (section == "midi") {
            if (key == "enabled") config.midi.enabled = parseBool(value);
            else if (key == "in") config.midi.in = value;
            else if (key == "out") config.midi.out = value;
            else if (key == "profile") config.midi.profile = lower(value);
            else if (key == "channel" && num(n) && n >= 1 && n <= 16) config.midi.channel = static_cast<uint8_t>(n - 1);
            else return fail("unknown or invalid midi setting '" + key + "'");
        } else if (section == "scene" && scene) {
            if (key == "pedal" && num(n)) scene->pedal = static_cast<int>(n);
            else if (key == "program" && num(n)) scene->program = static_cast<int>(n);
            else if (key == "plugin") {
                PluginSpec spec;
                std::string err;
                if (!parsePlugin(value, spec, &err))
                    return fail(err);
                scene->chain.push_back(spec);
            } else if (key == "param") {
                if (scene->chain.empty())
                    return fail("param must follow a plugin line");
                const auto sep = value.find_first_of(":=");
                if (sep == std::string::npos)
                    return fail("param is id:value");
                scene->chain.back().params.push_back({static_cast<uint32_t>(std::strtoul(value.substr(0, sep).c_str(), nullptr, 10)),
                                                      std::strtod(value.substr(sep + 1).c_str(), nullptr)});
            } else if (key == "state") {
                if (scene->chain.empty())
                    return fail("state must follow a plugin line");
                scene->chain.back().stateFile = expandPath(value);
            } else if (key == "tap") {
                // The tap takes the signal at this point in the chain.
                const std::string v = lower(value);
                if (v == "out1") scene->out1After = scene->chain.size();
                else if (v == "out2") scene->out2After = scene->chain.size();
                else return fail("tap must be out1 or out2");
            } else return fail("unknown or invalid scene setting '" + key + "'");
        } else {
            return fail("setting outside a known section");
        }
    }
    if (config.scenes.empty()) {
        *error = path + ": no [scene ...] sections";
        return false;
    }
    if (config.midi.out.empty() && config.midi.in != "any")
        config.midi.out = config.midi.in;
    // Relative plug-in and state paths are relative to the config file.
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    for (auto& s : config.scenes)
        for (auto& p : s.chain) {
            if (!p.path.empty() && p.path[0] != '/')
                p.path = dir + "/" + p.path;
            if (!p.stateFile.empty() && p.stateFile[0] != '/')
                p.stateFile = dir + "/" + p.stateFile;
        }
    return true;
}

} // namespace cue
