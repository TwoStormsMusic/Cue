#pragma once

#include "core/processor.h"
#include "core/spsc_queue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cue {

struct Vst3PluginInfo {
    std::string path, name, vendor, version, category, uid;
};

// Lists the audio effects inside one .vst3 bundle or file. yabridge's
// bridged Windows plug-ins appear here like any native VST3.
std::vector<Vst3PluginInfo> scanVst3(const std::string& path, std::string* error = nullptr);

// A VST3 plug-in hosted in this process, adapted to the stereo chain.
//
// VST3 has no cheap reset call. reset() toggles setProcessing(false/true),
// which the spec suggests plug-ins use to clear delay lines; not every
// plug-in does. The prototype measures this per plug-in.
class Vst3Processor : public Processor {
public:
    // which: the effect's name or class ID (32 hex digits, braces allowed);
    // empty = the first audio effect in the module. If nothing matches and
    // the module holds a single effect, that one is loaded.
    static std::unique_ptr<Vst3Processor> load(const std::string& path, const std::string& which, std::string* error);
    ~Vst3Processor() override;

    const std::string& name() const override { return name_; }

    // Main thread.
    bool activate(double sampleRate, uint32_t maxFrames) override;
    void deactivate() override;
    // REAPER-style state: the component state, then optionally the controller's.
    bool loadState(const std::vector<uint8_t>& component, const std::vector<uint8_t>& controller);
    bool saveState(std::vector<uint8_t>& component, std::vector<uint8_t>& controller) const;
    void pumpMainThread();

    // Any non-real-time thread. value is normalised 0..1, as VST3 parameters are.
    bool setParam(uint32_t id, double normalized) override;
    std::vector<ParamInfo> params() const override;

    // Audio thread.
    void startProcessing() override;
    void stopProcessing() override;
    void reset() override;
    void process(const float* const* in, float* const* out, uint32_t frames) override;

    uint32_t latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    uint32_t tailSamples() const override { return tail_.load(std::memory_order_relaxed); }

    struct Impl;

private:
    Vst3Processor();
    std::unique_ptr<Impl> impl_;
    std::string name_;
    std::atomic<uint32_t> latency_{0}, tail_{0};
};

} // namespace cue
