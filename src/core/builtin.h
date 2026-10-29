#pragma once

// Built-in processors. They stand in for plug-ins in tests and benchmarks,
// and the plain ones double as a safe fallback scene (a clean DI chain).

#include "processor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace cue {

class BuiltinProcessor : public Processor {
public:
    explicit BuiltinProcessor(std::string name) : name_(std::move(name)) {}
    const std::string& name() const override { return name_; }
    bool activate(double sampleRate, uint32_t) override {
        sampleRate_ = sampleRate;
        return true;
    }
    void deactivate() override {}
    void reset() override {}

protected:
    std::string name_;
    double sampleRate_ = 48000.0;
};

class GainProcessor : public BuiltinProcessor {
public:
    explicit GainProcessor(float gain, std::string name = "Gain") : BuiltinProcessor(std::move(name)), gain_(gain) {}
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                out[c][n] = in[c][n] * gain_;
    }

private:
    float gain_;
};

// Soft clipper: a crude, very nonlinear "amp".
class DriveProcessor : public BuiltinProcessor {
public:
    explicit DriveProcessor(float drive, std::string name = "Drive") : BuiltinProcessor(std::move(name)), drive_(drive) {}
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        const float norm = 1.0f / std::tanh(drive_);
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                out[c][n] = std::tanh(in[c][n] * drive_) * norm * 0.5f;
    }

private:
    float drive_;
};

class LowpassProcessor : public BuiltinProcessor {
public:
    explicit LowpassProcessor(float cutoffHz, std::string name = "Lowpass") : BuiltinProcessor(std::move(name)), cutoff_(cutoffHz) {}
    bool activate(double sampleRate, uint32_t maxFrames) override {
        BuiltinProcessor::activate(sampleRate, maxFrames);
        coeff_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff_ / static_cast<float>(sampleRate));
        return true;
    }
    void reset() override { std::fill(std::begin(z_), std::end(z_), 0.0f); }
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        for (int c = 0; c < kChainChannels; ++c) {
            float z = z_[c];
            for (uint32_t n = 0; n < frames; ++n) {
                z += coeff_ * (in[c][n] - z);
                out[c][n] = z;
            }
            z_[c] = z;
        }
    }

private:
    float cutoff_;
    float coeff_ = 1.0f;
    float z_[kChainChannels] = {};
};

// Feedback delay with a dry path: the source of tails.
class DelayProcessor : public BuiltinProcessor {
public:
    DelayProcessor(float delayMs, float feedback, float mix, std::string name = "Delay")
        : BuiltinProcessor(std::move(name)), delayMs_(delayMs), feedback_(feedback), mix_(mix) {}
    bool activate(double sampleRate, uint32_t maxFrames) override {
        BuiltinProcessor::activate(sampleRate, maxFrames);
        delay_ = std::max<size_t>(1, static_cast<size_t>(delayMs_ * sampleRate / 1000.0));
        for (auto& line : lines_)
            line.assign(delay_, 0.0f);
        pos_ = 0;
        return true;
    }
    void reset() override {
        for (auto& line : lines_)
            std::fill(line.begin(), line.end(), 0.0f);
        pos_ = 0;
    }
    uint32_t tailSamples() const override {
        if (feedback_ >= 1.0f)
            return kInfiniteTail;
        // Time for echoes to fall by 90 dB, plus one delay.
        const double repeats = feedback_ > 0.0f ? std::log(3.16e-5) / std::log(feedback_) : 1.0;
        return static_cast<uint32_t>(std::min(1e9, (repeats + 1.0) * static_cast<double>(delay_)));
    }
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        size_t pos = pos_;
        for (uint32_t n = 0; n < frames; ++n) {
            for (int c = 0; c < kChainChannels; ++c) {
                float& slot = lines_[c][pos];
                const float echo = slot;
                slot = in[c][n] + echo * feedback_;
                out[c][n] = in[c][n] + echo * mix_;
            }
            if (++pos == delay_)
                pos = 0;
        }
        pos_ = pos;
    }

private:
    float delayMs_, feedback_, mix_;
    size_t delay_ = 1;
    size_t pos_ = 0;
    std::vector<float> lines_[kChainChannels];
};

// Burns a fixed amount of arithmetic per sample, to stand in for a heavy amp
// sim in CPU tests. The result feeds the output at -200 dB so the work cannot
// be optimised away.
class BurnProcessor : public BuiltinProcessor {
public:
    explicit BurnProcessor(uint32_t opsPerSample, std::string name = "Burn")
        : BuiltinProcessor(std::move(name)), ops_(opsPerSample) {}
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        for (uint32_t n = 0; n < frames; ++n) {
            float acc = in[0][n];
            for (uint32_t k = 0; k < ops_; ++k)
                acc = acc * 0.999999f + 1.0e-7f;
            for (int c = 0; c < kChainChannels; ++c)
                out[c][n] = in[c][n] + acc * 1.0e-10f;
        }
    }

private:
    uint32_t ops_;
};

// Wraps another processor and counts calls, for tests.
class CountingProcessor : public Processor {
public:
    struct Counts {
        std::atomic<uint64_t> process{0}, frames{0}, reset{0}, start{0}, stop{0}, deactivate{0}, destroyed{0};
        std::atomic<bool> processingStarted{false};
        std::atomic<uint64_t> processWhileStopped{0};
    };

    CountingProcessor(std::unique_ptr<Processor> inner, Counts& counts) : inner_(std::move(inner)), counts_(counts) {}
    ~CountingProcessor() override { counts_.destroyed++; }
    const std::string& name() const override { return inner_->name(); }
    bool activate(double sr, uint32_t maxFrames) override { return inner_->activate(sr, maxFrames); }
    void deactivate() override {
        counts_.deactivate++;
        inner_->deactivate();
    }
    void startProcessing() override {
        counts_.start++;
        counts_.processingStarted = true;
        inner_->startProcessing();
    }
    void stopProcessing() override {
        counts_.stop++;
        counts_.processingStarted = false;
        inner_->stopProcessing();
    }
    void reset() override {
        counts_.reset++;
        inner_->reset();
    }
    void process(const float* const* in, float* const* out, uint32_t frames) override {
        counts_.process++;
        counts_.frames += frames;
        if (!counts_.processingStarted)
            counts_.processWhileStopped++;
        inner_->process(in, out, frames);
    }
    uint32_t latencySamples() const override { return inner_->latencySamples(); }
    uint32_t tailSamples() const override { return inner_->tailSamples(); }

private:
    std::unique_ptr<Processor> inner_;
    Counts& counts_;
};

} // namespace cue
