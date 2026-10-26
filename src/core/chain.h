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

// Processors in series. Line 0 is fed by the chain's input. Any other line is
// a branch: it takes a copy of an earlier line's signal at one point, so
// "the IR loader only on the FOH output" is a branch holding the IR loader.
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

// One chain: lines of processors fed by one mono input, with one tap per
// engine output. The engine switches between chains; only one is heard at a
// time, and idle chains are not processed.
class Chain {
public:
    Chain(std::string name, std::vector<Line> lines, std::vector<OutputTap> taps);
    // A chain that is a single line.
    Chain(std::string name, std::vector<std::unique_ptr<Processor>> slots, std::vector<OutputTap> taps);

    const std::string& name() const { return name_; }
    size_t numOutputs() const { return taps_.size(); }
    // Before activation: which of the engine's inputs feeds the chain.
    void setInput(size_t input) { input_ = input; }
    size_t input() const { return input_; }
    size_t numLines() const { return lines_.size(); }
    size_t lineSize(size_t line) const { return lines_[line].count; }
    // Slots are numbered across all lines, line 0 first.
    size_t numSlots() const { return slots_.size(); }
    size_t slotIndex(size_t line, size_t index) const { return lines_[line].first + index; }
    Processor& slot(size_t i) { return *slots_[i].processor; }

    // Before activation: whether a slot starts bypassed.
    void setInitialBypass(size_t slot, bool bypassed);
    bool bypassed(size_t slot) const { return slots_[slot].bypassed; }
    // Before activation: what a bypassed slot passes on. Thru (the default)
    // passes its input on untouched; mute passes silence.
    void setMuteOnBypass(size_t slot, bool mute) { slots_[slot].muteOnBypass = mute; }
    bool muteOnBypass(size_t slot) const { return slots_[slot].muteOnBypass; }
    // Before activation: feeds slot's side-chain input with the signal
    // after source, an earlier slot (in processing order: line 0 first,
    // then each branch). False if source does not come first.
    bool setSidechain(size_t slot, size_t source);

    // Main thread. Allocates every buffer the audio thread will need.
    bool activate(double sampleRate, uint32_t maxFrames);
    void deactivate();

    // Longest tail of any slot that is not bypassed, kInfiniteTail if one reports it.
    uint32_t tailSamples() const;
    uint32_t latencySamples() const;

    // Before activation: when a bypassed slot counts as rung out. After it
    // is bypassed it is fed silence until its output stays below quietLevel
    // for holdFrames, or for capFrames at most, and then no longer processed.
    void setDrain(float quietLevel, uint32_t holdFrames, uint32_t capFrames);

    // Audio thread.
    void startProcessing();
    void stopProcessing();
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
    // is not being processed; only the slot's flags change.
    void setBypass(size_t slot, bool bypassed, uint32_t fadeFrames, bool idle = false);
    // For idle chains: delivers queued parameter changes without processing.
    void flushIdle();
    // in: frames mono samples. outs: numOutputs() buffers, each overwritten.
    void process(const float* in, float* const* outs, uint32_t frames);
    // Any thread: the slot that took longest in one block since the last
    // call, and how long. False if nothing was processed.
    bool takeSlowest(size_t& slot, uint32_t& ns);

private:
    struct LineState {
        int parent = -1;
        size_t afterSlots = 0;
        size_t first = 0, count = 0; // its slots in slots_
        float* a[kChainChannels] = {};
        float* b[kChainChannels] = {};
    };
    struct SlotState {
        std::unique_ptr<Processor> processor;
        Ramp inGain{1.0f}; // input fade after a reset
        Ramp wet{1.0f};    // 1 = processed, 0 = bypassed
        bool bypassed = false;
        bool muteOnBypass = false;
        int sidechainFrom = -1;                  // slot whose output keys this one
        float* keyOut[kChainChannels] = {};      // this slot's output, kept for a slot it keys
        bool draining = false;                   // bypassed, still ringing out on silence
        bool needsReset = false;                 // bypassed without ringing out
        uint32_t quietFrames = 0, drainFrames = 0;
    };

    void processSlot(SlotState& s, float**& cur, float**& next, uint32_t frames);
    void drain(SlotState& s, float** scratch, uint32_t frames);
    void writeTap(size_t tap, const float* const* signal, float* out, uint32_t frames) const;

    std::string name_;
    std::vector<LineState> lines_;
    std::vector<SlotState> slots_;
    std::vector<OutputTap> taps_;
    uint32_t maxFrames_ = 0;
    bool active_ = false;
    std::vector<float> storage_;
    std::vector<float> gainScratch_, wetScratch_;
    float* tmp_[kChainChannels] = {};
    float* silence_[kChainChannels] = {};
    float quietLevel_ = 3.2e-5f; // -90 dB
    uint32_t drainHold_ = 14400, drainCap_ = 480000;
    bool holdTails_ = false;
    std::atomic<int> slowSlot_{-1};
    std::atomic<uint32_t> slowNs_{0};
    uint32_t tailMin_ = 0;
    size_t input_ = 0;
};

} // namespace cue
