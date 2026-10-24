#pragma once

#include "ramp.h"
#include "chain.h"
#include "spsc_queue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace cue {

struct EngineSettings {
    double sampleRate = 48000.0;
    uint32_t maxFrames = 64;
    size_t numOutputs = 2;

    float prerollMs = 50.0f;     // new chain runs on live input, muted, before it is heard
    float crossfadeMs = 20.0f;   // new chain fades in while the old chain's input fades out
    float tailQuietDb = -90.0f;  // a releasing chain below this level is considered silent
    float tailHoldMs = 300.0f;   // ...for this long before it stops
    float tailCapMs = 10000.0f;  // a releasing chain stops after this long whatever its level
    float stopFadeMs = 50.0f;    // fade applied when a releasing chain stops
    float bypassFadeMs = 10.0f;  // crossfade when a plug-in is bypassed or brought back
    float muteFadeMs = 10.0f;    // fade of the outputs when they are muted (the tuner)
    // A warm chain's delays and reverbs would hear the old chain's playing.
    // They are fed silence until the switch, so no echoes of notes played
    // before it are heard.
    bool resetTailsOnWarmSwitch = true;
    float tailResetMinMs = 50.0f; // slots with at least this tail count as delays/reverbs
};

enum class ChainState : uint8_t {
    Idle,      // loaded, not processed: costs no CPU
    PreRoll,   // processing live input, muted, before the crossfade
    Warm,      // processing live input, muted, ready for an instant switch
    Active,    // the chain being played (including while it fades in)
    Releasing, // input fading or faded out, tails ringing out audibly
    Stopping,  // tail ended or capped: fading to silence before Idle
};

const char* toString(ChainState s);

struct EngineCommand {
    enum class Type : uint8_t { SwitchTo, SetWarm, SetBypass, SetMute } type;
    int chain = -1;     // SwitchTo, SetWarm (-1 = no warm chain), SetBypass
    uint32_t slot = 0;  // SetBypass
    bool on = false;    // SetBypass: bypassed; SetMute: muted
};

struct EngineEvent {
    enum class Type : uint8_t { ChainActive, ChainIdle, ChainPreRoll } type;
    int chain;
};

// Per-chain counters the audio thread publishes for the status display.
struct ChainStats {
    std::atomic<uint64_t> processNs{0};     // total time spent processing this chain
    std::atomic<uint64_t> processBlocks{0}; // number of blocks processed
    std::atomic<uint32_t> maxBlockNs{0};    // worst block since last reset by the reader
    std::atomic<uint8_t> state{0};
};

// Runs all chains on the audio thread. Only chains that are not Idle are
// processed. Switches follow: reset, pre-roll, crossfade, tail ring-out.
class Engine {
public:
    explicit Engine(EngineSettings settings);
    ~Engine();

    // Main thread, before audio starts. Takes ownership; returns the index.
    int addChain(std::unique_ptr<Chain> chain);
    bool activate();
    void deactivate();
    // Main thread, after activate and before audio starts: runs every chain
    // on silence for a moment, on a helper thread, so plug-ins do their
    // first-block setup (an IR loader preparing its IR) now rather than
    // when the chain is first switched to.
    void prime(float milliseconds);

    size_t numChains() const { return chains_.size(); }
    Chain& chain(size_t i) { return *chains_[i].chain; }
    const EngineSettings& settings() const { return settings_; }

    // Any non-real-time thread.
    bool post(EngineCommand cmd) { return commands_.push(cmd); }
    bool switchTo(int chain) { return post({EngineCommand::Type::SwitchTo, chain}); }
    bool setWarm(int chain) { return post({EngineCommand::Type::SetWarm, chain}); }
    // Bypassed slots are not processed. Changes on an idle chain apply at once,
    // on a playing chain with a short crossfade.
    bool setBypass(int chain, size_t slot, bool bypassed) {
        return post({EngineCommand::Type::SetBypass, chain, static_cast<uint32_t>(slot), bypassed});
    }
    // Silences every output, with a short fade. Chains keep playing.
    bool setMute(bool muted) { return post({EngineCommand::Type::SetMute, -1, 0, muted}); }
    // One reader thread drains events.
    bool nextEvent(EngineEvent& ev);
    ChainStats& stats(size_t i) { return *chains_[i].stats; }

    // Audio thread. in: one mono input. outs: settings.numOutputs buffers.
    void process(const float* in, float* const* outs, uint32_t frames);

    // Audio-thread view, also readable from tests between process() calls.
    int activeChain() const { return active_; }
    int pendingChain() const { return pending_; }
    int warmChain() const { return warm_; }
    bool muted() const { return muted_; }
    ChainState state(size_t i) const { return chains_[i].state; }

private:
    // An idle chain's plug-ins: rung out to silence or reset (clean), or
    // waiting for the reset helper (dirty, resetting). Only clean chains
    // start; one that is not waits, a few milliseconds at most.
    enum CleanState : uint8_t { kClean, kDirty, kResetting, kInUse };
    struct Slot {
        std::unique_ptr<Chain> chain;
        std::unique_ptr<ChainStats> stats;
        std::unique_ptr<std::atomic<uint8_t>> clean;
        ChainState state = ChainState::Idle;
        Ramp inGain{0.0f};
        Ramp outGain{0.0f};
        int64_t prerollLeft = 0;
        uint64_t releaseFrames = 0;
        uint64_t quietFrames = 0;
        bool quietEnd = false; // its tail died away rather than being capped
    };

    uint32_t ms(float milliseconds) const;
    void handle(const EngineCommand& cmd);
    void requestSwitch(int target);
    void commit(int target);
    void setWarmTarget(int target);
    void startChain(Slot& s, ChainState state);
    bool claimIdle(Slot& s);
    void stopChain(int index, bool rangOut);
    void resetLoop();
    void setState(int index, ChainState state);
    void emit(EngineEvent::Type type, int chain);
    void advance(int index, uint32_t frames, float peak);

    EngineSettings settings_;
    std::vector<Slot> chains_;
    bool activated_ = false;

    int active_ = -1;
    int pending_ = -1;
    int warm_ = -1;
    int deferredSwitch_ = -1; // a switch waiting for its chain's reset
    bool warmWaiting_ = false; // the warm chain is waiting for its reset

    std::thread resetter_;
    std::atomic<bool> stopResetter_{false};

    CommandQueue<EngineCommand, 256> commands_;
    CommandQueue<EngineEvent, 1024> events_;

    // Scratch buffers, sized in activate().
    std::vector<float> inScratch_;
    std::vector<float> gainScratch_;
    std::vector<float> chainOutStorage_;
    std::vector<float*> chainOuts_;
    uint32_t idleFlushCounter_ = 0;
    bool muted_ = false;
    Ramp outputGain_{1.0f};
};

} // namespace cue
