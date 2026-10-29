#pragma once

#include "processor.h"
#include "ramp.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cue {

// How a stereo chain signal becomes one mono output channel.
enum class OutputMix { Left, Right, Sum };

// One end of a wire: the chain's input, a block, or one of the chain's
// outputs. A wire leaves a block from its output and arrives at its input,
// or at its side-chain input (its key).
struct Port {
    enum class Kind : uint8_t { Input, Block, Output };
    Kind kind = Kind::Input;
    int index = 0;    // the block's slot, or the output's number
    bool key = false; // a block's side-chain input

    static Port input() { return {}; }
    static Port block(int slot, bool key = false) { return {Kind::Block, slot, key}; }
    static Port output(int o) { return {Kind::Output, o, false}; }
    bool operator==(const Port& o) const { return kind == o.kind && index == o.index && key == o.key; }
    bool operator!=(const Port& o) const { return !(*this == o); }
};

// At this level or below a wire passes nothing.
constexpr float kWireOffDb = -96.0f;

// What a wire does to the stereo signal it carries, in signal order: its
// level, its width (0 = mono, 1 = as it is, 2 = twice as wide), then a
// balance that turns one side down (-1 = the left only, 1 = the right only).
// A muted wire passes nothing and keeps its level for when it is not.
struct WireGains {
    float levelDb = 0.0f;
    float width = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    bool neutral() const { return levelDb == 0.0f && width == 1.0f && pan == 0.0f && !muted; }
};

// A feedback wire is heard one block late: what its plug-in played in the
// last block (1.3 ms at 64 frames, 48 kHz), so it may bring a plug-in's
// signal back to it. It is held to kFeedbackLimit, so it cannot run away.
constexpr float kFeedbackLimit = 2.0f; // +6 dBFS

struct Wire {
    Port from, to;
    WireGains gains;
    uint32_t id = 0; // given by the chain: the same wire keeps it through edits
    bool feedback = false;
};

// Processors in series, the way chains were first described. Line 0 is fed
// by the chain's input; any other line is a branch, taking a copy of an
// earlier line's signal at one point. A chain made of lines becomes blocks
// (line 0's first, then each branch's) and the wires between them.
struct Line {
    int parent = -1;       // the line this branch comes off; -1 for line 0
    size_t afterSlots = 0; // the branch starts after this many of the parent's slots
    std::vector<std::unique_ptr<Processor>> slots;
};

struct OutputTap {
    int line = 0;
    // Number of the line's slots processed before the tap is taken: 0 = the
    // line's input, SIZE_MAX = the end of the line.
    size_t afterSlots = SIZE_MAX;
    OutputMix mix = OutputMix::Sum;
};

// A change to a running chain's blocks and wires (see Engine::edit).
struct ChainEdit {
    struct Added {
        std::unique_ptr<Processor> processor; // activated, its state loaded
        bool bypassed = false;
        bool muteOnBypass = false;
    };
    // The chain's slots after the edit, in order: for each, the slot it is
    // now, or -1 for the next of added.
    std::vector<int> slots;
    std::vector<Added> added;
    std::vector<Wire> wires; // numbered as the slots are after the edit
};

// One chain: blocks (processors) joined by wires, fed by one mono input,
// feeding one mono signal to each engine output. Wires split and merge: a
// block's output feeds any number of wires, and the wires arriving at one
// input are summed. Blocks that no signal reaches, or whose signal reaches
// no output, are not processed. The engine switches between chains; only
// one is heard at a time, and idle chains are not processed.
//
// The chain can change while it plays. The main thread prepares a plan (the
// blocks in processing order and the wires between them) and the engine
// hands it to the audio thread, which swaps it in with a short fade.
class Chain {
public:
    struct Plan;
    struct PlanDeleter {
        void operator()(Plan* plan) const;
    };
    using PlanPtr = std::unique_ptr<Plan, PlanDeleter>;

    // An empty chain: no blocks and no wires, so it plays silence.
    Chain(std::string name, size_t numOutputs, OutputMix mix = OutputMix::Sum);
    Chain(std::string name, std::vector<Line> lines, std::vector<OutputTap> taps);
    // A chain that is a single line.
    Chain(std::string name, std::vector<std::unique_ptr<Processor>> slots, std::vector<OutputTap> taps);
    ~Chain();

