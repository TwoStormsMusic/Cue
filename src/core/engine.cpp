#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace cue {

const char* toString(SceneState s) {
    switch (s) {
    case SceneState::Idle: return "idle";
    case SceneState::PreRoll: return "pre-roll";
    case SceneState::Warm: return "warm";
    case SceneState::Active: return "active";
    case SceneState::Releasing: return "tail";
    case SceneState::Stopping: return "stopping";
    }
    return "?";
}

Engine::Engine(EngineSettings settings) : settings_(settings) {}

Engine::~Engine() { deactivate(); }

int Engine::addScene(std::unique_ptr<Scene> scene) {
    Slot slot;
    slot.scene = std::move(scene);
    slot.stats = std::make_unique<SceneStats>();
    scenes_.push_back(std::move(slot));
    return static_cast<int>(scenes_.size()) - 1;
}

bool Engine::activate() {
    const uint32_t maxFrames = settings_.maxFrames;
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i].scene->numOutputs() != settings_.numOutputs || !scenes_[i].scene->activate(settings_.sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                scenes_[j].scene->deactivate();
            return false;
        }
    }
    inScratch_.assign(maxFrames, 0.0f);
    gainScratch_.assign(maxFrames, 0.0f);
    sceneOutStorage_.assign(static_cast<size_t>(maxFrames) * settings_.numOutputs, 0.0f);
    sceneOuts_.resize(settings_.numOutputs);
    for (size_t o = 0; o < settings_.numOutputs; ++o)
        sceneOuts_[o] = sceneOutStorage_.data() + o * maxFrames;
    activated_ = true;
    return true;
}

