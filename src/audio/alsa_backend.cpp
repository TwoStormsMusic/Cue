#include "alsa_backend.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <xmmintrin.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cue {

void enableFlushToZero() {
    // FTZ (bit 15) and DAZ (bit 6) in MXCSR.
    _mm_setcsr(_mm_getcsr() | 0x8040);
}

namespace {

const snd_pcm_format_t kFormats[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S24_LE,
                                     SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S16_LE};

size_t bytesPerSample(int format) {
    switch (format) {
    case SND_PCM_FORMAT_S16_LE: return 2;
    case SND_PCM_FORMAT_S24_3LE: return 3;
    default: return 4;
    }
}

float readSample(const uint8_t* p, int format) {
    switch (format) {
    case SND_PCM_FORMAT_S32_LE: {
        int32_t v;
        std::memcpy(&v, p, 4);
        return static_cast<float>(v) * (1.0f / 2147483648.0f);
    }
    case SND_PCM_FORMAT_S24_LE: {
        int32_t v;
        std::memcpy(&v, p, 4);
        v = static_cast<int32_t>(static_cast<uint32_t>(v) << 8) >> 8;
        return static_cast<float>(v) * (1.0f / 8388608.0f);
    }
    case SND_PCM_FORMAT_S24_3LE: {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        v = static_cast<int32_t>(static_cast<uint32_t>(v) << 8) >> 8;
        return static_cast<float>(v) * (1.0f / 8388608.0f);
    }
    case SND_PCM_FORMAT_FLOAT_LE: {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    case SND_PCM_FORMAT_S16_LE: {
        int16_t v;
        std::memcpy(&v, p, 2);
        return static_cast<float>(v) * (1.0f / 32768.0f);
    }
    }
    return 0.0f;
}

void writeSample(uint8_t* p, int format, float x) {
    x = std::clamp(x, -1.0f, 1.0f);
    switch (format) {
    case SND_PCM_FORMAT_S32_LE: {
        const int32_t v = static_cast<int32_t>(std::lrint(static_cast<double>(x) * 2147483647.0));
        std::memcpy(p, &v, 4);
        break;
    }
    case SND_PCM_FORMAT_S24_LE: {
        const int32_t v = static_cast<int32_t>(std::lrint(x * 8388607.0f));
        std::memcpy(p, &v, 4);
        break;
    }
    case SND_PCM_FORMAT_S24_3LE: {
        const int32_t v = static_cast<int32_t>(std::lrint(x * 8388607.0f));
        p[0] = v & 0xff;
        p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff;
        break;
    }
    case SND_PCM_FORMAT_FLOAT_LE:
        std::memcpy(p, &x, 4);
        break;
    case SND_PCM_FORMAT_S16_LE: {
        const int16_t v = static_cast<int16_t>(std::lrint(x * 32767.0f));
        std::memcpy(p, &v, 2);
        break;
    }
    }
}

std::string alsaError(const char* what, int err) { return std::string(what) + ": " + snd_strerror(err); }

} // namespace

AlsaBackend::~AlsaBackend() { close(); }

bool AlsaBackend::configure(snd_pcm_t* pcm, bool capture, unsigned& channels, int& format, std::string* error) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    int err = snd_pcm_hw_params_any(pcm, hw);
    if (err < 0) {
        *error = alsaError("hw_params_any", err);
        return false;
    }
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        *error = alsaError("interleaved access", err);
        return false;
    }
    format = SND_PCM_FORMAT_UNKNOWN;
    for (snd_pcm_format_t f : kFormats) {
        if (snd_pcm_hw_params_test_format(pcm, hw, f) == 0) {
            snd_pcm_hw_params_set_format(pcm, hw, f);
            format = f;
            break;
        }
    }
    if (format == SND_PCM_FORMAT_UNKNOWN) {
        *error = "no supported sample format";
        return false;
    }

    unsigned needed = 1;
    if (capture)
        needed = config_.inputChannel + 1;
    else
        for (unsigned c : config_.outputChannels)
            needed = std::max(needed, c + 1);
    unsigned minCh = 0, maxCh = 0;
    snd_pcm_hw_params_get_channels_min(hw, &minCh);
    snd_pcm_hw_params_get_channels_max(hw, &maxCh);
    channels = std::clamp(needed, minCh, maxCh);
    if (channels < needed || (err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0) {
        *error = std::string(capture ? "capture" : "playback") + " needs " + std::to_string(needed) +
                 " channels, device has " + std::to_string(minCh) + "-" + std::to_string(maxCh);
        return false;
    }

    if ((err = snd_pcm_hw_params_set_rate(pcm, hw, config_.rate, 0)) < 0) {
        *error = alsaError(("rate " + std::to_string(config_.rate)).c_str(), err);
        return false;
    }
    snd_pcm_uframes_t period = config_.period;
    if ((err = snd_pcm_hw_params_set_period_size(pcm, hw, period, 0)) < 0) {
        *error = alsaError(("period size " + std::to_string(config_.period)).c_str(), err);
        return false;
    }
    // Capture gets spare room against overruns: it is read as soon as a
    // period is ready, so a larger buffer adds no latency.
    snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(config_.period) * (capture ? std::max(4u, config_.periods) : config_.periods);
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0) {
        *error = alsaError("buffer size", err);
        return false;
    }
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        *error = alsaError("hw_params", err);
        return false;
    }
    if (!capture) {
        snd_pcm_uframes_t size = 0;
        snd_pcm_hw_params_get_buffer_size(hw, &size);
        playbackBuffer_ = size;
    }

    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_uframes_t boundary = 0;
    snd_pcm_sw_params_get_boundary(sw, &boundary);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary); // started explicitly
    snd_pcm_sw_params_set_avail_min(pcm, sw, config_.period);
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
        *error = alsaError("sw_params", err);
        return false;
    }
    return true;
}

