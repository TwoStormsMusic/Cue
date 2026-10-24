#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

typedef struct _snd_pcm snd_pcm_t;

namespace cue {

struct AlsaConfig {
    std::string device = "hw:M2";
    unsigned rate = 48000;
    unsigned period = 64;        // frames per period, fixed
    unsigned periods = 2;        // playback buffer = periods x period
    unsigned inputChannel = 0;   // capture channel carrying the instrument (0-based)
    std::vector<unsigned> outputChannels = {0, 1}; // playback channel for each engine output
    int rtPriority = 80;         // SCHED_FIFO priority of the audio thread; 0 = do not ask
    int cpu = -1;                // CPU to pin the audio thread to; -1 = leave unpinned
};

struct AlsaStats {
    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> xruns{0};
    std::atomic<uint32_t> maxCycleNs{0}; // worst processing time of one period (reset by reader)
    std::atomic<uint64_t> totalCycleNs{0};
    std::atomic<bool> realtime{false};   // got SCHED_FIFO
    std::atomic<bool> pinned{false};
    std::atomic<bool> failed{false};     // the device could not be recovered; audio stopped
};

// Full-duplex ALSA I/O on one device, driven by a real-time thread that reads
// a period, calls the engine, and writes a period. No extra buffering: the
// latency is one capture period plus the playback buffer.
class AlsaBackend {
public:
    // in: one mono channel. outs: one buffer per configured output channel.
    using Callback = std::function<void(const float* in, float* const* outs, uint32_t frames)>;

    ~AlsaBackend();
    bool open(const AlsaConfig& config, std::string* error);
    bool start(Callback callback, std::string* error);
    void stop();
    void close();

    const AlsaStats& stats() const { return stats_; }
    const AlsaConfig& config() const { return config_; }
    std::string describe() const; // negotiated format, channels, latency

private:
    struct Format;
    bool configure(snd_pcm_t* pcm, bool capture, unsigned& channels, int& format, std::string* error);
    void run();
    bool recover(int err);
    void prefill();

    AlsaConfig config_;
    snd_pcm_t* capture_ = nullptr;
    snd_pcm_t* playback_ = nullptr;
    unsigned captureChannels_ = 0, playbackChannels_ = 0;
    int captureFormat_ = 0, playbackFormat_ = 0;
    std::vector<uint8_t> captureRaw_, playbackRaw_;
    std::vector<float> in_;
    std::vector<std::vector<float>> outs_;
    std::vector<float*> outPtrs_;

    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    AlsaStats stats_;
};

// Sets flush-to-zero and denormals-are-zero for the calling thread, so
// decaying tails cannot fall into slow denormal arithmetic.
void enableFlushToZero();

} // namespace cue
