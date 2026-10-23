#include "build.h"

#include "core/builtin.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace sh {

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
        ClapProcessor* plugin;
        std::vector<uint8_t> data;
        std::string file;
    };
    std::vector<PendingState> states;

    for (const SceneSpec& ss : config.scenes) {
        std::vector<std::unique_ptr<Processor>> chain;
        std::printf("scene '%s'%s\n", ss.name.c_str(), ss.pedal ? (" (pedal " + std::to_string(ss.pedal) + ")").c_str() : "");
        for (const PluginSpec& ps : ss.chain) {
            const auto t0 = std::chrono::steady_clock::now();
            std::unique_ptr<Processor> p;
            std::string err;
            if (ps.kind == "builtin") {
                p = makeBuiltin(ps, &err);
            } else {
                auto clap = ClapProcessor::load(ps.path, ps.id, &err);
                if (clap) {
                    for (auto [id, value] : ps.params)
                        clap->setParam(id, value);
                    if (!ps.stateFile.empty()) {
                        std::ifstream f(ps.stateFile, std::ios::binary);
                        if (!f) {
                            *error = "cannot read state file " + ps.stateFile;
                            return false;
                        }
                        states.push_back({clap.get(), std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}), ps.stateFile});
                    }
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
        std::vector<OutputTap> taps{{std::min(ss.out1After, n), config.mix}, {std::min(ss.out2After, n), config.mix}};
        rig.engine->addScene(std::make_unique<Scene>(ss.name, std::move(chain), std::move(taps)));
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (!rig.engine->activate()) {
        *error = "a plug-in failed to activate";
        return false;
    }
    std::printf("activated %zu scenes in %.1f ms\n", rig.engine->numScenes(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

    // State loads after activation; CLAP allows it on the main thread.
    for (auto& s : states)
        if (!s.plugin->loadState(s.data))
            std::fprintf(stderr, "warning: %s did not accept state from %s\n", s.plugin->name().c_str(), s.file.c_str());
    return true;
}

} // namespace sh
