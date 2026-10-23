#pragma once

#include "config.h"
#include "core/engine.h"
#include "host/clap_processor.h"
#include "host/vst3_processor.h"

#include <memory>
#include <string>
#include <vector>

namespace sh {

// Everything loaded from a config: the engine with its scenes, plus the
// CLAP plug-ins that need main-thread servicing.
struct Rig {
    std::unique_ptr<Engine> engine;
    std::vector<ClapProcessor*> clapPlugins; // owned by the engine's scenes
    std::vector<Vst3Processor*> vst3Plugins;

    // Main thread, every few milliseconds: plug-in callbacks and controller updates.
    void pumpMainThread() {
        for (auto* p : clapPlugins)
            p->pumpMainThread();
        for (auto* p : vst3Plugins)
            p->pumpMainThread();
    }
};

std::unique_ptr<Processor> makeBuiltin(const PluginSpec& spec, std::string* error);

// Loads every plug-in of every scene and activates the engine. Prints what
// it loads. On failure returns false with the reason.
bool buildRig(const Config& config, Rig& rig, std::string* error);

} // namespace sh
