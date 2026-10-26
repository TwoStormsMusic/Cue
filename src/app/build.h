#pragma once

#include "config.h"
#include "control/show.h"
#include "core/engine.h"
#include "host/clap_processor.h"
#include "host/vst3_processor.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cue {

// A loaded plug-in and where it sits in the engine.
struct RigPlugin {
    int chain = 0;
    size_t slot = 0;
    Processor* processor = nullptr; // owned by the engine's chain
    std::string label;              // `name =`, or the plug-in's own name
    bool midi = false;              // takes MIDI and the config lets it
    bool bypassed = false;          // as the config starts it
};

// Everything loaded from a config: the engine with its chains, plus the
// plug-ins that need main-thread servicing. Chains are numbered scene by
// scene, in config order, as the show expects.
struct Rig {
    std::unique_ptr<Engine> engine;
    std::vector<ClapProcessor*> clapPlugins;
    std::vector<Vst3Processor*> vst3Plugins;
    std::vector<RigPlugin> plugins;
    std::vector<std::string> chainNames;

    Processor* at(int chain, size_t slot) const;
    std::vector<ShowPlugin> showPlugins() const;

    // Main thread, every few milliseconds: plug-in callbacks and controller updates.
    void pumpMainThread() {
        for (auto* p : clapPlugins)
            p->pumpMainThread();
        for (auto* p : vst3Plugins)
            p->pumpMainThread();
    }
};

std::unique_ptr<Processor> makeBuiltin(const PluginSpec& spec, std::string* error);

// Loads every plug-in of every chain and activates the engine. Prints what
// it loads. On failure returns false with the reason.
bool buildRig(const Config& config, Rig& rig, std::string* error);

// The show's output for a live rig: engine commands, plug-in parameters and
// MIDI into plug-ins, and MIDI to the controller through midiOut.
class RigOutput : public ShowOutput {
public:
    RigOutput(Rig& rig, std::function<void(const MidiMessage&)> midiOut) : rig_(rig), midiOut_(std::move(midiOut)) {}
    void switchTo(int chain) override { rig_.engine->switchTo(chain); }
    void setWarm(int chain) override { rig_.engine->setWarm(chain); }
    void setBypass(int chain, size_t slot, bool bypassed) override { rig_.engine->setBypass(chain, slot, bypassed); }
    void setParam(int chain, size_t slot, uint32_t id, double value) override;
    void setMute(bool muted) override { rig_.engine->setMute(muted); }
    void sendMidi(const MidiMessage& m) override {
        if (midiOut_)
            midiOut_(m);
    }
    void forwardMidi(int chain, size_t slot, const MidiMessage& m) override;

private:
    Rig& rig_;
    std::function<void(const MidiMessage&)> midiOut_;
};

} // namespace cue
