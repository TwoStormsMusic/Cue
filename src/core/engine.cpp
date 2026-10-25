#include "engine.h"

#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <xmmintrin.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

namespace cue {

namespace {

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

void futexWait(std::atomic<uint32_t>& word, uint32_t expected) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}

void futexWake(std::atomic<uint32_t>& word, int count) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

thread_local bool t_helper = false; // one of the engine's helper threads

uint32_t nsSince(std::chrono::steady_clock::time_point t0) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    return static_cast<uint32_t>(std::min<int64_t>(ns, UINT32_MAX));
}

} // namespace

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
    slot.clean = std::make_unique<std::atomic<uint8_t>>(kClean);
    chains_.push_back(std::move(slot));
    return static_cast<int>(chains_.size()) - 1;
}

bool Engine::activate() {
    const uint32_t maxFrames = settings_.maxFrames;
    for (size_t i = 0; i < chains_.size(); ++i) {
        chains_[i].chain->setDrain(std::pow(10.0f, settings_.tailQuietDb / 20.0f), ms(settings_.tailHoldMs), ms(settings_.tailCapMs));
        if (chains_[i].chain->numOutputs() != settings_.numOutputs || !chains_[i].chain->activate(settings_.sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                chains_[j].chain->deactivate();
            return false;
        }
    }
    gainScratch_.assign(maxFrames, 0.0f);
    for (auto& s : chains_) {
        s.buffers.assign((2 + settings_.numOutputs) * static_cast<size_t>(maxFrames), 0.0f);
        s.in = s.buffers.data();
        s.gain = s.in + maxFrames;
        s.outs.resize(settings_.numOutputs);
        for (size_t o = 0; o < settings_.numOutputs; ++o)
            s.outs[o] = s.gain + (1 + o) * static_cast<size_t>(maxFrames);
    }
    jobs_.assign(chains_.size(), -1);
    activated_ = true;
    stopResetter_ = false;
    resetter_ = std::thread([this] { resetLoop(); });
    stopWorkers_ = false;
    realtimeWorkers_ = 0;
    generation_ = wake_.load();
    for (int i = 0; i < settings_.workers; ++i)
        workers_.emplace_back([this] { workerLoop(); });
    return true;
}

// A helper: an audio thread in CLAP's terms, processing whichever playing
// chains the audio thread leaves to it. It sleeps between blocks.
void Engine::workerLoop() {
    markAudioThread();
    t_helper = true;
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush-to-zero, denormals-are-zero
    if (settings_.workerPriority > 0) {
        sched_param sp{};
        sp.sched_priority = settings_.workerPriority;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
            realtimeWorkers_++;
    }
    uint32_t seen = wake_.load();
    while (!stopWorkers_.load(std::memory_order_relaxed)) {
        const uint32_t generation = wake_.load(std::memory_order_acquire);
        if (generation == seen) {
            // The audio thread looks at sleepers_ after bumping wake_, and
            // the futex sleeps only if wake_ is still unchanged: no wake-up
            // is lost.
            sleepers_.fetch_add(1);
            futexWait(wake_, seen);
            sleepers_.fetch_sub(1);
            continue;
        }
        seen = generation;
        takeJobs(generation);
    }
}

void Engine::stopWorkers() {
    if (workers_.empty())
        return;
    stopWorkers_ = true;
    wake_.fetch_add(1);
    futexWake(wake_, INT_MAX);
    for (auto& w : workers_)
        w.join();
    workers_.clear();
}

// Claims this block's chains one at a time, until none is left. A claim
// only succeeds while the block is still the one in progress.
void Engine::takeJobs(uint32_t generation) {
    uint64_t t = ticket_.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = t & 0xffff, count = (t >> 16) & 0xffff;
        if (static_cast<uint32_t>(t >> 32) != generation || next >= count)
            return;
        if (!ticket_.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        runChain(jobs_[next], t_helper);
        jobsDone_.fetch_add(1, std::memory_order_release);
        t = ticket_.load(std::memory_order_acquire);
    }
}

// The reset helper: an audio thread in CLAP's terms, working only on idle
// chains, which nothing else processes or resets meanwhile.
void Engine::resetLoop() {
    markAudioThread();
    while (!stopResetter_.load(std::memory_order_relaxed)) {
        bool worked = false;
        for (auto& s : chains_) {
            uint8_t expected = kDirty;
            if (s.clean->compare_exchange_strong(expected, kResetting)) {
                s.chain->resetProcessors();
                s.clean->store(kClean);
                worked = true;
            }
        }
        if (!worked)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Engine::prime(float milliseconds) {
    std::thread([&] {
        markAudioThread();
        const uint32_t frames = settings_.maxFrames;
        std::vector<float> buffers(6 * static_cast<size_t>(frames), 0.0f);
        const float* silence[kChainChannels] = {buffers.data(), buffers.data() + frames};
        float* scratch[kChainChannels] = {buffers.data() + 2 * frames, buffers.data() + 3 * frames};
        // Quiet noise (-40 dBFS), never heard: some plug-ins skip their DSP
        // on digital silence and only set up on the first real signal, which
        // would then land in the first audio block that hears them.
        float* noiseStore = buffers.data() + 4 * frames;
        uint32_t seed = 12345;
        for (size_t n = 0; n < 2 * static_cast<size_t>(frames); ++n) {
            seed = seed * 1664525u + 1013904223u;
            noiseStore[n] = 0.01f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
        }
        const float* noise[kChainChannels] = {noiseStore, noiseStore + frames};
        const size_t blocks = std::max<uint32_t>(1, ms(milliseconds) / frames);
        // Every plug-in, bypassed ones too: they may be switched on mid-song.
        for (auto& s : chains_) {
            if (s.state != ChainState::Idle)
                continue;
            for (size_t i = 0; i < s.chain->numSlots(); ++i) {
                Processor& p = s.chain->slot(i);
                p.startProcessing();
                for (size_t b = 0; b < blocks; ++b)
                    p.process(silence, scratch, frames);
                for (size_t b = 0; b < blocks; ++b)
                    p.process(noise, scratch, frames);
                // Clear what the noise left, then let anything a reset
                // misses die away on silence.
                p.reset();
                for (size_t b = 0; b < blocks; ++b)
                    p.process(silence, scratch, frames);
                p.stopProcessing();
            }
        }
    }).join();
}

void Engine::deactivate() {
    if (!activated_)
        return;
    stopWorkers();
    stopResetter_ = true;
    if (resetter_.joinable())
        resetter_.join();
    for (size_t i = 0; i < chains_.size(); ++i) {
        if (chains_[i].state != ChainState::Idle)
            chains_[i].chain->stopProcessing();
        chains_[i].state = ChainState::Idle;
        chains_[i].chain->deactivate();
    }
    activated_ = false;
}

bool Engine::nextBlockReport(BlockReport& report) {
    auto r = reports_.pop();
    if (!r)
        return false;
    report = *r;
    return true;
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

// An idle chain may start once its plug-ins are clean: then only the chain's
// own fades need resetting, which is cheap.
bool Engine::claimIdle(Slot& s) {
    uint8_t expected = kClean;
    if (!s.clean->compare_exchange_strong(expected, kInUse))
        return false;
    s.chain->resetRamps();
    return true;
}

// rangOut: its tail died away, so its plug-ins hold silence and need no
// reset; otherwise the reset helper clears them.
void Engine::stopChain(int index, bool rangOut) {
    Slot& s = chains_[index];
    s.chain->stopProcessing();
    s.inGain.set(0.0f);
    s.outGain.set(0.0f);
    setState(index, ChainState::Idle);
    s.clean->store(rangOut && s.chain->settled() ? kClean : kDirty);
    emit(EngineEvent::Type::ChainIdle, index);
}

void Engine::handle(const EngineCommand& cmd) {
    switch (cmd.type) {
    case EngineCommand::Type::SwitchTo: requestSwitch(cmd.chain); break;
    case EngineCommand::Type::SetWarm: setWarmTarget(cmd.chain); break;
    case EngineCommand::Type::SetBypass:
        if (cmd.chain >= 0 && cmd.chain < static_cast<int>(chains_.size())) {
            Slot& s = chains_[cmd.chain];
            const bool idle = s.state == ChainState::Idle;
            s.chain->setBypass(cmd.slot, cmd.on, idle ? 0 : ms(settings_.bypassFadeMs), idle);
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
    deferredSwitch_ = -1; // a newer request replaces one still waiting
    if (target == pending_)
        return;

    // A newer request replaces a pre-roll still in progress.
    if (pending_ >= 0) {
        const int cancelled = pending_;
        pending_ = -1;
        if (cancelled == warm_)
            setState(cancelled, ChainState::Warm);
        else
            stopChain(cancelled, false);
    }
    if (target == active_)
        return;

    Slot& s = chains_[target];
    switch (s.state) {
    case ChainState::Idle:
        if (!claimIdle(s)) {
            deferredSwitch_ = target; // starts when its reset is done
            return;
        }
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
            s.chain->holdTails(false, ms(settings_.crossfadeMs), ms(settings_.tailResetMinMs));
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
    warmWaiting_ = false;
    if (previous >= 0 && chains_[previous].state == ChainState::Warm)
        stopChain(previous, false);
    if (target < 0)
        return;
    Slot& s = chains_[target];
    if (s.state == ChainState::Idle) {
        if (!claimIdle(s)) {
            warmWaiting_ = true; // starts when its reset is done
            return;
        }
        startChain(s, ChainState::Warm);
        if (settings_.resetTailsOnWarmSwitch)
            s.chain->holdTails(true, 0, ms(settings_.tailResetMinMs));
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
            s.quietEnd = quiet;
            setState(index, ChainState::Stopping);
            s.outGain.linearTo(0.0f, ms(settings_.stopFadeMs));
        }
        break;
    }
    case ChainState::Stopping:
        if (s.outGain.done()) {
            if (index == warm_) {
                setState(index, ChainState::Warm);
                if (settings_.resetTailsOnWarmSwitch)
                    s.chain->holdTails(true, 0, ms(settings_.tailResetMinMs));
                s.inGain.dbTo(1.0f, ms(settings_.crossfadeMs));
            } else {
                stopChain(index, s.quietEnd);
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
    const auto blockStart = std::chrono::steady_clock::now();
    frames = std::min(frames, settings_.maxFrames);
    while (auto cmd = commands_.pop())
        handle(*cmd);
    // Starts that waited for a chain's reset.
    if (deferredSwitch_ >= 0 && chains_[deferredSwitch_].clean->load() == kClean)
        requestSwitch(deferredSwitch_);
    if (warmWaiting_ && warm_ >= 0 && chains_[warm_].clean->load() == kClean) {
        const int target = warm_;
        warm_ = -1;
        setWarmTarget(target);
    }

    for (size_t o = 0; o < settings_.numOutputs; ++o)
        std::memset(outs[o], 0, frames * sizeof(float));

    // Idle chains still receive parameter changes, every 5 ms.
    const bool flushIdle = (idleFlushCounter_ += frames) >= ms(5.0f);
    if (flushIdle)
        idleFlushCounter_ = 0;

    // The chains that play in this block, the one being heard first: the
    // audio thread starts on it while helpers take the others.
    size_t count = 0;
    if (active_ >= 0)
        jobs_[count++] = active_;
    for (size_t i = 0; i < chains_.size(); ++i) {
        chains_[i].ran = false;
        if (chains_[i].state != ChainState::Idle && static_cast<int>(i) != active_)
            jobs_[count++] = static_cast<int>(i);
    }
    BlockReport block;
    block.count = std::min(count, BlockReport::kMaxChains);
    for (size_t k = 0; k < block.count; ++k) {
        block.chains[k].chain = jobs_[k];
        block.chains[k].state = chains_[jobs_[k]].state;
    }
    runChains(in, frames, count, flushIdle);

    // Mixed and moved on in chain order, whichever thread processed them,
    // so the result is the same as processing them one by one.
    for (size_t i = 0; i < chains_.size(); ++i) {
        Slot& s = chains_[i];
        if (!s.ran)
            continue;
        // Output: muted chains (pre-roll, warm) are processed but not heard.
        float peak = 0.0f;
        const bool muted = s.outGain.done() && s.outGain.value() == 0.0f;
        if (muted) {
            for (size_t o = 0; o < settings_.numOutputs; ++o)
                for (uint32_t n = 0; n < frames; ++n)
                    peak = std::max(peak, std::fabs(s.outs[o][n]));
        } else {
            s.outGain.render(gainScratch_.data(), frames);
            for (size_t o = 0; o < settings_.numOutputs; ++o) {
                const float* src = s.outs[o];
                float* dst = outs[o];
                for (uint32_t n = 0; n < frames; ++n) {
                    peak = std::max(peak, std::fabs(src[n]));
                    dst[n] += src[n] * gainScratch_[n];
                }
            }
        }

        ChainStats& st = *s.stats;
        st.processNs.fetch_add(s.blockNs, std::memory_order_relaxed);
        st.processBlocks.fetch_add(1, std::memory_order_relaxed);
        if (s.blockNs > st.maxBlockNs.load(std::memory_order_relaxed))
            st.maxBlockNs.store(s.blockNs, std::memory_order_relaxed);

        advance(static_cast<int>(i), frames, peak);
    }

    if (!outputGain_.done() || outputGain_.value() != 1.0f) {
        outputGain_.render(gainScratch_.data(), frames);
        for (size_t o = 0; o < settings_.numOutputs; ++o)
            for (uint32_t n = 0; n < frames; ++n)
                outs[o][n] *= gainScratch_[n];
    }

    block.ns = nsSince(blockStart);
    if (block.ns > slowest_.ns) {
        for (size_t k = 0; k < block.count; ++k) {
            const Slot& s = chains_[block.chains[k].chain];
            block.chains[k].helper = s.helper;
            block.chains[k].ns = s.blockNs;
        }
        slowest_ = block;
    }
    if (reportWanted_.load(std::memory_order_relaxed) && reportWanted_.exchange(false, std::memory_order_relaxed)) {
        reports_.push(slowest_);
        slowest_ = BlockReport{};
    }
}

void Engine::runChains(const float* in, uint32_t frames, size_t count, bool flushIdle) {
    jobIn_ = in;
    jobFrames_ = frames;
    const bool parallel = !workers_.empty() && count > 1;
    if (parallel) {
        jobsDone_.store(0, std::memory_order_relaxed);
        const uint32_t generation = ++generation_;
        ticket_.store(static_cast<uint64_t>(generation) << 32 | static_cast<uint64_t>(count) << 16, std::memory_order_release);
        wake_.store(generation);
        const uint32_t sleeping = sleepers_.load();
        if (sleeping > 0)
            futexWake(wake_, static_cast<int>(std::min<size_t>(sleeping, count - 1)));
        takeJobs(generation);
    } else {
        for (size_t k = 0; k < count; ++k)
            runChain(jobs_[k], false);
    }

    // Idle chains are nobody's job: the audio thread sees to them while
    // the helpers finish.
    if (flushIdle) {
        for (auto& s : chains_) {
            // Not while the reset helper has it.
            if (s.state == ChainState::Idle && s.clean->load() == kClean)
                s.chain->flushIdle();
        }
    }

    if (parallel) {
        while (jobsDone_.load(std::memory_order_acquire) < count)
            _mm_pause();
    }
}

void Engine::runChain(int index, bool helper) {
    Slot& s = chains_[index];
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t frames = jobFrames_;
    // Input, with the chain's input gain.
    if (s.inGain.done() && s.inGain.value() == 1.0f) {
        std::memcpy(s.in, jobIn_, frames * sizeof(float));
    } else {
        s.inGain.render(s.gain, frames);
        for (uint32_t n = 0; n < frames; ++n)
            s.in[n] = jobIn_[n] * s.gain[n];
    }
    s.chain->process(s.in, s.outs.data(), frames);
    s.ran = true;
    s.helper = helper;
    s.blockNs = nsSince(t0);
}

} // namespace cue
