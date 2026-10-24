#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace cue {

const char* toString(ChainState s) {
    switch (s) {
    case ChainState::Idle: return "idle";
    case ChainState::PreRoll: return "pre-roll";
    case ChainState::Warm: return "warm";
    case ChainState::Active: return "active";
    case ChainState::Releasing: return "tail";
    case ChainState::Stopping: return "stopping";
    }
    return "?";
}

Engine::Engine(EngineSettings settings) : settings_(settings) {}

Engine::~Engine() { deactivate(); }

int Engine::addChain(std::unique_ptr<Chain> chain) {
    Slot slot;
    slot.chain = std::move(chain);
    slot.stats = std::make_unique<ChainStats>();
    chains_.push_back(std::move(slot));
    return static_cast<int>(chains_.size()) - 1;
}

bool Engine::activate() {
    const uint32_t maxFrames = settings_.maxFrames;
    for (size_t i = 0; i < chains_.size(); ++i) {
        if (chains_[i].chain->numOutputs() != settings_.numOutputs || !chains_[i].chain->activate(settings_.sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                chains_[j].chain->deactivate();
            return false;
        }
    }
    inScratch_.assign(maxFrames, 0.0f);
    gainScratch_.assign(maxFrames, 0.0f);
    chainOutStorage_.assign(static_cast<size_t>(maxFrames) * settings_.numOutputs, 0.0f);
    chainOuts_.resize(settings_.numOutputs);
    for (size_t o = 0; o < settings_.numOutputs; ++o)
        chainOuts_[o] = chainOutStorage_.data() + o * maxFrames;
    activated_ = true;
    return true;
}

void Engine::deactivate() {
    if (!activated_)
        return;
    for (size_t i = 0; i < chains_.size(); ++i) {
        if (chains_[i].state != ChainState::Idle)
            chains_[i].chain->stopProcessing();
        chains_[i].state = ChainState::Idle;
        chains_[i].chain->deactivate();
    }
    activated_ = false;
}

bool Engine::nextEvent(EngineEvent& ev) {
    auto e = events_.pop();
    if (!e)
        return false;
    ev = *e;
    return true;
}

uint32_t Engine::ms(float milliseconds) const {
    return static_cast<uint32_t>(std::lround(std::max(0.0f, milliseconds) * settings_.sampleRate / 1000.0));
}

void Engine::emit(EngineEvent::Type type, int chain) {
    // The event reader is not real-time; if it falls behind, events are dropped.
    events_.push({type, chain});
}

void Engine::setState(int index, ChainState state) {
    chains_[index].state = state;
    chains_[index].stats->state.store(static_cast<uint8_t>(state), std::memory_order_relaxed);
}

void Engine::startChain(Slot& s, ChainState state) {
    s.chain->startProcessing();
    const int index = static_cast<int>(&s - chains_.data());
    setState(index, state);
}

void Engine::stopChain(int index) {
    Slot& s = chains_[index];
    s.chain->stopProcessing();
    s.inGain.set(0.0f);
    s.outGain.set(0.0f);
    setState(index, ChainState::Idle);
    emit(EngineEvent::Type::ChainIdle, index);
}

void Engine::handle(const EngineCommand& cmd) {
    switch (cmd.type) {
    case EngineCommand::Type::SwitchTo: requestSwitch(cmd.chain); break;
    case EngineCommand::Type::SetWarm: setWarmTarget(cmd.chain); break;
    case EngineCommand::Type::SetBypass:
        if (cmd.chain >= 0 && cmd.chain < static_cast<int>(chains_.size())) {
            Slot& s = chains_[cmd.chain];
            s.chain->setBypass(cmd.slot, cmd.on, s.state == ChainState::Idle ? 0 : ms(settings_.bypassFadeMs));
        }
        break;
    case EngineCommand::Type::SetMute:
        if (cmd.on != muted_) {
            muted_ = cmd.on;
            outputGain_.linearTo(muted_ ? 0.0f : 1.0f, ms(settings_.muteFadeMs));
        }
        break;
    }
}

void Engine::requestSwitch(int target) {
    if (target < 0 || target >= static_cast<int>(chains_.size()))
        return;
    if (target == pending_)
        return;

    // A newer request replaces a pre-roll still in progress.
    if (pending_ >= 0) {
        const int cancelled = pending_;
        pending_ = -1;
        if (cancelled == warm_)
            setState(cancelled, ChainState::Warm);
        else
            stopChain(cancelled);
    }
    if (target == active_)
        return;

    Slot& s = chains_[target];
    switch (s.state) {
    case ChainState::Idle:
        s.chain->reset();
        startChain(s, ChainState::PreRoll);
        // Fade the input in: delay lines repeat an abrupt start as a click
        // one delay time later, after the chain has become audible.
        s.inGain.set(0.0f);
        s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
        s.outGain.set(0.0f);
        s.prerollLeft = ms(settings_.prerollMs);
        pending_ = target;
        emit(EngineEvent::Type::ChainPreRoll, target);
        if (s.prerollLeft == 0)
            commit(target);
        break;
    case ChainState::Warm:
        if (settings_.resetTailsOnWarmSwitch)
            s.chain->resetTails(ms(settings_.tailResetMinMs), ms(settings_.crossfadeMs));
        commit(target);
        break;
    case ChainState::PreRoll:
    case ChainState::Releasing:
    case ChainState::Stopping:
    case ChainState::Active:
        commit(target);
        break;
    }
}

void Engine::commit(int target) {
    const uint32_t fade = ms(settings_.crossfadeMs);
    const int old = active_;
    if (old >= 0 && old != target) {
        Slot& o = chains_[old];
        setState(old, ChainState::Releasing);
        o.inGain.dbTo(0.0f, fade);
        if (o.outGain.target() != 1.0f || !o.outGain.done())
            o.outGain.linearTo(1.0f, fade);
        o.releaseFrames = 0;
        o.quietFrames = 0;
    }

    Slot& s = chains_[target];
    setState(target, ChainState::Active);
    s.outGain.linearTo(1.0f, fade);
    if (s.inGain.value() != 1.0f || !s.inGain.done())
        s.inGain.dbTo(1.0f, fade);
    active_ = target;
    if (pending_ == target)
        pending_ = -1;
    emit(EngineEvent::Type::ChainActive, target);
}

void Engine::setWarmTarget(int target) {
    if (target >= static_cast<int>(chains_.size()))
        target = -1;
    if (target == warm_)
        return;
    const int previous = warm_;
    warm_ = target;
    if (previous >= 0 && chains_[previous].state == ChainState::Warm)
        stopChain(previous);
    if (target < 0)
        return;
    Slot& s = chains_[target];
    if (s.state == ChainState::Idle) {
        s.chain->reset();
        startChain(s, ChainState::Warm);
        s.inGain.set(0.0f);
        s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
        s.outGain.set(0.0f);
    }
    // A releasing chain becomes warm when its tail ends; active or
    // pre-rolling chains keep their state.
}

void Engine::advance(int index, uint32_t frames, float peak) {
    Slot& s = chains_[index];
    switch (s.state) {
    case ChainState::PreRoll:
        s.prerollLeft -= frames;
        if (s.prerollLeft <= 0)
            commit(index);
        break;
    case ChainState::Releasing: {
        s.releaseFrames += frames;
        const bool inputGone = s.inGain.done() && s.inGain.value() == 0.0f;
        if (inputGone) {
            const float threshold = std::pow(10.0f, settings_.tailQuietDb / 20.0f);
            s.quietFrames = peak < threshold ? s.quietFrames + frames : 0;
        }
        const bool quiet = inputGone && s.quietFrames >= ms(settings_.tailHoldMs);
        const bool capped = s.releaseFrames >= ms(settings_.tailCapMs);
        if (quiet || capped) {
            setState(index, ChainState::Stopping);
            s.outGain.linearTo(0.0f, ms(settings_.stopFadeMs));
        }
        break;
    }
    case ChainState::Stopping:
        if (s.outGain.done()) {
            if (index == warm_) {
                setState(index, ChainState::Warm);
                s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
            } else {
                stopChain(index);
            }
        }
        break;
    case ChainState::Idle:
    case ChainState::Warm:
    case ChainState::Active:
        break;
    }
}

void Engine::process(const float* in, float* const* outs, uint32_t frames) {
    frames = std::min(frames, settings_.maxFrames);
    while (auto cmd = commands_.pop())
        handle(*cmd);

    for (size_t o = 0; o < settings_.numOutputs; ++o)
        std::memset(outs[o], 0, frames * sizeof(float));

    // Idle chains still receive parameter changes, every 5 ms.
    const bool flushIdle = (idleFlushCounter_ += frames) >= ms(5.0f);
    if (flushIdle)
        idleFlushCounter_ = 0;

    for (size_t i = 0; i < chains_.size(); ++i) {
        Slot& s = chains_[i];
        if (s.state == ChainState::Idle) {
            if (flushIdle)
                s.chain->flushIdle();
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();

        // Input, with the chain's input gain.
        if (s.inGain.done() && s.inGain.value() == 1.0f) {
            std::memcpy(inScratch_.data(), in, frames * sizeof(float));
        } else {
            s.inGain.render(gainScratch_.data(), frames);
            for (uint32_t n = 0; n < frames; ++n)
                inScratch_[n] = in[n] * gainScratch_[n];
        }

        s.chain->process(inScratch_.data(), chainOuts_.data(), frames);

        // Output: muted chains (pre-roll, warm) are processed but not heard.
        float peak = 0.0f;
        const bool muted = s.outGain.done() && s.outGain.value() == 0.0f;
        if (muted) {
            for (size_t o = 0; o < settings_.numOutputs; ++o)
                for (uint32_t n = 0; n < frames; ++n)
                    peak = std::max(peak, std::fabs(chainOuts_[o][n]));
        } else {
            s.outGain.render(gainScratch_.data(), frames);
            for (size_t o = 0; o < settings_.numOutputs; ++o) {
                const float* src = chainOuts_[o];
                float* dst = outs[o];
                for (uint32_t n = 0; n < frames; ++n) {
                    peak = std::max(peak, std::fabs(src[n]));
                    dst[n] += src[n] * gainScratch_[n];
                }
            }
        }

        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        ChainStats& st = *s.stats;
        st.processNs.fetch_add(ns, std::memory_order_relaxed);
        st.processBlocks.fetch_add(1, std::memory_order_relaxed);
        if (ns > st.maxBlockNs.load(std::memory_order_relaxed))
            st.maxBlockNs.store(static_cast<uint32_t>(std::min<uint64_t>(ns, UINT32_MAX)), std::memory_order_relaxed);

        advance(static_cast<int>(i), frames, peak);
    }

    if (!outputGain_.done() || outputGain_.value() != 1.0f) {
        outputGain_.render(gainScratch_.data(), frames);
        for (size_t o = 0; o < settings_.numOutputs; ++o)
            for (uint32_t n = 0; n < frames; ++n)
                outs[o][n] *= gainScratch_[n];
    }
}

} // namespace cue
