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

bool buildRig(const Config& config, Rig& rig, std::string* error) {
    EngineSettings es = config.engine;
    es.sampleRate = config.audio.rate;
    es.maxFrames = config.audio.period;
    es.numOutputs = 2;
    rig.engine = std::make_unique<Engine>(es);
    rig.clapPlugins.clear();

    struct PendingState {
        Processor* plugin;
        std::vector<uint8_t> data;
        std::string file;
        bool vst3;
    };
    std::vector<PendingState> states;
    auto readState = [&](const PluginSpec& ps, Processor* p, bool vst3) {
        std::ifstream f(ps.stateFile, std::ios::binary);
        if (!f) {
            *error = "cannot read state file " + ps.stateFile;
            return false;
        }
        states.push_back({p, std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}), ps.stateFile, vst3});
        return true;
    };

    for (const SceneSpec& ss : config.scenes) {
        std::vector<std::unique_ptr<Processor>> chain;
        std::printf("scene '%s'%s\n", ss.name.c_str(), ss.pedal ? (" (pedal " + std::to_string(ss.pedal) + ")").c_str() : "");
        for (const PluginSpec& ps : ss.chain) {
            const auto t0 = std::chrono::steady_clock::now();
            std::unique_ptr<Processor> p;
            std::string err;
            if (ps.kind == "builtin") {
                p = makeBuiltin(ps, &err);
            } else if (ps.kind == "vst3") {
                auto vst = Vst3Processor::load(ps.path, ps.id, &err);
                if (vst) {
                    for (auto [id, value] : ps.params)
                        vst->setParam(id, value);
                    if (!ps.stateFile.empty() && !readState(ps, vst.get(), true))
                        return false;
                    rig.vst3Plugins.push_back(vst.get());
                    p = std::move(vst);
                }
            } else {
                auto clap = ClapProcessor::load(ps.path, ps.id, &err);
                if (clap) {
                    for (auto [id, value] : ps.params)
                        clap->setParam(id, value);
                    if (!ps.stateFile.empty() && !readState(ps, clap.get(), false))
                        return false;
                    rig.clapPlugins.push_back(clap.get());
                    p = std::move(clap);
                }
            }
            if (!p) {
                *error = "scene '" + ss.name + "': " + ps.text + ": " + err;
                return false;
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  loaded %-40s %7.1f ms\n", p->name().c_str(), ms);
            chain.push_back(std::move(p));
        }
        const size_t n = chain.size();
        std::vector<OutputTap> taps{{0, std::min(ss.out1After, n), config.mix}, {0, std::min(ss.out2After, n), config.mix}};
        rig.engine->addChain(std::make_unique<Chain>(ss.name, std::move(chain), std::move(taps)));
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (!rig.engine->activate()) {
        *error = "a plug-in failed to activate";
        return false;
    }
    std::printf("activated %zu scenes in %.1f ms\n", rig.engine->numChains(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    // State loads after activation; CLAP allows it on the main thread.
    for (auto& s : states) {
        const bool ok = s.vst3 ? static_cast<Vst3Processor*>(s.plugin)->loadState(s.data, {})
                               : static_cast<ClapProcessor*>(s.plugin)->loadState(s.data);
        if (!ok)
            std::fprintf(stderr, "warning: %s did not accept state from %s\n", s.plugin->name().c_str(), s.file.c_str());
    }
    return true;
}

} // namespace cue
