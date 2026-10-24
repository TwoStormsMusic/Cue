#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace cue {

// Every processor in a scene chain is stereo in, stereo out. Plug-ins with
// other layouts are adapted by their wrapper.
constexpr int kChainChannels = 2;

constexpr uint32_t kInfiniteTail = std::numeric_limits<uint32_t>::max();

// One loaded, activated audio processor (a plug-in or a built-in block).
//
// Threading: activate/deactivate run on the main thread. startProcessing,
// stopProcessing, reset and process run on the audio thread and must not
// allocate, lock or block.
class Processor {
public:
    virtual ~Processor() = default;

    virtual const std::string& name() const = 0;

    virtual bool activate(double sampleRate, uint32_t maxFrames) = 0;
    virtual void deactivate() = 0;

    // Called when the engine starts or stops calling process() for a while.
    virtual void startProcessing() {}
    virtual void stopProcessing() {}

    // Clears internal state (delay lines, envelopes, filter memory).
    virtual void reset() = 0;

    // in and out each hold kChainChannels channel pointers. They never alias.
    virtual void process(const float* const* in, float* const* out, uint32_t frames) = 0;

    // Called on the audio thread, between processing, for processors in idle
    // scenes, so queued parameter changes still reach a plug-in that is not
    // being processed. Must be cheap when there is nothing to do.
    virtual void flushIdle() {}

    virtual uint32_t latencySamples() const { return 0; }
    // Length of the output tail after input stops; kInfiniteTail if unknown.
    virtual uint32_t tailSamples() const { return 0; }
};

} // namespace cue
