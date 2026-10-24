#include "chain.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cue {

namespace {
std::vector<Line> oneLine(std::vector<std::unique_ptr<Processor>> slots) {
    std::vector<Line> lines(1);
    lines[0].slots = std::move(slots);
    return lines;
}
} // namespace

Chain::Chain(std::string name, std::vector<std::unique_ptr<Processor>> slots, std::vector<OutputTap> taps)
    : Chain(std::move(name), oneLine(std::move(slots)), std::move(taps)) {}

Chain::Chain(std::string name, std::vector<Line> lines, std::vector<OutputTap> taps)
    : name_(std::move(name)), taps_(std::move(taps)) {
    if (lines.empty())
        lines.emplace_back();
    for (size_t i = 0; i < lines.size(); ++i) {
        Line& l = lines[i];
        LineState ls;
        // A branch must come off an earlier line, which is processed first.
        ls.parent = i == 0 ? -1 : (l.parent >= 0 && l.parent < static_cast<int>(i) ? l.parent : 0);
        ls.afterSlots = i == 0 ? 0 : std::min(l.afterSlots, lines_[ls.parent].count);
        ls.first = slots_.size();
        ls.count = l.slots.size();
        for (auto& p : l.slots) {
            SlotState s;
            s.processor = std::move(p);
            slots_.push_back(std::move(s));
        }
        lines_.push_back(ls);
    }
    for (auto& tap : taps_) {
        if (tap.line < 0 || tap.line >= static_cast<int>(lines_.size()))
            tap.line = 0;
        tap.afterSlots = std::min(tap.afterSlots, lines_[tap.line].count);
    }
}

void Chain::setInitialBypass(size_t slot, bool bypassed) {
    SlotState& s = slots_[slot];
    s.bypassed = bypassed;
    s.wet.set(bypassed ? 0.0f : 1.0f);
}

bool Chain::activate(double sampleRate, uint32_t maxFrames) {
    maxFrames_ = maxFrames;
    // Two stereo buffers per line plus one scratch pair.
    const size_t perChannel = maxFrames;
    storage_.assign(perChannel * kChainChannels * (2 * lines_.size() + 1), 0.0f);
    float* p = storage_.data();
    for (auto& l : lines_) {
        for (int c = 0; c < kChainChannels; ++c, p += perChannel)
            l.a[c] = p;
        for (int c = 0; c < kChainChannels; ++c, p += perChannel)
            l.b[c] = p;
    }
    for (int c = 0; c < kChainChannels; ++c, p += perChannel)
        tmp_[c] = p;
    gainScratch_.assign(maxFrames, 0.0f);
    wetScratch_.assign(maxFrames, 0.0f);
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].processor->activate(sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                slots_[j].processor->deactivate();
            return false;
        }
    }
    active_ = true;
    return true;
}

void Chain::deactivate() {
    if (!active_)
        return;
    for (auto& s : slots_)
        s.processor->deactivate();
    active_ = false;
}

uint32_t Chain::tailSamples() const {
    uint32_t tail = 0;
    for (const auto& s : slots_) {
        if (s.bypassed)
            continue;
        const uint32_t t = s.processor->tailSamples();
        if (t == kInfiniteTail)
            return kInfiniteTail;
        tail = std::max(tail, t);
    }
    return tail;
}

uint32_t Chain::latencySamples() const {
    uint32_t latency = 0;
    for (size_t i = 0; i < lines_[0].count; ++i)
        if (!slots_[i].bypassed)
            latency += slots_[i].processor->latencySamples();
    return latency;
}

void Chain::startProcessing() {
    for (auto& s : slots_)
        s.processor->startProcessing();
}

void Chain::stopProcessing() {
    for (auto& s : slots_)
        s.processor->stopProcessing();
}

void Chain::reset() {
    for (auto& s : slots_) {
        s.processor->reset();
        s.inGain.set(1.0f);
        s.wet.set(s.bypassed ? 0.0f : 1.0f);
    }
}

