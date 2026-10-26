#pragma once

// The show: what the pedals and MIDI do. It holds the performance (scenes in
// order, each with its presets), the named controls, triggers and groups from
// the config, and turns input into engine commands, plug-in parameter changes
// and FCB feedback. See docs/config.md for the language.

#include "app/config.h"
#include "midi/alsa_midi.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cue {

// A loaded plug-in as the show sees it.
struct ShowPlugin {
    int chain = 0;     // engine chain index: chains are numbered scene by scene, in config order
    size_t slot = 0;   // engine slot in the chain: line by line, in config order
    std::string label; // its `name =`, or the plug-in's own name
    bool midi = false; // takes MIDI, and the config lets it
    bool bypassed = false; // how the engine starts it
};

// Everything the show does to the outside world. The live implementation
// posts to the engine and the MIDI port; the tests record the calls.
class ShowOutput {
public:
    virtual ~ShowOutput() = default;
    virtual void switchTo(int chain) = 0;
    virtual void setWarm(int chain) = 0; // -1 = none
    virtual void setBypass(int chain, size_t slot, bool bypassed) = 0;
    virtual void setParam(int chain, size_t slot, uint32_t id, double value) = 0;
    virtual void setMute(bool muted) = 0;
    virtual void sendMidi(const MidiMessage& m) = 0;                           // to the controller
    virtual void forwardMidi(int chain, size_t slot, const MidiMessage& m) = 0; // into a plug-in
};

// Thread-safe: every call takes one lock. ShowOutput calls happen under it,
// so an output must not call back into the show.
class Show {
public:
    explicit Show(ShowOutput& out);
    ~Show();

    // Compiles the mappings. Errors and warnings name the config line.
    bool load(const Config& config, const std::vector<ShowPlugin>& plugins, std::string* error);
    // The same while it plays, after a change to the performance (a plug-in
    // added or removed): the preset, scene, controls and mute carry on as
    // they are. apply runs under the show's lock once the new performance
    // compiles, so nothing the show sends crosses the change; if it returns
    // false (with why) the show stays as it was. Then everything the
    // mappings say is sent again where it differs.
    bool reload(const Config& config, const std::vector<ShowPlugin>& plugins, std::string* error,
                const std::function<bool(std::string*)>& apply = nullptr);
    std::vector<std::string> warnings() const;

    // Selects the first preset and sets every LED and the display.
    void start();
    void onMidi(const MidiMessage& m);
    // A key from the keyboard of the machine. True if a trigger used it.
    bool onKey(char key);
    // Presets are numbered across the whole performance, from 0.
    void selectPreset(int index);
    void stepPreset(int delta);
    // One action, written as in a config: "Wah = next", "group Reset".
    bool run(const std::string& action, std::string* error);

    int preset() const;
    int scene() const;
    size_t numPresets() const;
    std::string presetName(int index) const;
    std::string sceneName(int index) const;
    std::vector<std::string> controlNames() const;
    // A control's value, or NaN if there is no such control.
    double value(const std::string& control) const;
    std::string valueText(const std::string& control) const;
    bool muted() const;
    bool led(int pedal) const;
    std::string display() const;
    // One line for the status display.
    std::string status() const;

    // What moves a plug-in's parameters from outside: a control's drive
    // (with what it follows and its pedal) or a trigger's param action.
    struct ParamTie {
        uint32_t param = 0;
        std::string control, follows, trigger;
        int pedal = 0;
    };
    std::vector<ParamTie> ties(int chain, size_t slot) const;

    struct Impl;

private:
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> d_;
};

} // namespace cue
