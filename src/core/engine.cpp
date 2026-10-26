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
        if (chains_[i].chain->numOutputs() != settings_.numOutputs || chains_[i].chain->input() >= std::max<size_t>(settings_.numInputs, 1) ||
            !chains_[i].chain->activate(settings_.sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j)
                chains_[j].chain->deactivate();
            return false;
        }
    }
    gainScratch_.assign(maxFrames, 0.0f);
    for (auto& s : chains_) {
        s.buffers.assign((3 + settings_.numOutputs) * static_cast<size_t>(maxFrames), 0.0f);
        s.in = s.buffers.data();
        s.gain = s.in + maxFrames;
        s.editScratch = s.gain + maxFrames;
        s.outs.resize(settings_.numOutputs);
        for (size_t o = 0; o < settings_.numOutputs; ++o)
            s.outs[o] = s.editScratch + (1 + o) * static_cast<size_t>(maxFrames);
    }
    jobs_.assign(chains_.size(), -1);
    const size_t inputs = std::max<size_t>(settings_.numInputs, 1);
    jobIn_.assign((inputs + 1) * static_cast<size_t>(maxFrames), 0.0f); // the last stays silent
    oneInput_.assign(inputs, jobIn_.data() + inputs * static_cast<size_t>(maxFrames));
    heardJobs_ = backgroundJobs_ = 0;
    shared_ = false;
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
        takeJobs(generation, UINT32_MAX);
    }
}

void Engine::stopWorkers() {
    if (workers_.empty())
        return;
    // No more claims; a background chain a helper is still processing
    // finishes before the helper stops.
    ticket_.store(static_cast<uint64_t>(++generation_) << 32);
    backgroundJobs_ = 0;
    stopWorkers_ = true;
    wake_.fetch_add(1);
    futexWake(wake_, INT_MAX);
    for (auto& w : workers_)
        w.join();
    workers_.clear();
}

// Claims this block's chains one at a time, until none is left below limit.
// A claim only succeeds while the block is still the one in progress, and
// the block's lists stay as they are until every chain claimed is done.
void Engine::takeJobs(uint32_t generation, uint32_t limit) {
    uint64_t t = ticket_.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = t & 0xffff, count = (t >> 16) & 0xffff;
        if (static_cast<uint32_t>(t >> 32) != generation || next >= std::min(count, limit))
            return;
        if (!ticket_.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        runChain(jobs_[next], t_helper);
        (next < heardJobs_ ? heardDone_ : backgroundDone_).fetch_add(1, std::memory_order_release);
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
    // Every plug-in, bypassed ones too: they may be switched on mid-song.
    std::vector<Processor*> all;
    for (auto& s : chains_) {
        if (s.state != ChainState::Idle)
            continue;
        for (size_t i = 0; i < s.chain->numSlots(); ++i)
            all.push_back(&s.chain->slot(i));
    }
    primeProcessors(all, milliseconds);
}

void Engine::primeProcessors(const std::vector<Processor*>& processors, float milliseconds) {
    if (processors.empty())
        return;
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
        for (Processor* p : processors) {
            p->startProcessing();
            for (size_t b = 0; b < blocks; ++b)
                p->process(silence, scratch, frames);
            for (size_t b = 0; b < blocks; ++b)
                p->process(noise, scratch, frames);
            // Clear what the noise left, then let anything a reset
            // misses die away on silence.
            p->reset();
            for (size_t b = 0; b < blocks; ++b)
                p->process(silence, scratch, frames);
            p->stopProcessing();
        }
    }).join();
}

bool Engine::prepareEdit(int chain, ChainEdit& edit, PreparedEdit& out, std::string* error) {
    if (chain < 0 || chain >= static_cast<int>(chains_.size())) {
        *error = "there is no chain number " + std::to_string(chain + 1);
        return false;
    }
    if (!activated_) {
        *error = "the engine is not running";
        return false;
    }
    Chain::PlanPtr plan = chains_[chain].chain->prepare(edit, error);
    if (!plan)
        return false;
    primeProcessors(Chain::added(*plan), 100.0f);
    out.chain = chain;
    out.plan = std::move(plan);
    return true;
}