bool AlsaBackend::open(const AlsaConfig& config, std::string* error) {
    close();
    config_ = config;
    int err = snd_pcm_open(&capture_, config_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        *error = alsaError(("open capture " + config_.device).c_str(), err);
        capture_ = nullptr;
        return false;
    }
    err = snd_pcm_open(&playback_, config_.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        *error = alsaError(("open playback " + config_.device).c_str(), err);
        playback_ = nullptr;
        close();
        return false;
    }
    if (!configure(capture_, true, captureChannels_, captureFormat_, error) ||
        !configure(playback_, false, playbackChannels_, playbackFormat_, error)) {
        close();
        return false;
    }
    // Linked streams start and stop together.
    snd_pcm_link(capture_, playback_);

    captureRaw_.assign(static_cast<size_t>(config_.period) * captureChannels_ * bytesPerSample(captureFormat_), 0);
    playbackRaw_.assign(static_cast<size_t>(config_.period) * playbackChannels_ * bytesPerSample(playbackFormat_), 0);
    in_.assign(config_.period, 0.0f);
    outs_.assign(config_.outputChannels.size(), std::vector<float>(config_.period, 0.0f));
    outPtrs_.clear();
    for (auto& o : outs_)
        outPtrs_.push_back(o.data());
    return true;
}

std::string AlsaBackend::describe() const {
    if (!capture_)
        return "closed";
    char buf[256];
    const double latencyMs = 1000.0 * config_.period * (1 + config_.periods) / config_.rate;
    std::snprintf(buf, sizeof buf, "%s, %u Hz, %u frames x %u, capture %u ch %s, playback %u ch %s, about %.2f ms in to out plus converters",
                  config_.device.c_str(), config_.rate, config_.period, config_.periods, captureChannels_,
                  snd_pcm_format_name(static_cast<snd_pcm_format_t>(captureFormat_)), playbackChannels_,
                  snd_pcm_format_name(static_cast<snd_pcm_format_t>(playbackFormat_)), latencyMs);
    return buf;
}

void AlsaBackend::prefill() {
    std::fill(playbackRaw_.begin(), playbackRaw_.end(), 0);
    for (unsigned i = 0; i < config_.periods; ++i)
        snd_pcm_writei(playback_, playbackRaw_.data(), config_.period);
}

bool AlsaBackend::recover(int err, bool playback) {
    const uint64_t cycle = stats_.cycles.load();
    if (cycle < config_.rate / 4 / std::max(1u, config_.period))
        stats_.startupXruns++;
    else
        stats_.xruns++;
    stats_.lastXrunPlayback = playback;
    stats_.lastXrunCycle = cycle;
    stats_.lastXrunMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count());
    stats_.lastXrunBlockNs = lastBlockNs_;
    stats_.lastXrunLateFrames = lastLateFrames_;
    snd_pcm_drop(capture_);
    if (snd_pcm_prepare(capture_) < 0)
        return false;
    snd_pcm_prepare(playback_); // linked: often prepared already
    prefill();
    return snd_pcm_start(capture_) >= 0;
}

