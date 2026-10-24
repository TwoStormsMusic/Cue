#pragma once

#include "core/processor.h"
#include "core/spsc_queue.h"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cue {

struct ClapPluginInfo {
    std::string path, id, name, vendor, version;
};

// Lists the plug-ins inside one .clap file.
std::vector<ClapPluginInfo> scanClapFile(const std::string& path, std::string* error = nullptr);

struct ParamInfo {
    uint32_t id;
    std::string name, module;
    double min, max, def;
};

// Marks the calling thread as the audio thread for CLAP's thread-check
// extension. The thread that first loads a plug-in is the main thread.
void markClapAudioThread();

class ClapLibrary;

// A CLAP plug-in hosted in this process, adapted to the stereo chain.
class ClapProcessor : public Processor {
public:
    // pluginId empty = the first plug-in in the file.
    static std::unique_ptr<ClapProcessor> load(const std::string& path, const std::string& pluginId, std::string* error);
    ~ClapProcessor() override;

    const std::string& name() const override { return name_; }
    const std::string& pluginId() const { return id_; }

    // Main thread.
    bool activate(double sampleRate, uint32_t maxFrames) override;
    void deactivate() override;
    std::vector<ParamInfo> params() const;
    bool saveState(std::vector<uint8_t>& out) const;
    bool loadState(const std::vector<uint8_t>& data);
    // Handles callbacks the plug-in asked for (request_callback, rescans).
    void pumpMainThread();

    // Any non-real-time thread: queued, delivered on the audio thread by the
    // next process() call, or by flushIdle() if the scene is idle.
    bool setParam(uint32_t id, double value);

    // Audio thread.
    void startProcessing() override;
    void stopProcessing() override;
    void reset() override;
    void process(const float* const* in, float* const* out, uint32_t frames) override;
    void flushIdle() override;

    uint32_t latencySamples() const override;
    uint32_t tailSamples() const override;

    // Host callbacks, public for the C trampolines.
    void onRequestRestart() { restartRequested_ = true; }
    void onRequestCallback() { callbackRequested_ = true; }
    void onParamsRescan() { rescanRequested_ = true; }
    void onLatencyChanged() { latencyDirty_ = true; }
    void onTailChanged() { tailDirty_ = true; }
    void onLog(int severity, const char* msg) const;

private:
    struct ParamChange {
        uint32_t id;
        double value;
    };
    struct InputEvents;

    ClapProcessor() = default;
    void drainParams(uint32_t maxEvents);
    void queryPorts();

    std::shared_ptr<ClapLibrary> library_;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_audio_ports_t* ports_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_tail_t* tail_ = nullptr;

    std::string name_, id_;
    bool activated_ = false;
    bool processing_ = false;
    int64_t steadyTime_ = 0;

    // Audio port layout: channel count of each port, main port first.
    std::vector<uint32_t> inPortChannels_, outPortChannels_;
    std::vector<clap_audio_buffer_t> inBuffers_, outBuffers_;
    std::vector<float*> inPtrs_, outPtrs_;
    std::vector<float> scratch_; // mono adaptation and spare ports
    uint32_t maxFrames_ = 0;

    CommandQueue<ParamChange, 512> paramQueue_;
    std::vector<clap_event_param_value_t> events_;
    std::unique_ptr<InputEvents> inEvents_;
    clap_output_events_t outEvents_{};
    std::atomic<bool> paramsPending_{false};

    std::atomic<bool> restartRequested_{false}, callbackRequested_{false}, rescanRequested_{false};
    std::atomic<bool> latencyDirty_{false}, tailDirty_{false};
    std::atomic<uint32_t> cachedLatency_{0}, cachedTail_{0};
};

} // namespace cue