bool Engine::applyEdit(PreparedEdit& edit, std::string* error) {
    if (!edit.plan || edit.chain < 0 || edit.chain >= static_cast<int>(chains_.size())) {
        *error = "nothing to change";
        return false;
    }
    Chain::Plan* plan = edit.plan.get();
    EngineCommand cmd{EngineCommand::Type::Edit, edit.chain};
    cmd.plan = plan;
    bool posted = post(cmd);
    for (int tries = 0; !posted && tries < 500; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        posted = post(cmd);
    }
    if (!posted) {
        *error = "the engine is not taking changes";
        return false;
    }
    // The audio thread has it now; it only reads what commit copies.
    edit.plan.release();
    chains_[edit.chain].chain->commit(*plan);
    return true;
}

bool Engine::edit(int chain, ChainEdit edit, std::string* error) {
    PreparedEdit prepared;
    return prepareEdit(chain, edit, prepared, error) && applyEdit(prepared, error);
}

bool Engine::setWire(int chain, Port from, Port to, WireGains gains) {
    if (chain < 0 || chain >= static_cast<int>(chains_.size()))
        return false;
    const uint32_t id = chains_[chain].chain->setGains(from, to, gains);
    if (id == 0)
        return false;
    if (activated_)
        post({EngineCommand::Type::SetWire, chain, id, false, gains});
    return true;
}

void Engine::collect() {
    while (auto plan = retired_.pop())
        Chain::PlanDeleter()(*plan);
}

// The audio thread never frees: what it is done with goes to the main thread.
void Engine::retire(Chain::Plan* plan) {
    if (plan)
        retired_.push(plan); // with 256 waiting the main thread has stopped collecting, and this one is never freed
}