    const std::string& name() const { return name_; }
    size_t numOutputs() const { return mix_.size(); }
    // Before activation: which of the engine's inputs feeds the chain.
    void setInput(size_t input) { input_ = input; }
    size_t input() const { return input_; }
    void setOutputMix(size_t output, OutputMix mix) { mix_.at(output) = mix; }

    // ---- Main thread: the chain as last built or edited ----
    size_t numSlots() const { return blocks_.size(); }
    Processor& slot(size_t i) { return *blocks_[i]->processor; }
    const std::vector<Wire>& wires() const { return wires_; }
    // Before activation. Returns the block's slot.
    size_t addBlock(std::unique_ptr<Processor> processor);
    // Before activation: adds a wire. False (with why) if an end does not
    // exist, the two ports are already wired, or it would make a loop (but
    // for a feedback wire, which is heard a block late).
    bool connect(Port from, Port to, WireGains gains = {}, std::string* error = nullptr, bool feedback = false);
    // Before activation: feeds slot's side-chain input with what source
    // passes on. False if that would make a loop.
    bool setSidechain(size_t slot, size_t source);
    // Whether wires fit blocks, of which those with keys[i] take a key:
    // every end exists, no two wires join the same ports, no loop other than
    // through a feedback wire.
    bool checkWires(const std::vector<Wire>& wires, const std::vector<bool>& keys, std::string* error) const;

    // Before activation: whether a slot starts bypassed.
    void setInitialBypass(size_t slot, bool bypassed);
    bool bypassed(size_t slot) const { return blocks_[slot]->bypassed; }
    // Before activation: what a bypassed slot passes on. Thru (the default)
    // passes its input on untouched; mute passes silence.
    void setMuteOnBypass(size_t slot, bool mute) { blocks_[slot]->muteOnBypass = mute; }
    bool muteOnBypass(size_t slot) const { return blocks_[slot]->muteOnBypass; }

    // Main thread. Allocates every buffer the audio thread will need.
    bool activate(double sampleRate, uint32_t maxFrames);
    void deactivate();
    bool active() const { return active_; }

    // Main thread, while active: the plan for an edit, or nullptr (with
    // why). Nothing changes until the plan is committed; added processors
    // belong to the plan from here on, and go with it if it is dropped.
    PlanPtr prepare(ChainEdit& edit, std::string* error);
    // Main thread: the blocks and wires above become the plan's, once the
    // audio thread has been given it.
    void commit(const Plan& plan);
    // Main thread: the processors a plan adds, to prime them.
    static std::vector<Processor*> added(const Plan& plan);
    // Main thread: a wire's gains, as the next plan will start them. Returns
    // the wire's id, or 0 if no wire joins the two ports.
    uint32_t setGains(Port from, Port to, WireGains gains);

    // Before activation: when a bypassed slot counts as rung out. After it
    // is bypassed it is fed silence until its output stays below quietLevel
    // for holdFrames, or for capFrames at most, and then no longer processed.
    void setDrain(float quietLevel, uint32_t holdFrames, uint32_t capFrames);

    // ---- Audio thread ----
    void startProcessing();
    void stopProcessing();
    // Longest tail of any block played and not bypassed, kInfiniteTail if one reports it.
    uint32_t tailSamples() const;
    // Clears every plug-in and the chain's own fades.
    void reset();
    // The two halves of reset(). Plug-in resets can be slow (a delay clears
    // seconds of buffer), so the engine runs them on a helper thread while
    // the chain is idle; resetRamps() is cheap and runs on the audio thread
    // when the chain starts.
    void resetProcessors();
    void resetRamps();
    // False while a bypassed slot is still ringing out, or did not ring out
    // and needs a reset before it is heard again.
    bool settled() const;
    // While held, slots with a tail of at least minTail samples (delays,
    // reverbs) are fed silence, so a warm chain's delay holds no echoes of
    // notes played before it is heard. Releasing fades their input in over
    // fadeFrames: a delay line that starts on an abrupt note repeats that
    // abrupt start as a click one delay time later.
    void holdTails(bool hold, uint32_t fadeFrames, uint32_t minTail);
    // Bypasses a slot, or brings it back, with a crossfade over fadeFrames
    // (0 = at once). A bypassed slot rings out on silence and is then no
    // longer processed; one coming back fades its input in, so a delay
    // starts without a click and with no stale echoes. A slot set to mute on
    // bypass fades to and from silence instead of its input. idle: the chain
    // is not being processed; only the slot's flags change. Slots are
    // numbered as in the last plan the chain was given.
    void setBypass(size_t slot, bool bypassed, uint32_t fadeFrames, bool idle = false);
    // Moves a wire's gains to new ones over fadeFrames.
    void setWireGains(uint32_t wire, WireGains gains, uint32_t fadeFrames);
    // For idle and resting chains: delivers queued parameter changes without processing.
    void flushIdle();
    // For chains being processed: the same, for the blocks they do not play.
    void flushUnplayed();
    // in: frames mono samples. outs: numOutputs() buffers, each overwritten.
    void process(const float* in, float* const* outs, uint32_t frames);
    // Any thread: the slot that took longest in one block since the last
    // call, and how long. False if nothing was processed.
    bool takeSlowest(size_t& slot, uint32_t& ns);

