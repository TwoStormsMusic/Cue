#pragma once

#include "processor.h"
#include "ramp.h"

#include <memory>
#include <string>
#include <vector>

namespace sh {

// How a stereo chain signal becomes one mono output channel.
enum class OutputMix { Left, Right, Sum };

struct OutputTap {
    // Number of chain slots processed before this tap is taken: 0 = the dry
    // input, chain.size() = the end of the chain. "Out 2 before the IR loader"
    // is the IR loader's slot index.
    size_t afterSlots = 0;
    OutputMix mix = OutputMix::Sum;
};

// One scene: a chain of processors fed by one mono input, with taps to the
// engine's mono outputs. The chain is a straight line for the prototype; the
// routing grid generalises it later.
class Scene {
public:
    Scene(std::string name, std::vector<std::unique_ptr<Processor>> chain, std::vector<OutputTap> taps);

    const std::string& name() const { return name_; }
    size_t numOutputs() const { return taps_.size(); }
    size_t numSlots() const { return chain_.size(); }
    Processor& slot(size_t i) { return *chain_[i]; }

    // Main thread. Allocates every buffer the audio thread will need.
    bool activate(double sampleRate, uint32_t maxFrames);
    void deactivate();

    // Longest tail of any slot, kInfiniteTail if any slot reports one.
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
    // in: frames mono samples. outs: numOutputs() buffers, each overwritten.
    void process(const float* in, float* const* outs, uint32_t frames);

private:
    void writeTap(size_t tap, const float* const* chain, float* out, uint32_t frames) const;

    std::string name_;
    std::vector<std::unique_ptr<Processor>> chain_;
    std::vector<OutputTap> taps_;
    uint32_t maxFrames_ = 0;
    bool active_ = false;
    // Two stereo ping-pong buffers the chain is processed through.
    std::vector<float> storage_;
    std::vector<Ramp> slotInGain_; // per-slot input fades after resetTails()
    std::vector<float> gainScratch_;
    float* bufA_[kChainChannels] = {};
    float* bufB_[kChainChannels] = {};
};

} // namespace sh