void Engine::deactivate() {
    if (!activated_)
        return;
    stopWorkers();
    stopResetter_ = true;
    if (resetter_.joinable())
        resetter_.join();
    collect();
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
    // A chain that changes state is processed again, until it goes quiet anew.
    chains_[index].resting = false;
    chains_[index].mutedQuietFrames = 0;
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
    // A change waiting on its fade is swapped in at once, now that it is idle.
    s.editing = false;
    s.editGain.set(1.0f);
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
    case EngineCommand::Type::Edit:
        if (cmd.chain >= 0 && cmd.chain < static_cast<int>(chains_.size())) {
            Slot& s = chains_[cmd.chain];
            retire(s.chain->receive(cmd.plan));
            if (s.editing)
                s.chain->prepareSwap(ms(settings_.editFadeMs));
        } else {
            retire(cmd.plan);
        }
        break;
    case EngineCommand::Type::SetWire:
        if (cmd.chain >= 0 && cmd.chain < static_cast<int>(chains_.size()))
            chains_[cmd.chain].chain->setWireGains(cmd.slot, cmd.gains, ms(settings_.bypassFadeMs));
        break;
    case EngineCommand::Type::SetMute:
        if (cmd.on != muted_) {
            muted_ = cmd.on;
            inputGain_.linearTo(muted_ ? 0.0f : 1.0f, ms(settings_.muteFadeMs));
            if (!muted_)
                for (auto& s : chains_) {
                    s.resting = false;
                    s.mutedQuietFrames = 0;
                }
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

// Changes to chains' wiring. An idle chain takes one at once (unless the
// reset helper has it); a playing one fades its output out, swaps and fades
// back in.
void Engine::applyPlans() {
    const uint32_t fade = ms(settings_.editFadeMs);
    for (auto& s : chains_) {
        if (!s.chain->hasPending())
            continue;
        if (s.state == ChainState::Idle) {
            uint8_t c = s.clean->load();
            if ((c != kClean && c != kDirty) || !s.clean->compare_exchange_strong(c, kInUse))
                continue;
            bool stale = c == kDirty;
            retire(s.chain->swap(false, 0, &stale));
            s.clean->store(stale ? kDirty : kClean);
            continue;
        }
        if (s.resting) {
            retire(s.chain->swap(true, fade));
            s.editing = false;
            s.editGain.set(1.0f);
            continue;
        }
        if (!s.editing) {
            s.editing = true;
            s.editGain.linearTo(0.0f, fade);
            s.chain->prepareSwap(fade);
        }
        if (s.editGain.done() && s.editGain.value() == 0.0f) {
            retire(s.chain->swap(true, fade));
            s.editing = false;
            s.editGain.linearTo(1.0f, fade);
        }
    }
}

void Engine::process(const float* in, float* const* outs, uint32_t frames) {
    oneInput_[0] = in;
    process(oneInput_.data(), outs, frames);
}

void Engine::process(const float* const* ins, float* const* outs, uint32_t frames) {
    const auto blockStart = std::chrono::steady_clock::now();
    frames = std::min(frames, settings_.maxFrames);
    // The last block's background chains, before anything can touch them.
    finishBackground();
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
    applyPlans();

    for (size_t o = 0; o < settings_.numOutputs; ++o)
        std::memset(outs[o], 0, frames * sizeof(float));

    // Idle chains still receive parameter changes, every 5 ms.
    const bool flushIdle = (idleFlushCounter_ += frames) >= ms(5.0f);
    if (flushIdle)
        idleFlushCounter_ = 0;

    // The chains that play in this block. Heard ones first, the one being
    // played first of all: the audio thread starts on it while helpers take
    // the others. Then the ones nobody hears yet (warm, pre-roll): their
    // output is not used, so the block does not wait for them.
    auto background = [](const Slot& s) {
        return (s.state == ChainState::PreRoll || s.state == ChainState::Warm) && s.outGain.done() && s.outGain.value() == 0.0f;
    };
    for (auto& s : chains_)
        s.ran = s.background = false;
    size_t heard = 0, unheard = 0;
    if (active_ >= 0 && !chains_[active_].resting) {
        jobs_[heard++] = active_;
        chains_[active_].ran = true;
    }
    for (size_t i = 0; i < chains_.size(); ++i) {
        Slot& s = chains_[i];
        if (s.state != ChainState::Idle && !s.resting && static_cast<int>(i) != active_ && !background(s)) {
            jobs_[heard++] = static_cast<int>(i);
            s.ran = true;
        }
    }
    for (size_t i = 0; i < chains_.size(); ++i) {
        Slot& s = chains_[i];
        if (s.state != ChainState::Idle && !s.resting && !s.ran) {
            jobs_[heard + unheard++] = static_cast<int>(i);
            s.ran = s.background = true;
        }
    }
    BlockReport block;
    block.count = std::min(heard + unheard, BlockReport::kMaxChains);
    for (size_t k = 0; k < block.count; ++k) {
        const Slot& s = chains_[jobs_[k]];
        block.chains[k].chain = jobs_[k];
        block.chains[k].state = s.state;
        block.chains[k].background = s.background;
        block.chains[k].ns = s.backgroundNs;
    }
    runChains(ins, frames, heard, unheard, flushIdle);

    // Mixed and moved on in chain order, whichever thread processed them,
    // so the result is the same as processing them one by one. Background
    // chains move on at the start of the next block, once they are done.
    for (size_t i = 0; i < chains_.size(); ++i) {
        Slot& s = chains_[i];
        if (!s.ran || s.background)
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

        recordStats(s);
        advance(static_cast<int>(i), frames, peak);
        restWhenQuiet(s, frames, peak);
    }

    block.ns = nsSince(blockStart);
    if (block.ns > slowest_.ns) {
        for (size_t k = 0; k < block.count; ++k) {
            const Slot& s = chains_[block.chains[k].chain];
            if (s.background)
                continue; // a helper may still be on it
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

void Engine::runChains(const float* const* ins, uint32_t frames, size_t heard, size_t background, bool flushIdle) {
    // A copy of the inputs: background chains may read them after process()
    // has returned and the caller has reused its buffers.
    // Muted, they hear silence (with a fade), so tails ring out.
    const bool inputsLive = inputGain_.done() && inputGain_.value() == 1.0f;
    if (!inputsLive)
        inputGain_.render(gainScratch_.data(), frames);
    for (size_t i = 0; i < std::max<size_t>(settings_.numInputs, 1); ++i) {
        float* dst = jobIn_.data() + i * static_cast<size_t>(settings_.maxFrames);
        if (inputsLive) {
            std::memcpy(dst, ins[i], frames * sizeof(float));
        } else {
            for (uint32_t n = 0; n < frames; ++n)
                dst[n] = ins[i][n] * gainScratch_[n];
        }
    }
    jobFrames_ = frames;
    heardJobs_ = heard;
    backgroundJobs_ = background;
    shared_ = !workers_.empty() && heard + background > 1;
    if (shared_) {
        heardDone_.store(0, std::memory_order_relaxed);
        backgroundDone_.store(0, std::memory_order_relaxed);
        const uint32_t generation = ++generation_;
        ticket_.store(static_cast<uint64_t>(generation) << 32 | static_cast<uint64_t>(heard + background) << 16, std::memory_order_release);
        wake_.store(generation);
        const size_t wanted = heard + background - (heard > 0 ? 1 : 0);
        const uint32_t sleeping = sleepers_.load();
        if (sleeping > 0 && wanted > 0)
            futexWake(wake_, static_cast<int>(std::min<size_t>(sleeping, wanted)));
        // The audio thread takes heard chains only.
        takeJobs(generation, static_cast<uint32_t>(heard));
    } else {
        for (size_t k = 0; k < heard + background; ++k)
            runChain(jobs_[k], false);
    }

    // Idle and resting chains are nobody's job: the audio thread sees to
    // them while the helpers finish.
    if (flushIdle) {
        for (auto& s : chains_) {
            // Not while the reset helper has it.
            if ((s.state == ChainState::Idle && s.clean->load() == kClean) || s.resting)
                s.chain->flushIdle();
            // A playing chain's blocks that nothing reaches: a helper may be
            // processing the chain, but never these.
            else if (s.state != ChainState::Idle)
                s.chain->flushUnplayed();
        }
    }

    if (shared_) {
        while (heardDone_.load(std::memory_order_acquire) < heard)
            _mm_pause();
    }
}

// Waits for the last block's background chains, processing any no helper
// got to, then moves them on.
void Engine::finishBackground() {
    if (backgroundJobs_ == 0)
        return;
    if (shared_) {
        takeJobs(generation_, UINT32_MAX);
        while (backgroundDone_.load(std::memory_order_acquire) < backgroundJobs_)
            _mm_pause();
    }
    for (size_t k = heardJobs_; k < heardJobs_ + backgroundJobs_; ++k) {
        Slot& s = chains_[jobs_[k]];
        s.backgroundNs = s.blockNs;
        recordStats(s);
        advance(jobs_[k], jobFrames_, 0.0f); // pre-roll and warm need no peak
        restWhenQuiet(s, jobFrames_, 0.0f); // nobody hears a warm chain's tail
    }
    backgroundJobs_ = 0;
}

// While the inputs are muted, the chain played and the warm one stop being
// processed once they have been quiet for a while. A chain on its way in or
// out (pre-roll, releasing) goes on: it has somewhere to get to.
void Engine::restWhenQuiet(Slot& s, uint32_t frames, float peak) {
    const bool inputsSilent = muted_ && inputGain_.done() && inputGain_.value() == 0.0f;
    if (!inputsSilent || (s.state != ChainState::Active && s.state != ChainState::Warm))
        return;
    const float threshold = std::pow(10.0f, settings_.tailQuietDb / 20.0f);
    s.mutedQuietFrames = peak < threshold ? s.mutedQuietFrames + frames : 0;
    if (s.mutedQuietFrames >= ms(settings_.tailHoldMs))
        s.resting = true;
}

void Engine::recordStats(Slot& s) {
    ChainStats& st = *s.stats;
    st.processNs.fetch_add(s.blockNs, std::memory_order_relaxed);
    st.processBlocks.fetch_add(1, std::memory_order_relaxed);
    if (s.blockNs > st.maxBlockNs.load(std::memory_order_relaxed))
        st.maxBlockNs.store(s.blockNs, std::memory_order_relaxed);
}

void Engine::runChain(int index, bool helper) {
    Slot& s = chains_[index];
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t frames = jobFrames_;
    // Its input, with the chain's input gain.
    const float* in = jobIn_.data() + s.chain->input() * static_cast<size_t>(settings_.maxFrames);
    if (s.inGain.done() && s.inGain.value() == 1.0f) {
        std::memcpy(s.in, in, frames * sizeof(float));
    } else {
        s.inGain.render(s.gain, frames);
        for (uint32_t n = 0; n < frames; ++n)
            s.in[n] = in[n] * s.gain[n];
    }
    s.chain->process(s.in, s.outs.data(), frames);
    if (!s.editGain.done() || s.editGain.value() != 1.0f) {
        s.editGain.render(s.editScratch, frames);
        for (float* out : s.outs)
            for (uint32_t n = 0; n < frames; ++n)
                out[n] *= s.editScratch[n];
    }
    s.helper = helper;
    s.blockNs = nsSince(t0);
}

} // namespace cue