    // Edits. receive() takes a plan the engine was given; it is played once
    // swapped in. It returns a plan it replaces that was never played, to
    // be freed, or nullptr.
    Plan* receive(Plan* plan);
    bool hasPending() const { return pending_ != nullptr; }
    // While the chain plays: fades out the inputs of the blocks whose inputs
    // the pending plan changes, ahead of the swap.
    void prepareSwap(uint32_t fadeFrames);
    // Plays the pending plan from now on and returns the one it replaces,
    // to be freed. processing: the chain is being processed, so blocks it
    // starts playing start processing and those it stops playing stop.
    // Blocks that start or change inputs fade their input in. stale, for a
    // chain that is not processing: set if a block held sound and so needs
    // a reset.
    Plan* swap(bool processing, uint32_t fadeFrames, bool* stale = nullptr);

private:
    struct Block {
        std::unique_ptr<Processor> processor;
        Ramp inGain{1.0f}; // input fade after a reset, or around an edit
        Ramp wet{1.0f};    // 1 = processed, 0 = bypassed
        bool bypassed = false;
        bool muteOnBypass = false;
        bool hasKey = false;     // takes a side-chain input
        bool activated = false;  // by the chain
        bool draining = false;   // bypassed, still ringing out on silence
        bool needsReset = false; // bypassed without ringing out
        bool processing = false; // between startProcessing and stopProcessing
        bool stale = false;      // stopped playing while it may have held sound
        uint32_t quietFrames = 0, drainFrames = 0;
        uint32_t mark = 0;   // in the plan being swapped in
        uint64_t inputs = 0; // the wires it played from
        std::vector<float> storage;
        float* in[kChainChannels] = {};  // its inputs summed, when they need summing
        float* out[kChainChannels] = {};
        float* key[kChainChannels] = {}; // its side-chain input, when it has one
        const float* res[kChainChannels] = {}; // what it passes on in this block
        ~Block();
    };

    void allocate(Block& b);
    PlanPtr makePlan(std::vector<std::shared_ptr<Block>> blocks, std::vector<Wire> wires);
    const float* const* gather(Plan& plan, uint32_t first, uint32_t count, float* const* dst, uint32_t frames, bool into = false);
    void processBlock(Block& b, const float* const* input, uint32_t frames);
    void drain(Block& b, uint32_t frames);
    void fadeIn(Block& b, uint32_t fadeFrames);
    void writeTap(OutputMix mix, const float* const* signal, float* out, uint32_t frames) const;
    Plan* latest() const { return pending_ ? pending_ : plan_; }

    std::string name_;
    std::vector<OutputMix> mix_; // one per output
    size_t input_ = 0;

    // Main thread.
    std::vector<std::shared_ptr<Block>> blocks_;
    std::vector<Wire> wires_;
    uint32_t nextWire_ = 1;

    // Audio thread, once active.
    Plan* plan_ = nullptr;
    Plan* pending_ = nullptr;
    uint32_t mark_ = 0;
    double sampleRate_ = 0;
    uint32_t maxFrames_ = 0;
    bool active_ = false;
    std::vector<float> storage_;
    std::vector<float> gainScratch_, wetScratch_, rampScratch_;
    float* inBuf_[kChainChannels] = {};
    float* tmp_[kChainChannels] = {};
    float* mixBuf_[kChainChannels] = {};
    float* silence_[kChainChannels] = {};
    float quietLevel_ = 3.2e-5f; // -90 dB
    uint32_t drainHold_ = 14400, drainCap_ = 480000;
    bool holdTails_ = false;
    std::atomic<int> slowSlot_{-1};
    std::atomic<uint32_t> slowNs_{0};
    uint32_t tailMin_ = 0;
};

} // namespace cue
