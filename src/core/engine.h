#pragma once

#include "ramp.h"
#include "chain.h"
#include "spsc_queue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cue {

struct EngineSettings {
    double sampleRate = 48000.0;
    uint32_t maxFrames = 64;
    size_t numInputs = 1;
    size_t numOutputs = 2;

    float prerollMs = 50.0f;     // new chain runs on live input, muted, before it is heard
    float crossfadeMs = 20.0f;   // new chain fades in while the old chain's input fades out
    float tailQuietDb = -90.0f;  // a releasing chain below this level is considered silent
    float tailHoldMs = 300.0f;   // ...for this long before it stops
    float tailCapMs = 10000.0f;  // a releasing chain stops after this long whatever its level
    float stopFadeMs = 50.0f;    // fade applied when a releasing chain stops
    float bypassFadeMs = 10.0f;  // crossfade when a plug-in is bypassed or brought back
    float muteFadeMs = 10.0f;    // fade of the inputs when they are muted (the tuner)
    float editFadeMs = 10.0f;    // a playing chain's wiring changes between a fade out and a fade in
    float levelFadeMs = 20.0f;   // an input's or output's level (or mute) moves over this long
    // A warm chain's delays and reverbs would hear the old chain's playing.
    // They are fed silence until the switch, so no echoes of notes played
    // before it are heard.
    bool resetTailsOnWarmSwitch = true;
    float tailResetMinMs = 50.0f; // slots with at least this tail count as delays/reverbs
    // Helper threads that process playing chains alongside the audio thread,
    // so a switch (the old chain ringing out, the new one, the warm one)
    // takes about as long as its slowest chain rather than all of them in
    // turn. Chains that are not heard (warm, pre-roll) may also finish
    // while the next block starts, so a heavy or cold-starting one never
    // holds up the chain being played. 0 = every chain on the audio thread.
    int workers = 0;
    int workerPriority = 0; // their SCHED_FIFO priority; 0 = normal scheduling
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
    enum class Type : uint8_t { SwitchTo, SetWarm, SetBypass, SetMute, Edit, SetWire, SetBypassMode, SetInputLevel, SetOutputLevel, SetLimiter, Measure } type;
    int chain = -1;     // SwitchTo, SetWarm (-1 = no warm chain), SetBypass, SetBypassMode, Edit, SetWire
    uint32_t slot = 0;  // SetBypass, SetBypassMode; SetWire: the wire's id; SetInputLevel, SetOutputLevel, SetLimiter: which
    bool on = false;    // SetBypass: bypassed; SetMute, SetOutputLevel: muted; SetBypassMode: mutes when bypassed; SetLimiter, Measure: on
    WireGains gains;    // SetWire
    Chain::Plan* plan = nullptr; // Edit
    float value = 0.0f, value2 = 0.0f; // SetInputLevel, SetOutputLevel: dB; SetLimiter: ceiling dB, release ms
};

// About 10 ms of every input as it arrives and every output before its
// level, for Auto: sums, so a longer stretch is their sum.
struct LevelBin {
    static constexpr size_t kMax = 16; // inputs or outputs measured
    struct Channel {
        float peak = 0.0f;
        float energy = 0.0f;  // the sum of its samples squared
        float kEnergy = 0.0f; // ...K-weighted (BS.1770), for loudness
    };
    uint32_t frames = 0;
    Channel in[kMax], out[kMax];
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

// The slowest block over a stretch of time, with the chains that played in
// it: what an xrun can be traced to.
struct BlockReport {
    static constexpr size_t kMaxChains = 8;
    struct Entry {
        int chain = -1;
        ChainState state = ChainState::Idle;
        bool helper = false;     // processed on a helper thread
        bool background = false; // not heard (warm, pre-roll): did not hold up the block
        uint32_t ns = 0;         // for a background chain, its previous block
    };
    uint32_t ns = 0;    // the whole block, on the audio thread
    size_t count = 0;   // chains listed
    Entry chains[kMaxChains];
};

// Runs all chains for the audio thread. Only chains that are not Idle are
// processed, on the audio thread and helper threads at once. Switches
// follow: reset, pre-roll, crossfade, tail ring-out.
class Engine {
public:
    explicit Engine(EngineSettings settings);
    ~Engine();

    // Main thread, before audio starts. Takes ownership; returns the index.
    // A spare chain (empty) waits to be claimed by a chain added while the
    // engine plays: chains are never added or taken away once it runs.
    int addChain(std::unique_ptr<Chain> chain, bool spare = false);
    bool activate();
    void deactivate();
    // Main thread, after activate and before audio starts: runs every chain
    // on silence for a moment, on a helper thread, so plug-ins do their
    // first-block setup (an IR loader preparing its IR) now rather than
    // when the chain is first switched to.
    void prime(float milliseconds);

