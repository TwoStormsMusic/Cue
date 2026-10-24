#include "scene.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cue {

Scene::Scene(std::string name, std::vector<std::unique_ptr<Processor>> chain, std::vector<OutputTap> taps)
    : name_(std::move(name)), chain_(std::move(chain)), taps_(std::move(taps)) {
    for (auto& tap : taps_)
        tap.afterSlots = std::min(tap.afterSlots, chain_.size());
}

bool Scene::activate(double sampleRate, uint32_t maxFrames) {
    maxFrames_ = maxFrames;
    storage_.assign(static_cast<size_t>(maxFrames) * kChainChannels * 2, 0.0f);
    slotInGain_.assign(chain_.size(), Ramp(1.0f));
    gainScratch_.assign(maxFrames, 0.0f);
    for (int c = 0; c < kChainChannels; ++c) {
        bufA_[c] = storage_.data() + static_cast<size_t>(c) * maxFrames;
        bufB_[c] = storage_.data() + static_cast<size_t>(kChainChannels + c) * maxFrames;
    }
    for (size_t i = 0; i < chain_.size(); ++i) {
        if (!chain_[i]->activate(sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                chain_[j]->deactivate();
            return false;
        }
    }
    active_ = true;
    return true;
}

void Scene::deactivate() {
    if (!active_)
        return;
    for (auto& p : chain_)
        p->deactivate();
    active_ = false;
}

uint32_t Scene::tailSamples() const {
    uint32_t tail = 0;
    for (const auto& p : chain_) {
        const uint32_t t = p->tailSamples();
        if (t == kInfiniteTail)
            return kInfiniteTail;
        tail = std::max(tail, t);
    }
    return tail;
}

uint32_t Scene::latencySamples() const {
    uint32_t latency = 0;
    for (const auto& p : chain_)
        latency += p->latencySamples();
    return latency;
}

void Scene::startProcessing() {
    for (auto& p : chain_)
        p->startProcessing();
}

void Scene::stopProcessing() {
    for (auto& p : chain_)
        p->stopProcessing();
}

void Scene::reset() {
    for (auto& p : chain_)
        p->reset();
    for (auto& g : slotInGain_)
        g.set(1.0f);
}

void Scene::resetTails(uint32_t minTail, uint32_t fadeFrames) {
    for (size_t i = 0; i < chain_.size(); ++i) {
        if (chain_[i]->tailSamples() >= minTail) {
            chain_[i]->reset();
            slotInGain_[i].set(0.0f);
            slotInGain_[i].dbTo(1.0f, fadeFrames);
        }
    }
}

void Scene::flushIdle() {
    for (auto& p : chain_)
        p->flushIdle();
}

void Scene::writeTap(size_t tap, const float* const* chain, float* out, uint32_t frames) const {
    switch (taps_[tap].mix) {
    case OutputMix::Left:
        std::memcpy(out, chain[0], frames * sizeof(float));
        break;
    case OutputMix::Right:
        std::memcpy(out, chain[1], frames * sizeof(float));
        break;
    case OutputMix::Sum:
        for (uint32_t n = 0; n < frames; ++n)
            out[n] = 0.5f * (chain[0][n] + chain[1][n]);
        break;
    }
}

void Scene::process(const float* in, float* const* outs, uint32_t frames) {
    for (int c = 0; c < kChainChannels; ++c)
        std::memcpy(bufA_[c], in, frames * sizeof(float));

    float** cur = bufA_;
    float** next = bufB_;
    for (size_t slot = 0; slot <= chain_.size(); ++slot) {
        for (size_t t = 0; t < taps_.size(); ++t)
            if (taps_[t].afterSlots == slot)
                writeTap(t, cur, outs[t], frames);
        if (slot == chain_.size())
            break;
        Ramp& fade = slotInGain_[slot];
        if (!fade.done()) {
            fade.render(gainScratch_.data(), frames);
            for (int c = 0; c < kChainChannels; ++c)
                for (uint32_t n = 0; n < frames; ++n)
                    cur[c][n] *= gainScratch_[n];
        }
        chain_[slot]->process(cur, next, frames);
        std::swap(cur, next);
    }
}

} // namespace cue
