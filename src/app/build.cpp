#include "build.h"

#include "core/builtin.h"

#include <algorithm>
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

// A state to load once its plug-in is active.
struct PendingState {
    Processor* plugin;
    std::vector<uint8_t> data, controller;
    std::string file;
    bool vst3;
};
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
    const int i = find(chain, slot);
    return i < 0 ? nullptr : plugins[i].processor;
}

int Rig::find(int chain, size_t slot) const {
    for (size_t i = 0; i < plugins.size(); ++i)
        if (plugins[i].chain == chain && plugins[i].slot == slot)
            return static_cast<int>(i);
    return -1;
}

void RigOutput::setBypass(int chain, size_t slot, bool bypassed) {
    rig_.engine->setBypass(chain, slot, bypassed);
    const int i = rig_.find(chain, slot);
    if (i >= 0 && rig_.bypassedNow)
        rig_.bypassedNow[i].store(bypassed, std::memory_order_relaxed);
}

std::vector<ShowPlugin> Rig::showPlugins() const {
    std::vector<ShowPlugin> out;
    for (size_t i = 0; i < plugins.size(); ++i) {
        const RigPlugin& p = plugins[i];
        out.push_back({p.chain, p.slot, p.label, p.midi, bypassedNow ? bypassedNow[i].load(std::memory_order_relaxed) : p.bypassed});
    }
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

namespace {

bool loadPlugin(const PluginSpec& ps, std::unique_ptr<Processor>& out, std::vector<PendingState>& states, std::string* error) {
    std::unique_ptr<Processor> p;
    std::string err;
    if (ps.kind == "builtin") {
        p = makeBuiltin(ps, &err);
    } else if (ps.kind == "vst3") {
        auto vst = Vst3Processor::load(ps.path, ps.id, &err);
        if (vst) {
            if (ps.embeddedState) {
                states.push_back({vst.get(), ps.state, ps.controllerState, ps.stateFile, true});
            } else if (!ps.stateFile.empty()) {
                PendingState st{vst.get(), {}, {}, ps.stateFile, true};
                if (!readFile(ps.stateFile, st.data, error) ||
                    (!ps.controllerStateFile.empty() && !readFile(ps.controllerStateFile, st.controller, error)))
                    return false;
                states.push_back(std::move(st));
            }
            p = std::move(vst);
        }
    } else {
        auto clap = ClapProcessor::load(ps.path, ps.id, &err);
        if (clap) {
            if (ps.embeddedState) {
                states.push_back({clap.get(), ps.state, {}, ps.stateFile, false});
            } else if (!ps.stateFile.empty()) {
                PendingState st{clap.get(), {}, {}, ps.stateFile, false};
                if (!readFile(ps.stateFile, st.data, error))
                    return false;
                states.push_back(std::move(st));
            }
            p = std::move(clap);
        }
    }
    if (!p) {
        *error = ps.where + ": " + ps.text + ": " + err;
        return false;
    }
    out = std::move(p);
    return true;
}

void loadStates(const std::vector<PendingState>& states) {
    for (const auto& s : states) {
        const bool ok = s.vst3 ? static_cast<Vst3Processor*>(s.plugin)->loadState(s.data, s.controller)
                               : static_cast<ClapProcessor*>(s.plugin)->loadState(s.data);
        if (!ok)
            std::fprintf(stderr, "warning: %s did not accept state from %s\n", s.plugin->name().c_str(), s.file.c_str());
    }
}

void setParams(const PluginSpec& ps, Processor& p) {
    for (auto [id, value] : ps.params)
        if (!p.setParam(id, value))
            std::fprintf(stderr, "warning: %s: %s did not take param %u\n", ps.where.c_str(), p.name().c_str(), id);
}

} // namespace

void Rig::track(Processor* p, const PluginSpec& spec) {
    if (spec.kind == "clap")
        clapPlugins.push_back(static_cast<ClapProcessor*>(p));
    else if (spec.kind == "vst3")
        vst3Plugins.push_back(static_cast<Vst3Processor*>(p));
}

void Rig::untrack(Processor* p) {
    clapPlugins.erase(std::remove(clapPlugins.begin(), clapPlugins.end(), static_cast<ClapProcessor*>(p)), clapPlugins.end());
    vst3Plugins.erase(std::remove(vst3Plugins.begin(), vst3Plugins.end(), static_cast<Vst3Processor*>(p)), vst3Plugins.end());
}

bool loadLivePlugin(const PluginSpec& spec, const EngineSettings& engine, std::unique_ptr<Processor>& out, std::string* error) {
    std::vector<PendingState> states;
    std::unique_ptr<Processor> p;
    if (!loadPlugin(spec, p, states, error))
        return false;
    if (!p->activate(engine.sampleRate, engine.maxFrames)) {
        *error = spec.where + ": " + p->name() + " failed to activate";
        return false;
    }
    loadStates(states);
    setParams(spec, *p);
    out = std::move(p);
    return true;
}

void captureState(Processor& p, PluginSpec& ps) {
    bool ok = true;
    if (ps.kind == "clap") {
        std::vector<uint8_t> state;
        ok = static_cast<ClapProcessor&>(p).saveState(state);
        if (ok) {
            ps.state = std::move(state);
            ps.controllerState.clear();
        }
    } else if (ps.kind == "vst3") {
        std::vector<uint8_t> state, controller;
        ok = static_cast<Vst3Processor&>(p).saveState(state, controller);
        if (ok) {
            ps.state = std::move(state);
            ps.controllerState = std::move(controller);
        }
    } else {
        return;
    }
    if (ok)
        ps.embeddedState = true;
    else
        std::fprintf(stderr, "warning: %s did not give its state; keeping the one it was loaded with\n", p.name().c_str());
    // A param line in the file wins over the state when the performance
    // loads, so the lines follow what the plug-in says now: a move made in
    // its own editor is kept rather than set back.
    for (auto& [id, value] : ps.params) {
        double now = value;
        if (p.paramValue(id, now))
            value = now;
    }
}

bool buildRig(Config& config, Rig& rig, std::string* error, size_t spareChains) {
    EngineSettings es = config.engine;
    es.sampleRate = config.audio.rate;
    es.maxFrames = config.audio.period;
    es.numInputs = config.inputs.size();
    es.numOutputs = config.outputs.size();
    es.workers = config.workers;
    es.workerPriority = config.audio.rtPriority;
    rig.engine = std::make_unique<Engine>(es);
    rig.clapPlugins.clear();
    rig.vst3Plugins.clear();
    rig.plugins.clear();
    rig.chainNames.clear();

    std::vector<PendingState> states;
    for (SceneSpec& scene : config.scenes) {
        std::printf("scene '%s'\n", scene.name.c_str());
        for (ChainSpec& cs : scene.chains) {
            const int chainIndex = static_cast<int>(rig.chainNames.size());
            cs.id = chainIndex;
            std::printf("  chain '%s'\n", cs.name.c_str());
            auto chain = std::make_unique<Chain>(cs.name, config.outputs.size(), config.mix);
            chain->setInput(cs.inputIndex);
            std::vector<std::string> labels;
            for (size_t slot = 0; slot < cs.blocks.size(); ++slot) {
                const PluginSpec& ps = cs.blocks[slot];
                const auto t0 = std::chrono::steady_clock::now();
                std::unique_ptr<Processor> p;
                if (!loadPlugin(ps, p, states, error))
                    return false;
                rig.track(p.get(), ps);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                RigPlugin rp;
                rp.chain = chainIndex;
                rp.slot = slot;
                rp.processor = p.get();
                rp.label = ps.label.empty() ? p->name() : ps.label;
                rp.midi = ps.midi && p->acceptsMidi();
                rp.bypassed = ps.bypassed;
                std::printf("    %-34s %7.1f ms%s%s%s%s%s\n", p->name().c_str(), ms, rp.label == p->name() ? "" : ("  as '" + rp.label + "'").c_str(),
                            ps.bypassed ? "  bypassed" : "", ps.muteOnBypass ? "  mutes when bypassed" : "", rp.midi ? "  takes MIDI" : "",
                            ps.sidechain.empty() ? "" : ("  side-chain from '" + ps.sidechain + "'").c_str());
                rig.plugins.push_back(rp);
                labels.push_back(rp.label);
                if (!ps.sidechain.empty() && !p->hasSidechain()) {
                    *error = ps.where + ": sidechain = " + ps.sidechain + ": " + p->name() + " has no side-chain input";
                    return false;
                }
                chain->addBlock(std::move(p));
                if (ps.bypassed)
                    chain->setInitialBypass(slot, true);
                chain->setMuteOnBypass(slot, ps.muteOnBypass);
            }
            // What `sidechain =` names becomes a wire into the plug-in's key.
            std::vector<std::pair<size_t, size_t>> feeds;
            if (!chainSidechains(cs, labels, feeds, error))
                return false;
            for (auto [slot, source] : feeds) {
                cs.wires.push_back({Port::block(static_cast<int>(source)), Port::block(static_cast<int>(slot), true), {}, cs.blocks[slot].where});
                cs.blocks[slot].sidechain.clear();
            }
            for (const WireSpec& w : cs.wires) {
                std::string why;
                if (!chain->connect(w.from, w.to, w.gains, &why, w.feedback)) {
                    *error = (w.where.empty() ? cs.where : w.where) + ": chain '" + cs.name + "': " + why;
                    return false;
                }
            }
            std::printf("    wires %s\n", wireList(cs, config.outputs).c_str());
            rig.engine->addChain(std::move(chain));
            rig.chainNames.push_back(cs.name);
        }
    }
    // Empty, for chains added while it plays.
    for (size_t i = 0; i < spareChains; ++i) {
        rig.engine->addChain(std::make_unique<Chain>("", config.outputs.size(), config.mix), true);
        rig.chainNames.emplace_back();
    }

    rig.bypassedNow.reset(new std::atomic<bool>[rig.plugins.size()]);
    for (size_t i = 0; i < rig.plugins.size(); ++i)
        rig.bypassedNow[i].store(rig.plugins[i].bypassed, std::memory_order_relaxed);

    postLevels(*rig.engine, config);
    const auto t0 = std::chrono::steady_clock::now();
    if (!rig.engine->activate()) {
        *error = "a plug-in failed to activate";
        return false;
    }
    std::printf("activated %zu chains in %.1f ms\n", rig.engine->numChains(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    // State loads after activation; CLAP allows it on the main thread. Then
    // the config's param lines, which win over the state.
    loadStates(states);
    size_t k = 0;
    for (const SceneSpec& scene : config.scenes)
        for (const ChainSpec& cs : scene.chains)
            for (const PluginSpec* ps : chainPlugins(cs))
                setParams(*ps, *rig.plugins[k++].processor);

    // First-block setup now, not on the first switch to a chain.
    const auto t1 = std::chrono::steady_clock::now();
    rig.engine->prime(100.0f);
    std::printf("primed every plug-in in %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
    return true;
}

void postInputLevel(Engine& engine, const Config& config, size_t input) {
    if (input < config.inputLevels.size())
        engine.setInputLevel(input, config.inputLevels[input].gainDb);
}

void postOutputLevel(Engine& engine, const Config& config, size_t output) {
    if (output >= config.outputLevels.size())
        return;
    const OutputLevel& l = config.outputLevels[output];
    engine.setOutputLevel(output, l.levelDb, l.muted);
    engine.setLimiter(output, l.limiter, l.ceilingDb, l.releaseMs);
}

void postLevels(Engine& engine, const Config& config) {
    for (size_t i = 0; i < config.inputLevels.size(); ++i)
        postInputLevel(engine, config, i);
    for (size_t o = 0; o < config.outputLevels.size(); ++o)
        postOutputLevel(engine, config, o);
}

} // namespace cue
