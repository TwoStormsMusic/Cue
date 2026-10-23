#pragma once

#include "ramp.h"
#include "scene.h"
#include "spsc_queue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace sh {

struct EngineSettings {
    double sampleRate = 48000.0;
    uint32_t maxFrames = 64;
    size_t numOutputs = 2;

    float prerollMs = 50.0f;     // new scene runs on live input, muted, before it is heard
    float crossfadeMs = 20.0f;   // new scene fades in while the old scene's input fades out
    float tailQuietDb = -90.0f;  // a releasing scene below this level is considered silent
    float tailHoldMs = 300.0f;   // ...for this long before it stops
    float tailCapMs = 10000.0f;  // a releasing scene stops after this long whatever its level
    float stopFadeMs = 50.0f;    // fade applied when a releasing scene stops
    // A warm scene's delays and reverbs have heard the old scene's playing.
    // Resetting them on switch avoids echoes of notes played before it.
    bool resetTailsOnWarmSwitch = true;
    float tailResetMinMs = 50.0f; // slots with at least this tail count as delays/reverbs
};

enum class SceneState : uint8_t {
    Idle,      // loaded, not processed: costs no CPU
    PreRoll,   // processing live input, muted, before the crossfade
    Warm,      // processing live input, muted, ready for an instant switch
    Active,    // the scene being played (including while it fades in)
    Releasing, // input fading or faded out, tails ringing out audibly
    Stopping,  // tail ended or capped: fading to silence before Idle
};

const char* toString(SceneState s);

struct EngineCommand {
    enum class Type : uint8_t { SwitchTo, SetWarm } type;
    int scene; // -1 for SetWarm means no warm scene
};

struct EngineEvent {
    enum class Type : uint8_t { SceneActive, SceneIdle, ScenePreRoll } type;
    int scene;
};

// Per-scene counters the audio thread publishes for the status display.
struct SceneStats {
    std::atomic<uint64_t> processNs{0};     // total time spent processing this scene
    std::atomic<uint64_t> processBlocks{0}; // number of blocks processed
    std::atomic<uint32_t> maxBlockNs{0};    // worst block since last reset by the reader
    std::atomic<uint8_t> state{0};
};

// Runs all scenes on the audio thread. Only scenes that are not Idle are
// processed. Switches follow: reset, pre-roll, crossfade, tail ring-out.
class Engine {
public:
    explicit Engine(EngineSettings settings);
    ~Engine();

    // Main thread, before audio starts. Takes ownership; returns the index.
    int addScene(std::unique_ptr<Scene> scene);
    bool activate();
    void deactivate();

    size_t numScenes() const { return scenes_.size(); }
    Scene& scene(size_t i) { return *scenes_[i].scene; }
    const EngineSettings& settings() const { return settings_; }

    // Any non-real-time thread.
    bool post(EngineCommand cmd) { return commands_.push(cmd); }
    bool switchTo(int scene) { return post({EngineCommand::Type::SwitchTo, scene}); }
    bool setWarm(int scene) { return post({EngineCommand::Type::SetWarm, scene}); }
    // One reader thread drains events.
    bool nextEvent(EngineEvent& ev);
    SceneStats& stats(size_t i) { return *scenes_[i].stats; }

    // Audio thread. in: one mono input. outs: settings.numOutputs buffers.
    void process(const float* in, float* const* outs, uint32_t frames);

    // Audio-thread view, also readable from tests between process() calls.
    int activeScene() const { return active_; }
    int pendingScene() const { return pending_; }
    int warmScene() const { return warm_; }
    SceneState state(size_t i) const { return scenes_[i].state; }

private:
    struct Slot {
        std::unique_ptr<Scene> scene;
        std::unique_ptr<SceneStats> stats;
        SceneState state = SceneState::Idle;
        Ramp inGain{0.0f};
        Ramp outGain{0.0f};
        int64_t prerollLeft = 0;
        uint64_t releaseFrames = 0;
        uint64_t quietFrames = 0;
    };

    uint32_t ms(float milliseconds) const;
    void handle(const EngineCommand& cmd);
    void requestSwitch(int target);
    void commit(int target);
    void setWarmTarget(int target);
    void startScene(Slot& s, SceneState state);
    void stopScene(int index);
    void setState(int index, SceneState state);
    void emit(EngineEvent::Type type, int scene);
    void advance(int index, uint32_t frames, float peak);

    EngineSettings settings_;
    std::vector<Slot> scenes_;
    bool activated_ = false;

    int active_ = -1;
    int pending_ = -1;
    int warm_ = -1;

    CommandQueue<EngineCommand, 256> commands_;
    CommandQueue<EngineEvent, 1024> events_;

    // Scratch buffers, sized in activate().
    std::vector<float> inScratch_;
    std::vector<float> gainScratch_;
    std::vector<float> sceneOutStorage_;
    std::vector<float*> sceneOuts_;
};

} // namespace sh
