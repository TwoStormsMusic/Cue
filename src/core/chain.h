#pragma once

#include "processor.h"
#include "ramp.h"

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
    size_t numLines() const { return lines_.size(); }
    size_t lineSize(size_t line) const { return lines_[line].count; }
    // Slots are numbered across all lines, line 0 first.
    size_t numSlots() const { return slots_.size(); }
    size_t slotIndex(size_t line, size_t index) const { return lines_[line].first + index; }
    Processor& slot(size_t i) { return *slots_[i].processor; }

    // Before activation: whether a slot starts bypassed.
    void setInitialBypass(size_t slot, bool bypassed);
    bool bypassed(size_t slot) const { return slots_[slot].bypassed; }

    // Main thread. Allocates every buffer the audio thread will need.
    bool activate(double sampleRate, uint32_t maxFrames);
    void deactivate();

    // Longest tail of any slot that is not bypassed, kInfiniteTail if one reports it.
    uint32_t tailSamples() const;
    uint32_t latencySamples() const;

    // Audio thread.
    void startProcessing();
    void stopProcessing();
    void reset();
    // Resets only the slots that report a tail of at least minTail samples
    // (delays, reverbs), leaving amp sims and filters settled. Their input is
    // faded in over fadeFrames: a delay line that starts on an abrupt note
    // repeats that abrupt start as a click one delay time later.
    void resetTails(uint32_t minTail, uint32_t fadeFrames);
    // Bypasses a slot, or brings it back, with a crossfade over fadeFrames
    // (0 = at once). A bypassed slot is not processed. One coming back is
    // reset first, since its state is from before it was bypassed.
    void setBypass(size_t slot, bool bypassed, uint32_t fadeFrames);
    // For idle chains: delivers queued parameter changes without processing.
    void flushIdle();
    // in: frames mono samples. outs: numOutputs() buffers, each overwritten.
    void process(const float* in, float* const* outs, uint32_t frames);

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
    };

    void processSlot(SlotState& s, float**& cur, float**& next, uint32_t frames);
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
};

} // namespace cue
