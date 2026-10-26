#pragma once

#include "core/processor.h"
#include "core/spsc_queue.h"

#include <clap/clap.h>

#include <atomic>
#include <chrono>
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
    std::vector<ParamInfo> params() const override;
    bool saveState(std::vector<uint8_t>& out) const;
    bool loadState(const std::vector<uint8_t>& data);
    // Handles callbacks the plug-in asked for (request_callback, rescans).
    void pumpMainThread();

    // Any non-real-time thread: queued, delivered on the audio thread by the
    // next process() call, or by flushIdle() if the scene is idle.
    bool setParam(uint32_t id, double value) override;

    // Audio thread.
    void startProcessing() override;
    void stopProcessing() override;
    void reset() override;
    void process(const float* const* in, float* const* out, uint32_t frames) override;
    void flushIdle() override;

    bool acceptsMidi() const override { return midiPort_ >= 0; }
    void sendMidi(uint8_t status, uint8_t data1, uint8_t data2) override;

    // The plug-in's second input port, if it has one, is its side-chain.
    bool hasSidechain() const override;
    void setSidechain(const float* const* key) override;

    uint32_t latencySamples() const override;
    uint32_t tailSamples() const override;

    // Host callbacks, public for the C trampolines.
    void onRequestRestart() { restartRequested_ = true; }
    void onRequestCallback() { callbackRequested_ = true; }
    void onParamsRescan() { rescanRequested_ = true; }
    void onLatencyChanged() { latencyDirty_ = true; }
    void onTailChanged() { tailDirty_ = true; }
    void onLog(int severity, const char* msg) const;
    bool onRegisterTimer(uint32_t periodMs, clap_id* id);
    bool onUnregisterTimer(clap_id id);

private:
    struct ParamChange {
        uint32_t id;
        double value;
    };
    struct MidiBytes {
        uint8_t data[3];
    };
    union Event {
        clap_event_header_t header;
        clap_event_param_value_t param;
        clap_event_midi_t midi;
    };
    struct InputEvents;
    // A timer the plug-in registered: pumpMainThread calls it when due.
    struct Timer {
        clap_id id;
        std::chrono::milliseconds period;
        std::chrono::steady_clock::time_point due;
    };

    ClapProcessor() = default;
    void drainParams(uint32_t maxEvents);
    void drainMidi();
    void pushMidiEvent(uint8_t s, uint8_t d1, uint8_t d2);
    void queryPorts();

    std::shared_ptr<ClapLibrary> library_;
    clap_host_t host_{};
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_audio_ports_t* ports_ = nullptr;
    const clap_plugin_params_t* paramsExt_ = nullptr;
    const clap_plugin_state_t* state_ = nullptr;
    const clap_plugin_latency_t* latency_ = nullptr;
    const clap_plugin_tail_t* tail_ = nullptr;
    const clap_plugin_timer_support_t* timerExt_ = nullptr;

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
    const float* key_[kChainChannels] = {}; // side-chain signal, or null

    CommandQueue<ParamChange, 512> paramQueue_;
    std::vector<Event> events_;
    std::unique_ptr<InputEvents> inEvents_;
    clap_output_events_t outEvents_{};
    std::atomic<bool> paramsPending_{false};

    // MIDI in: the latest value of every controller and pitch bend, with a
    // bit set for each one changed since the last block; other messages queued.
    int midiPort_ = -1;
    std::atomic<uint8_t> ccValue_[16 * 128] = {};
    std::atomic<uint64_t> ccDirty_[16 * 128 / 64] = {};
    std::atomic<uint16_t> bendValue_[16] = {};
    std::atomic<uint32_t> bendDirty_{0};
    CommandQueue<MidiBytes, 256> midiQueue_;
    std::atomic<bool> midiPending_{false};

    std::atomic<bool> restartRequested_{false}, callbackRequested_{false}, rescanRequested_{false};
    std::atomic<bool> latencyDirty_{false}, tailDirty_{false};
    std::atomic<uint32_t> cachedLatency_{0}, cachedTail_{0};

    std::vector<Timer> timers_; // main thread only
    clap_id nextTimerId_ = 1;
};

} // namespace cue
