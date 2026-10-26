#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cue {

// Every processor in a scene chain is stereo in, stereo out. Plug-ins with
// other layouts are adapted by their wrapper.
constexpr int kChainChannels = 2;

constexpr uint32_t kInfiniteTail = std::numeric_limits<uint32_t>::max();

// Marks the calling thread as one that processes audio (CLAP's audio
// thread), for plug-ins that check which thread calls them.
void markAudioThread();
bool isAudioThread();

struct ParamInfo {
    uint32_t id;
    std::string name, module;
    double min, max, def;
    int steps = 0; // number of steps for a stepped parameter, 0 = continuous
};

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

    // MIDI for plug-ins that take it (a wah moved by an expression pedal).
    // sendMidi may be called from any non-real-time thread; the message is
    // delivered with the next processed block. Controller and pitch-bend
    // values are kept as the latest value, so a plug-in in an idle chain gets
    // where the pedal is now when its chain starts, not every move since.
    virtual bool acceptsMidi() const { return false; }
    virtual void sendMidi(uint8_t /*status*/, uint8_t /*data1*/, uint8_t /*data2*/) {}

    // A second, side-chain input (a gate keyed by another signal). Main
    // thread, after activate: key holds kChainChannels buffers of at least
    // maxFrames that the chain fills before each process() call; nullptr =
    // silence. Without a key the side-chain input gets silence.
    virtual bool hasSidechain() const { return false; }
    virtual void setSidechain(const float* const* /*key*/) {}

    // Any non-real-time thread: queued, delivered with the next processed
    // block, or by flushIdle() in an idle chain. CLAP plug-ins take the
    // parameter's own value, VST3 plug-ins a normalised 0..1.
    virtual bool setParam(uint32_t /*id*/, double /*value*/) { return false; }
    // Main thread.
    virtual std::vector<ParamInfo> params() const { return {}; }
    // Main thread: a parameter's value now, as setParam takes it, and how the
    // plug-in writes a value ("-3.0 dB"). False or empty when it cannot say.
    virtual bool paramValue(uint32_t /*id*/, double& /*value*/) const { return false; }
    virtual std::string paramText(uint32_t /*id*/, double /*value*/) const { return {}; }

    virtual uint32_t latencySamples() const { return 0; }
    // Length of the output tail after input stops; kInfiniteTail if unknown.
    virtual uint32_t tailSamples() const { return 0; }
};

} // namespace cue
