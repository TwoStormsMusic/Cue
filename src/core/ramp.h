#pragma once

#include <cmath>
#include <cstdint>

namespace cue {

// A gain that moves towards a target over a fixed number of samples.
//
// Linear ramps are used for output crossfades: the scenes being mixed are
// played from the same input, so their signals are correlated and a linear
// (equal-gain) fade keeps the level constant.
//
// Exponential ramps (straight lines in dB) are used on a scene's input. Amp
// sims and compressors flatten a linear fade: the output stays loud until the
// last few percent and then drops abruptly. A dB ramp keeps that drop smooth.
class Ramp {
public:
    static constexpr float kFloor = 1.0e-4f; // -80 dB, below which a dB ramp snaps to 0

    explicit Ramp(float value = 0.0f) : value_(value), target_(value) {}

    void set(float value) {
        value_ = target_ = value;
        remaining_ = 0;
    }

    void linearTo(float target, uint32_t frames) {
        target_ = target;
        if (frames == 0 || value_ == target) {
            set(target);
            return;
        }
        exponential_ = false;
        remaining_ = frames;
        step_ = (target - value_) / static_cast<float>(frames);
    }

    void dbTo(float target, uint32_t frames) {
        target_ = target;
        if (frames == 0 || value_ == target) {
            set(target);
            return;
        }
        const float from = value_ < kFloor ? kFloor : value_;
        const float to = target < kFloor ? kFloor : target;
        value_ = from;
        exponential_ = true;
        remaining_ = frames;
        step_ = std::pow(to / from, 1.0f / static_cast<float>(frames));
    }

    float value() const { return value_; }
    float target() const { return target_; }
    bool done() const { return remaining_ == 0; }

    // Writes the gain for each of the next frames samples into out.
    void render(float* out, uint32_t frames) {
        uint32_t n = 0;
        for (; n < frames && remaining_ > 0; ++n, --remaining_) {
            value_ = exponential_ ? value_ * step_ : value_ + step_;
            out[n] = value_;
        }
        if (remaining_ == 0)
            value_ = target_;
        for (; n < frames; ++n)
            out[n] = value_;
    }

    // Advances without rendering, for blocks where the gain is not needed.
    void skip(uint32_t frames) {
        if (frames >= remaining_) {
            remaining_ = 0;
            value_ = target_;
            return;
        }
        remaining_ -= frames;
        value_ = exponential_ ? value_ * std::pow(step_, static_cast<float>(frames))
                              : value_ + step_ * static_cast<float>(frames);
    }

private:
    float value_;
    float target_;
    float step_ = 0.0f;
    uint32_t remaining_ = 0;
    bool exponential_ = false;
};

} // namespace cue