    // The chains up to the last one in use (spares after it are not counted).
    size_t numChains() const { return used_; }
    // Every chain, spares too.
    size_t capacity() const { return chains_.size(); }
    Chain& chain(size_t i) { return *chains_[i].chain; }
    // Main thread, once active: a spare chain for a new one, now in use and
    // fed by input: preferred if it is spare and idle, else the first one
    // never used, else any idle one. -1 if none is left. It plays silence
    // until an edit gives it blocks and wires.
    int claimChain(size_t input, int preferred = -1);
    // Main thread: a chain an edit has emptied, no longer played or warm,
    // spare again.
    void releaseChain(int chain);
    bool inUse(size_t i) const { return i < spare_.size() && !spare_[i]; }
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
    // What a slot passes on while bypassed: its input (thru) or silence
    // (mute). Heard, the change fades as a bypass does.
    bool setMuteOnBypass(int chain, size_t slot, bool mute) {
        return post({EngineCommand::Type::SetBypassMode, chain, static_cast<uint32_t>(slot), mute});
    }
    // Silences every input, with a short fade, so delays and reverbs ring
    // out. A chain that has gone quiet (tailQuietDb for tailHoldMs) then
    // rests: it is not processed until the inputs come back.
    bool setMute(bool muted) { return post({EngineCommand::Type::SetMute, -1, 0, muted}); }
    // The performance's levels, the same whichever chain plays. An input's
    // gain, before every chain (the Gain Stage's calibration), and an
    // output's level and mute, after every chain is mixed, then its
    // limiter. -100 dB or less is silence. They move over levelFadeMs;
    // before the first block, at once.
    bool setInputLevel(size_t input, float db) {
        EngineCommand c{EngineCommand::Type::SetInputLevel, -1, static_cast<uint32_t>(input)};
        c.value = db;
        return post(c);
    }
    bool setOutputLevel(size_t output, float db, bool muted) {
        EngineCommand c{EngineCommand::Type::SetOutputLevel, -1, static_cast<uint32_t>(output), muted};
        c.value = db;
        return post(c);
    }
    // No added latency: a peak over the ceiling is turned down at once, and
    // the gain comes back over about releaseMs.
    bool setLimiter(size_t output, bool on, float ceilingDb, float releaseMs) {
        EngineCommand c{EngineCommand::Type::SetLimiter, -1, static_cast<uint32_t>(output), on};
        c.value = ceilingDb;
        c.value2 = releaseMs;
        return post(c);
    }
    // Any thread: how far an output's limiter turned it down since last
    // asked, in dB (0 = not at all). One reader.
    float takeLimiterDb(size_t output);
    // While on, the audio thread hands over a LevelBin every 10 ms or so;
    // one reader takes them, often enough (the queue holds about 10 s).
    bool measure(bool on) { return post({EngineCommand::Type::Measure, -1, 0, on}); }
    bool nextLevelBin(LevelBin& bin);
    // Main thread, once active: changes a chain's blocks and wires while it
    // plays (see ChainEdit). Added processors are primed here, as prime()
    // does; the audio thread swaps the change in with a short fade. False,
    // with why, if it cannot be done (a loop, say), and then nothing changes.
    bool edit(int chain, ChainEdit edit, std::string* error);
    // The same in two steps, so the slow part can be done first.
    struct PreparedEdit {
        int chain = -1;
        Chain::PlanPtr plan;
    };
    bool prepareEdit(int chain, ChainEdit& edit, PreparedEdit& out, std::string* error);
    bool applyEdit(PreparedEdit& edit, std::string* error);
    // Main thread: a wire's gains, moved to over bypassFadeMs. False if no
    // wire joins the two ports.
    bool setWire(int chain, Port from, Port to, WireGains gains);
    // Main thread, often: frees what edits replaced.
    void collect();
    // One reader thread drains events.
    bool nextEvent(EngineEvent& ev);
    ChainStats& stats(size_t i) { return *chains_[i].stats; }
    // Any thread: asks for the slowest block since the last report, which
    // the audio thread hands over within a block; one reader takes it.
    void requestBlockReport() { reportWanted_.store(true, std::memory_order_relaxed); }
    bool nextBlockReport(BlockReport& report);
    size_t numWorkers() const { return workers_.size(); }
    // Helper threads that got real-time priority.
    int realtimeWorkers() const { return realtimeWorkers_.load(); }

    // Audio thread. ins: settings.numInputs mono buffers. outs:
    // settings.numOutputs buffers.
    void process(const float* const* ins, float* const* outs, uint32_t frames);
    // The same with one input (any others hear silence).
    void process(const float* in, float* const* outs, uint32_t frames);