bool AlsaBackend::start(Callback callback, std::string* error) {
    if (!capture_ || running_)
        return false;
    callback_ = std::move(callback);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void AlsaBackend::run() {
    enableFlushToZero();
    if (config_.rtPriority > 0) {
        sched_param sp{};
        sp.sched_priority = config_.rtPriority;
        stats_.realtime = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    }
    if (config_.cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(config_.cpu, &set);
        stats_.pinned = pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
    }

    prefill();
    started_ = std::chrono::steady_clock::now();
    snd_pcm_start(capture_);

    const size_t capStride = captureChannels_ * bytesPerSample(captureFormat_);
    const size_t capOffset = config_.inputChannel * bytesPerSample(captureFormat_);
    const size_t playStride = playbackChannels_ * bytesPerSample(playbackFormat_);
    const size_t playBytes = bytesPerSample(playbackFormat_);
    const uint32_t frames = config_.period;

    while (running_) {
        snd_pcm_sframes_t n = snd_pcm_readi(capture_, captureRaw_.data(), frames);
        if (n < 0) {
            if (!recover(static_cast<int>(n), false))
                break;
            continue;
        }
        if (static_cast<uint32_t>(n) != frames) {
            recover(-EPIPE, false);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        // Frames already captured beyond this period: how late this block starts.
        const snd_pcm_sframes_t waiting = snd_pcm_avail_update(capture_);
        lastLateFrames_ = waiting > 0 ? static_cast<uint32_t>(waiting) : 0;

        for (uint32_t i = 0; i < frames; ++i)
            in_[i] = readSample(captureRaw_.data() + i * capStride + capOffset, captureFormat_);
        callback_(in_.data(), outPtrs_.data(), frames);
        std::fill(playbackRaw_.begin(), playbackRaw_.end(), 0);
        for (size_t o = 0; o < outs_.size(); ++o) {
            const size_t offset = config_.outputChannels[o] * playBytes;
            for (uint32_t i = 0; i < frames; ++i)
                writeSample(playbackRaw_.data() + i * playStride + offset, playbackFormat_, outs_[o][i]);
        }

        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        stats_.cycles++;
        stats_.totalCycleNs += ns;
        lastBlockNs_ = static_cast<uint32_t>(std::min<uint64_t>(ns, UINT32_MAX));
        if (ns > stats_.maxCycleNs.load(std::memory_order_relaxed))
            stats_.maxCycleNs.store(lastBlockNs_, std::memory_order_relaxed);
        const snd_pcm_sframes_t space = snd_pcm_avail_update(playback_);
        if (space >= 0 && playbackBuffer_ > 0) {
            const int32_t queued = static_cast<int32_t>(playbackBuffer_) - static_cast<int32_t>(space);
            if (queued < stats_.minQueuedFrames.load(std::memory_order_relaxed))
                stats_.minQueuedFrames.store(queued, std::memory_order_relaxed);
        }

        snd_pcm_sframes_t w = snd_pcm_writei(playback_, playbackRaw_.data(), frames);
        if (w < 0 && !recover(static_cast<int>(w), true))
            break;
    }
    if (running_)
        stats_.failed = true;
    snd_pcm_drop(capture_);
    snd_pcm_drop(playback_);
}

void AlsaBackend::stop() {
    if (!running_)
        return;
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void AlsaBackend::close() {
    stop();
    if (capture_ && playback_)
        snd_pcm_unlink(capture_);
    if (capture_)
        snd_pcm_close(capture_);
    if (playback_)
        snd_pcm_close(playback_);
    capture_ = playback_ = nullptr;
}

// ------------------------------------------------------------ no sound card

NullBackend::~NullBackend() { stop(); }

bool NullBackend::open(const AlsaConfig& config, std::string* error) {
    if (config.rate == 0 || config.period == 0) {
        *error = "rate and period must be above zero";
        return false;
    }
    config_ = config;
    return true;
}

bool NullBackend::start(Callback callback, std::string* error) {
    callback_ = std::move(callback);
    running_ = true;
    thread_ = std::thread([this] {
        enableFlushToZero();
        std::vector<float> in(config_.period, 0.0f);
        std::vector<std::vector<float>> outs(config_.outputChannels.size(), std::vector<float>(config_.period, 0.0f));
        std::vector<float*> ptrs;
        for (auto& o : outs)
            ptrs.push_back(o.data());
        const auto period = std::chrono::nanoseconds(1000000000ull * config_.period / config_.rate);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            next += period;
            const auto t0 = std::chrono::steady_clock::now();
            callback_(in.data(), ptrs.data(), config_.period);
            const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            stats_.cycles.fetch_add(1, std::memory_order_relaxed);
            stats_.totalCycleNs.fetch_add(ns, std::memory_order_relaxed);
            if (ns > stats_.maxCycleNs.load(std::memory_order_relaxed))
                stats_.maxCycleNs.store(static_cast<uint32_t>(std::min<uint64_t>(ns, UINT32_MAX)), std::memory_order_relaxed);
            std::this_thread::sleep_until(next);
            // Fell far behind (a debugger, a suspended machine): start again from now.
            if (std::chrono::steady_clock::now() - next > 50 * period)
                next = std::chrono::steady_clock::now();
        }
    });
    (void)error;
    return true;
}

void NullBackend::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

std::string NullBackend::describe() const {
    char buf[160];
    std::snprintf(buf, sizeof buf, "no sound card: the engine runs on a timer, hearing silence (%u Hz, %u frames, %zu outputs)", config_.rate,
                  config_.period, config_.outputChannels.size());
    return buf;
}

} // namespace cue