void Engine::deactivate() {
    if (!activated_)
        return;
    for (size_t i = 0; i < scenes_.size(); ++i) {
        if (scenes_[i].state != SceneState::Idle)
            scenes_[i].scene->stopProcessing();
        scenes_[i].state = SceneState::Idle;
        scenes_[i].scene->deactivate();
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

void Engine::emit(EngineEvent::Type type, int scene) {
    // The event reader is not real-time; if it falls behind, events are dropped.
    events_.push({type, scene});
}

void Engine::setState(int index, SceneState state) {
    scenes_[index].state = state;
    scenes_[index].stats->state.store(static_cast<uint8_t>(state), std::memory_order_relaxed);
}

void Engine::startScene(Slot& s, SceneState state) {
    s.scene->startProcessing();
    const int index = static_cast<int>(&s - scenes_.data());
    setState(index, state);
}

void Engine::stopScene(int index) {
    Slot& s = scenes_[index];
    s.scene->stopProcessing();
    s.inGain.set(0.0f);
    s.outGain.set(0.0f);
    setState(index, SceneState::Idle);
    emit(EngineEvent::Type::SceneIdle, index);
}

void Engine::handle(const EngineCommand& cmd) {
    switch (cmd.type) {
    case EngineCommand::Type::SwitchTo: requestSwitch(cmd.scene); break;
    case EngineCommand::Type::SetWarm: setWarmTarget(cmd.scene); break;
    }
}

void Engine::requestSwitch(int target) {
    if (target < 0 || target >= static_cast<int>(scenes_.size()))
        return;
    if (target == pending_)
        return;

    // A newer request replaces a pre-roll still in progress.
    if (pending_ >= 0) {
        const int cancelled = pending_;
        pending_ = -1;
        if (cancelled == warm_)
            setState(cancelled, SceneState::Warm);
        else
            stopScene(cancelled);
    }
    if (target == active_)
        return;

    Slot& s = scenes_[target];
    switch (s.state) {
    case SceneState::Idle:
        s.scene->reset();
        startScene(s, SceneState::PreRoll);
        // Fade the input in: delay lines repeat an abrupt start as a click
        // one delay time later, after the scene has become audible.
        s.inGain.set(0.0f);
        s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
        s.outGain.set(0.0f);
        s.prerollLeft = ms(settings_.prerollMs);
        pending_ = target;
        emit(EngineEvent::Type::ScenePreRoll, target);
        if (s.prerollLeft == 0)
            commit(target);
        break;
    case SceneState::Warm:
        if (settings_.resetTailsOnWarmSwitch)
            s.scene->resetTails(ms(settings_.tailResetMinMs), ms(settings_.crossfadeMs));
        commit(target);
        break;
    case SceneState::PreRoll:
    case SceneState::Releasing:
    case SceneState::Stopping:
    case SceneState::Active:
        commit(target);
        break;
    }
}

void Engine::commit(int target) {
    const uint32_t fade = ms(settings_.crossfadeMs);
    const int old = active_;
    if (old >= 0 && old != target) {
        Slot& o = scenes_[old];
        setState(old, SceneState::Releasing);
        o.inGain.dbTo(0.0f, fade);
        if (o.outGain.target() != 1.0f || !o.outGain.done())
            o.outGain.linearTo(1.0f, fade);
        o.releaseFrames = 0;
        o.quietFrames = 0;
    }

    Slot& s = scenes_[target];
    setState(target, SceneState::Active);
    s.outGain.linearTo(1.0f, fade);
    if (s.inGain.value() != 1.0f || !s.inGain.done())
        s.inGain.dbTo(1.0f, fade);
    active_ = target;
    if (pending_ == target)
        pending_ = -1;
    emit(EngineEvent::Type::SceneActive, target);
}

void Engine::setWarmTarget(int target) {
    if (target >= static_cast<int>(scenes_.size()))
        target = -1;
    if (target == warm_)
        return;
    const int previous = warm_;
    warm_ = target;
    if (previous >= 0 && scenes_[previous].state == SceneState::Warm)
        stopScene(previous);
    if (target < 0)
        return;
    Slot& s = scenes_[target];
    if (s.state == SceneState::Idle) {
        s.scene->reset();
        startScene(s, SceneState::Warm);
        s.inGain.set(0.0f);
        s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
        s.outGain.set(0.0f);
    }
    // A releasing scene becomes warm when its tail ends; active or
    // pre-rolling scenes keep their state.
}

void Engine::advance(int index, uint32_t frames, float peak) {
    Slot& s = scenes_[index];
    switch (s.state) {
    case SceneState::PreRoll:
        s.prerollLeft -= frames;
        if (s.prerollLeft <= 0)
            commit(index);
        break;
    case SceneState::Releasing: {
        s.releaseFrames += frames;
        const bool inputGone = s.inGain.done() && s.inGain.value() == 0.0f;
        if (inputGone) {
            const float threshold = std::pow(10.0f, settings_.tailQuietDb / 20.0f);
            s.quietFrames = peak < threshold ? s.quietFrames + frames : 0;
        }
        const bool quiet = inputGone && s.quietFrames >= ms(settings_.tailHoldMs);
        const bool capped = s.releaseFrames >= ms(settings_.tailCapMs);
        if (quiet || capped) {
            setState(index, SceneState::Stopping);
            s.outGain.linearTo(0.0f, ms(settings_.stopFadeMs));
        }
        break;
    }
    case SceneState::Stopping:
        if (s.outGain.done()) {
            if (index == warm_) {
                setState(index, SceneState::Warm);
                s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
            } else {
                stopScene(index);
            }
        }
        break;
    case SceneState::Idle:
    case SceneState::Warm:
    case SceneState::Active:
        break;
    }
}

void Engine::process(const float* in, float* const* outs, uint32_t frames) {
    frames = std::min(frames, settings_.maxFrames);
    while (auto cmd = commands_.pop())
        handle(*cmd);

    for (size_t o = 0; o < settings_.numOutputs; ++o)
        std::memset(outs[o], 0, frames * sizeof(float));

    // Idle scenes still receive parameter changes, every 5 ms.
    const bool flushIdle = (idleFlushCounter_ += frames) >= ms(5.0f);
    if (flushIdle)
        idleFlushCounter_ = 0;

    for (size_t i = 0; i < scenes_.size(); ++i) {
        Slot& s = scenes_[i];
        if (s.state == SceneState::Idle) {
            if (flushIdle)
                s.scene->flushIdle();
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();

        // Input, with the scene's input gain.
        if (s.inGain.done() && s.inGain.value() == 1.0f) {
            std::memcpy(inScratch_.data(), in, frames * sizeof(float));
        } else {
            s.inGain.render(gainScratch_.data(), frames);
            for (uint32_t n = 0; n < frames; ++n)
                inScratch_[n] = in[n] * gainScratch_[n];
        }

        s.scene->process(inScratch_.data(), sceneOuts_.data(), frames);

        // Output: muted scenes (pre-roll, warm) are processed but not heard.
        float peak = 0.0f;
        const bool muted = s.outGain.done() && s.outGain.value() == 0.0f;
        if (muted) {
            for (size_t o = 0; o < settings_.numOutputs; ++o)
                for (uint32_t n = 0; n < frames; ++n)
                    peak = std::max(peak, std::fabs(sceneOuts_[o][n]));
        } else {
            s.outGain.render(gainScratch_.data(), frames);
            for (size_t o = 0; o < settings_.numOutputs; ++o) {
                const float* src = sceneOuts_[o];
                float* dst = outs[o];
                for (uint32_t n = 0; n < frames; ++n) {
                    peak = std::max(peak, std::fabs(src[n]));
                    dst[n] += src[n] * gainScratch_[n];
                }
            }
        }

        const auto ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        SceneStats& st = *s.stats;
        st.processNs.fetch_add(ns, std::memory_order_relaxed);
        st.processBlocks.fetch_add(1, std::memory_order_relaxed);
        if (ns > st.maxBlockNs.load(std::memory_order_relaxed))
            st.maxBlockNs.store(static_cast<uint32_t>(std::min<uint64_t>(ns, UINT32_MAX)), std::memory_order_relaxed);

        advance(static_cast<int>(i), frames, peak);
    }
}

} // namespace cue
