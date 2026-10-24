#include "build.h"

#include "core/builtin.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace cue {

namespace {
float arg(const PluginSpec& s, size_t i, float def) {
    return i < s.args.size() ? std::strtof(s.args[i].c_str(), nullptr) : def;
}

bool readFile(const std::string& path, std::vector<uint8_t>& data, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = "cannot read state file " + path;
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
} // namespace

std::unique_ptr<Processor> makeBuiltin(const PluginSpec& spec, std::string* error) {
    const std::string& name = spec.args.at(0);
    if (name == "gain") return std::make_unique<GainProcessor>(arg(spec, 1, 1.0f));
    if (name == "drive") return std::make_unique<DriveProcessor>(arg(spec, 1, 3.0f));
    if (name == "lowpass") return std::make_unique<LowpassProcessor>(arg(spec, 1, 2000.0f));
    if (name == "delay") return std::make_unique<DelayProcessor>(arg(spec, 1, 400.0f), arg(spec, 2, 0.4f), arg(spec, 3, 0.4f));
    if (name == "burn") return std::make_unique<BurnProcessor>(static_cast<uint32_t>(arg(spec, 1, 500.0f)));
    *error = "unknown builtin '" + name + "' (gain, drive, lowpass, delay, burn)";
    return nullptr;
}

Processor* Rig::at(int chain, size_t slot) const {
    for (const auto& p : plugins)
        if (p.chain == chain && p.slot == slot)
            return p.processor;
    return nullptr;
}

std::vector<ShowPlugin> Rig::showPlugins() const {
    std::vector<ShowPlugin> out;
    for (const auto& p : plugins)
        out.push_back({p.chain, p.slot, p.label, p.midi, p.bypassed});
    return out;
}

void RigOutput::setParam(int chain, size_t slot, uint32_t id, double value) {
    if (Processor* p = rig_.at(chain, slot))
        p->setParam(id, value);
}

void RigOutput::forwardMidi(int chain, size_t slot, const MidiMessage& m) {
    if (Processor* p = rig_.at(chain, slot))
        p->sendMidi(m.status, m.data1, m.data2);
}

bool buildRig(const Config& config, Rig& rig, std::string* error) {
    EngineSettings es = config.engine;
    es.sampleRate = config.audio.rate;
    es.maxFrames = config.audio.period;
    es.numOutputs = 2;
    rig.engine = std::make_unique<Engine>(es);
    rig.clapPlugins.clear();
    rig.vst3Plugins.clear();
    rig.plugins.clear();
    rig.chainNames.clear();

    struct PendingState {
        Processor* plugin;
        std::vector<uint8_t> data, controller;
        std::string file;
        bool vst3;
    };
    std::vector<PendingState> states;

    for (const SceneSpec& scene : config.scenes) {
        std::printf("scene '%s'\n", scene.name.c_str());
        for (const ChainSpec& cs : scene.chains) {
            const int chainIndex = static_cast<int>(rig.chainNames.size());
            std::printf("  chain '%s'\n", cs.name.c_str());
            std::vector<Line> lines;
            std::vector<bool> initialBypass;
            size_t slot = 0;
            for (const LineSpec& ls : cs.lines) {
                Line line;
                line.parent = ls.parent;
                line.afterSlots = ls.after;
                if (ls.parent >= 0)
                    std::printf("    branch '%s' after plug-in %zu of %s\n", ls.name.c_str(), ls.after,
                                ls.parent == 0 ? "the main line" : ("branch '" + cs.lines[ls.parent].name + "'").c_str());
                for (const PluginSpec& ps : ls.plugins) {
                    const auto t0 = std::chrono::steady_clock::now();
                    std::unique_ptr<Processor> p;
                    std::string err;
                    if (ps.kind == "builtin") {
                        p = makeBuiltin(ps, &err);
                    } else if (ps.kind == "vst3") {
                        auto vst = Vst3Processor::load(ps.path, ps.id, &err);
                        if (vst) {
                            if (!ps.stateFile.empty()) {
                                PendingState st{vst.get(), {}, {}, ps.stateFile, true};
                                if (!readFile(ps.stateFile, st.data, error) ||
                                    (!ps.controllerStateFile.empty() && !readFile(ps.controllerStateFile, st.controller, error)))
                                    return false;
                                states.push_back(std::move(st));
                            }
                            rig.vst3Plugins.push_back(vst.get());
                            p = std::move(vst);
                        }
                    } else {
                        auto clap = ClapProcessor::load(ps.path, ps.id, &err);
                        if (clap) {
                            if (!ps.stateFile.empty()) {
                                PendingState st{clap.get(), {}, {}, ps.stateFile, false};
                                if (!readFile(ps.stateFile, st.data, error))
                                    return false;
                                states.push_back(std::move(st));
                            }
                            rig.clapPlugins.push_back(clap.get());
                            p = std::move(clap);
                        }
                    }
                    if (!p) {
                        *error = ps.where + ": " + ps.text + ": " + err;
                        return false;
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    RigPlugin rp;
                    rp.chain = chainIndex;
                    rp.slot = slot++;
                    rp.processor = p.get();
                    rp.label = ps.label.empty() ? p->name() : ps.label;
                    rp.midi = ps.midi && p->acceptsMidi();
                    rp.bypassed = ps.bypassed;
                    std::printf("    %-34s %7.1f ms%s%s%s\n", p->name().c_str(), ms, rp.label == p->name() ? "" : ("  as '" + rp.label + "'").c_str(),
                                ps.bypassed ? "  bypassed" : "", rp.midi ? "  takes MIDI" : "");
                    rig.plugins.push_back(rp);
                    initialBypass.push_back(ps.bypassed);
                    line.slots.push_back(std::move(p));
                }
                lines.push_back(std::move(line));
            }
            std::vector<OutputTap> taps;
            for (const TapSpec& t : cs.out)
                taps.push_back({t.line, t.after, config.mix});
            auto chain = std::make_unique<Chain>(cs.name, std::move(lines), std::move(taps));
            for (size_t i = 0; i < initialBypass.size(); ++i)
                if (initialBypass[i])
                    chain->setInitialBypass(i, true);
            rig.engine->addChain(std::move(chain));
            rig.chainNames.push_back(cs.name);
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (!rig.engine->activate()) {
        *error = "a plug-in failed to activate";
        return false;
    }
    std::printf("activated %zu chains in %.1f ms\n", rig.engine->numChains(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    // State loads after activation; CLAP allows it on the main thread. Then
    // the config's param lines, which win over the state.
    for (auto& s : states) {
        const bool ok = s.vst3 ? static_cast<Vst3Processor*>(s.plugin)->loadState(s.data, s.controller)
                               : static_cast<ClapProcessor*>(s.plugin)->loadState(s.data);
        if (!ok)
            std::fprintf(stderr, "warning: %s did not accept state from %s\n", s.plugin->name().c_str(), s.file.c_str());
    }
    size_t k = 0;
    for (const SceneSpec& scene : config.scenes)
        for (const ChainSpec& cs : scene.chains)
            for (const PluginSpec* ps : chainPlugins(cs)) {
                Processor* p = rig.plugins[k++].processor;
                for (auto [id, value] : ps->params)
                    if (!p->setParam(id, value))
                        std::fprintf(stderr, "warning: %s: %s did not take param %u\n", ps->where.c_str(), p->name().c_str(), id);
            }
    return true;
}

} // namespace cue