    // Audio-thread view, also readable from tests between process() calls.
    int activeChain() const { return active_; }
    int pendingChain() const { return pending_; }
    int warmChain() const { return warm_; }
    bool muted() const { return muted_; }
    ChainState state(size_t i) const { return chains_[i].state; }
    bool resting(size_t i) const { return chains_[i].resting; }

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
        // While the inputs are muted: quiet for this long, and so not processed.
        uint64_t mutedQuietFrames = 0;
        bool resting = false;
        // A change to its wiring waits for its output to fade out.
        bool editing = false;
        Ramp editGain{1.0f};
        // Its own buffers, so chains can be processed on several threads at once.
        std::vector<float> buffers;
        float* in = nullptr;
        float* gain = nullptr;
        float* editScratch = nullptr;
        std::vector<float*> outs;
        bool ran = false;        // processed in this block
        bool background = false; // ...without being heard, so the block does not wait for it
        bool helper = false;     // ...on a helper thread
        uint32_t blockNs = 0;
        uint32_t backgroundNs = 0; // blockNs of its last background block, once finished
    };

    uint32_t ms(float milliseconds) const;
    void handle(const EngineCommand& cmd);
    void applyPlans();
    void retire(Chain::Plan* plan);
    void primeProcessors(const std::vector<Processor*>& processors, float milliseconds);
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
    void runChains(const float* const* ins, uint32_t frames, size_t heard, size_t background, bool flushIdle);
    void finishBackground();
    void takeJobs(uint32_t generation, uint32_t limit);
    void runChain(int index, bool helper);
    void recordStats(Slot& s);
    void restWhenQuiet(Slot& s, uint32_t frames, float peak);
    void workerLoop();
    void stopWorkers();
    void applyOutputLevels(float* const* outs, uint32_t frames);
    void measureBin(const float* const* ins, const float* const* outs, uint32_t frames);

    EngineSettings settings_;
    std::vector<Slot> chains_;
    bool activated_ = false;
    // Main thread: which chains are spare, which of them were never used,
    // and how many there are up to the last one in use.
    std::vector<bool> spare_, fresh_;
    size_t used_ = 0;

    int active_ = -1;
    int pending_ = -1;
    int warm_ = -1;
    int deferredSwitch_ = -1; // a switch waiting for its chain's reset
    bool warmWaiting_ = false; // the warm chain is waiting for its reset

    std::thread resetter_;
    std::atomic<bool> stopResetter_{false};

    // This block's chains: the heard ones, the one being played first, then
    // the background ones. Helpers claim them through ticket_:
    // generation << 32 | count << 16 | next. Background chains may still be
    // running when process() returns; the next block waits for them first.
    std::vector<int> jobs_;
    std::vector<float> jobIn_; // the inputs, one after another, kept for background chains
    std::vector<const float*> oneInput_; // process(in): in, then silence
    uint32_t jobFrames_ = 0;
    uint32_t generation_ = 0;
    size_t heardJobs_ = 0, backgroundJobs_ = 0;
    bool shared_ = false; // this block's chains were shared with the helpers
    std::vector<std::thread> workers_;
    std::atomic<bool> stopWorkers_{false};
    std::atomic<int> realtimeWorkers_{0};
    alignas(64) std::atomic<uint64_t> ticket_{0};
    alignas(64) std::atomic<uint32_t> heardDone_{0};
    alignas(64) std::atomic<uint32_t> backgroundDone_{0};
    alignas(64) std::atomic<uint32_t> wake_{0}; // a futex: bumped for each block helpers share
    std::atomic<uint32_t> sleepers_{0};

    BlockReport slowest_;
    std::atomic<bool> reportWanted_{false};
    CommandQueue<BlockReport, 8> reports_;

    CommandQueue<EngineCommand, 256> commands_;
    // Plans the audio thread is done with, for the main thread to free.
    CommandQueue<Chain::Plan*, 256> retired_;
    CommandQueue<EngineEvent, 1024> events_;

    // Scratch buffers, sized in activate().
    std::vector<float> gainScratch_, levelScratch_;
    uint32_t idleFlushCounter_ = 0;
    bool muted_ = false;
    Ramp inputGain_{1.0f};
    bool started_ = false; // the first block has been processed

    // The performance's levels: linear, an output's 0 while muted.
    std::vector<Ramp> inLevel_, outLevel_;
    struct Limiter {
        bool on = false;
        float ceiling = 1.0f;
        float release = 0.0f; // the gain's coefficient back towards 1, per sample
        float gain = 1.0f;
        float releaseMs = 100.0f;
    };
    std::vector<Limiter> limiters_;
    std::unique_ptr<std::atomic<float>[]> limiterLow_; // the lowest gain since last taken

    // Auto's measuring.
    bool measuring_ = false;
    LevelBin bin_;
    uint32_t binLength_ = 480;
    struct KWeight {
        double b[3], a[2];
    } kShelf_{}, kHighPass_{};
    std::vector<double> kState_; // per input then output: 2 states for each of the 2 filters
    RingQueue<LevelBin, 1024> bins_;
};

} // namespace cue