void Chain::resetTails(uint32_t minTail, uint32_t fadeFrames) {
    for (auto& s : slots_) {
        if (!s.bypassed && s.processor->tailSamples() >= minTail) {
            s.processor->reset();
            s.inGain.set(0.0f);
            s.inGain.dbTo(1.0f, fadeFrames);
        }
    }
}

void Chain::setBypass(size_t slot, bool bypassed, uint32_t fadeFrames) {
    if (slot >= slots_.size())
        return;
    SlotState& s = slots_[slot];
    if (s.bypassed == bypassed)
        return;
    s.bypassed = bypassed;
    if (bypassed) {
        s.wet.linearTo(0.0f, fadeFrames);
        return;
    }
    if (s.wet.done() && s.wet.value() == 0.0f) {
        s.processor->reset();
        s.inGain.set(0.0f);
        s.inGain.dbTo(1.0f, fadeFrames);
    }
    s.wet.linearTo(1.0f, fadeFrames);
}

void Chain::flushIdle() {
    for (auto& s : slots_)
        s.processor->flushIdle();
}

void Chain::writeTap(size_t tap, const float* const* signal, float* out, uint32_t frames) const {
    switch (taps_[tap].mix) {
    case OutputMix::Left:
        std::memcpy(out, signal[0], frames * sizeof(float));
        break;
    case OutputMix::Right:
        std::memcpy(out, signal[1], frames * sizeof(float));
        break;
    case OutputMix::Sum:
        for (uint32_t n = 0; n < frames; ++n)
            out[n] = 0.5f * (signal[0][n] + signal[1][n]);
        break;
    }
}

void Chain::processSlot(SlotState& s, float**& cur, float**& next, uint32_t frames) {
    const bool fullyBypassed = s.wet.done() && s.wet.value() == 0.0f;
    if (fullyBypassed)
        return; // the signal passes through untouched
    const bool fading = !s.wet.done() || s.wet.value() != 1.0f;

    // The plug-in's input, faded in after a reset. The dry signal for a
    // bypass crossfade stays at full level.
    const float* const* input = cur;
    if (!s.inGain.done() || s.inGain.value() != 1.0f) {
        s.inGain.render(gainScratch_.data(), frames);
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                tmp_[c][n] = cur[c][n] * gainScratch_[n];
        input = tmp_;
    }
    s.processor->process(input, next, frames);
    if (fading) {
        s.wet.render(wetScratch_.data(), frames);
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                next[c][n] = cur[c][n] + wetScratch_[n] * (next[c][n] - cur[c][n]);
    }
    std::swap(cur, next);
}

void Chain::process(const float* in, float* const* outs, uint32_t frames) {
    frames = std::min(frames, maxFrames_);
    for (size_t t = 0; t < taps_.size(); ++t)
        std::memset(outs[t], 0, frames * sizeof(float));
    for (int c = 0; c < kChainChannels; ++c)
        std::memcpy(lines_[0].a[c], in, frames * sizeof(float));

    for (size_t li = 0; li < lines_.size(); ++li) {
        LineState& line = lines_[li];
        float* curStore[kChainChannels];
        float* nextStore[kChainChannels];
        for (int c = 0; c < kChainChannels; ++c) {
            curStore[c] = line.a[c];
            nextStore[c] = line.b[c];
        }
        float** cur = curStore;
        float** next = nextStore;
        for (size_t k = 0; k <= line.count; ++k) {
            // Branches leaving here get a copy of the signal at this point.
            for (size_t bi = li + 1; bi < lines_.size(); ++bi)
                if (lines_[bi].parent == static_cast<int>(li) && lines_[bi].afterSlots == k)
                    for (int c = 0; c < kChainChannels; ++c)
                        std::memcpy(lines_[bi].a[c], cur[c], frames * sizeof(float));
            for (size_t t = 0; t < taps_.size(); ++t)
                if (taps_[t].line == static_cast<int>(li) && taps_[t].afterSlots == k)
                    writeTap(t, cur, outs[t], frames);
            if (k == line.count)
                break;
            processSlot(slots_[line.first + k], cur, next, frames);
        }
    }
}

} // namespace cue
