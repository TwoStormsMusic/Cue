#include "chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace cue {

namespace {

std::vector<Line> oneLine(std::vector<std::unique_ptr<Processor>> slots) {
    std::vector<Line> lines(1);
    lines[0].slots = std::move(slots);
    return lines;
}

// A wire's gains as a matrix: L' = c[0] L + c[1] R, R' = c[2] L + c[3] R.
// Width first (the side signal scaled about the middle), then the balance.
void coefficients(const WireGains& g, float c[4]) {
    const float level = g.muted || g.levelDb <= kWireOffDb ? 0.0f : std::pow(10.0f, g.levelDb / 20.0f);
    const float w = std::clamp(g.width, 0.0f, 2.0f);
    const float pan = std::clamp(g.pan, -1.0f, 1.0f);
    const float left = level * (pan > 0 ? 1 - pan : 1), right = level * (pan < 0 ? 1 + pan : 1);
    c[0] = left * (1 + w) / 2;
    c[1] = left * (1 - w) / 2;
    c[2] = right * (1 - w) / 2;
    c[3] = right * (1 + w) / 2;
}

bool identity(const float c[4]) { return c[0] == 1.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f; }

uint64_t combine(uint64_t h, uint64_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }

} // namespace

// What the audio thread plays: the blocks in processing order, each with the
// wires into its input and its key, then the wires into each output.
struct Chain::Plan {
    struct LiveWire {
        uint32_t id = 0;
        const float* const* src = nullptr; // the chain's input, or a block's res
        float from[4] = {1, 0, 0, 1}, to[4] = {1, 0, 0, 1};
        Ramp t{1.0f};      // 0 = from, 1 = to
        bool plain = true; // to is the identity and t is done: the signal passes as it is
    };
    struct Step {
        Block* block = nullptr;
        uint32_t slot = 0;
        uint32_t wires = 0, numWires = 0; // into its input, in wires
        uint32_t keys = 0, numKeys = 0;   // into its key
        uint64_t inputs = 0;              // which wires those are
    };
    std::vector<std::shared_ptr<Block>> slots; // every block, played or not, by slot
    std::vector<Wire> spec;                    // the wires as given
    std::vector<Step> order;                   // the blocks played, each after what it hears
    std::vector<LiveWire> wires;
    std::vector<std::pair<uint32_t, uint32_t>> outs; // each output's wires: first, count
    std::vector<Processor*> added;                   // processors this plan brings in
};

void Chain::PlanDeleter::operator()(Plan* plan) const { delete plan; }

Chain::Block::~Block() {
    if (!activated)
        return;
    if (hasKey)
        processor->setSidechain(nullptr);
    processor->deactivate();
}

Chain::Chain(std::string name, size_t numOutputs, OutputMix mix) : name_(std::move(name)), mix_(numOutputs, mix) {}

Chain::Chain(std::string name, std::vector<std::unique_ptr<Processor>> slots, std::vector<OutputTap> taps)
    : Chain(std::move(name), oneLine(std::move(slots)), std::move(taps)) {}

Chain::Chain(std::string name, std::vector<Line> lines, std::vector<OutputTap> taps) : name_(std::move(name)) {
    if (lines.empty())
        lines.emplace_back();
    const size_t n = lines.size();
    std::vector<size_t> first(n), count(n), after(n);
    std::vector<int> parent(n);
    for (size_t i = 0; i < n; ++i) {
        // A branch must come off an earlier line.
        parent[i] = i == 0 ? -1 : (lines[i].parent >= 0 && lines[i].parent < static_cast<int>(i) ? lines[i].parent : 0);
        after[i] = i == 0 ? 0 : std::min(lines[i].afterSlots, count[parent[i]]);
        first[i] = blocks_.size();
        count[i] = lines[i].slots.size();
        for (auto& p : lines[i].slots)
            addBlock(std::move(p));
    }
    // The signal of a line after so many of its slots.
    std::function<Port(size_t, size_t)> at = [&](size_t l, size_t k) -> Port {
        if (k > 0)
            return Port::block(static_cast<int>(first[l] + k - 1));
        return l == 0 ? Port::input() : at(parent[l], after[l]);
    };
    for (size_t l = 0; l < n; ++l)
        for (size_t k = 0; k < count[l]; ++k)
            connect(at(l, k), Port::block(static_cast<int>(first[l] + k)));
    for (size_t t = 0; t < taps.size(); ++t) {
        mix_.push_back(taps[t].mix);
        const size_t line = taps[t].line >= 0 && static_cast<size_t>(taps[t].line) < n ? static_cast<size_t>(taps[t].line) : 0;
        connect(at(line, std::min(taps[t].afterSlots, count[line])), Port::output(static_cast<int>(t)));
    }
}

Chain::~Chain() { deactivate(); }

size_t Chain::addBlock(std::unique_ptr<Processor> processor) {
    auto b = std::make_shared<Block>();
    b->processor = std::move(processor);
    b->hasKey = b->processor->hasSidechain();
    blocks_.push_back(std::move(b));
    return blocks_.size() - 1;
}

bool Chain::checkWires(const std::vector<Wire>& wires, const std::vector<bool>& keys, std::string* error) const {
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return false;
    };
    const int n = static_cast<int>(keys.size()), outputs = static_cast<int>(mix_.size());
    for (size_t i = 0; i < wires.size(); ++i) {
        const Wire& w = wires[i];
        if (w.from.kind == Port::Kind::Output)
            return fail("a wire cannot start at an output");
        if (w.to.kind == Port::Kind::Input)
            return fail("a wire cannot end at the input");
        if (w.from.kind == Port::Kind::Block && (w.from.index < 0 || w.from.index >= n || w.from.key))
            return fail("a wire starts at a plug-in the chain does not have");
        if (w.to.kind == Port::Kind::Block && (w.to.index < 0 || w.to.index >= n))
            return fail("a wire ends at a plug-in the chain does not have");
        if (w.to.kind == Port::Kind::Block && w.to.key && !keys[w.to.index])
            return fail("that plug-in has no side-chain input");
        if (w.to.kind == Port::Kind::Output && (w.to.index < 0 || w.to.index >= outputs || w.to.key))
            return fail("a wire ends at an output the performance does not have");
        if (w.from.kind == Port::Kind::Block && w.to.kind == Port::Kind::Block && w.from.index == w.to.index)
            return fail("a plug-in cannot feed itself");
        for (size_t k = 0; k < i; ++k)
            if (wires[k].from == w.from && wires[k].to == w.to)
                return fail("those two are already wired");
    }
    // No loops: a block's output never comes back to it, by any path.
    std::vector<uint8_t> state(n, 0); // 0 unseen, 1 on the path, 2 done
    std::function<bool(int)> visit = [&](int b) {
        state[b] = 1;
        for (const Wire& w : wires) {
            if (w.from.kind != Port::Kind::Block || w.from.index != b || w.to.kind != Port::Kind::Block)
                continue;
            if (state[w.to.index] == 1 || (state[w.to.index] == 0 && !visit(w.to.index)))
                return false;
        }
        state[b] = 2;
        return true;
    };
    for (int b = 0; b < n; ++b)
        if (state[b] == 0 && !visit(b))
            return fail("that would make a loop: a plug-in's signal would come back to it");
    return true;
}

bool Chain::connect(Port from, Port to, WireGains gains, std::string* error) {
    std::vector<Wire> wires = wires_;
    wires.push_back({from, to, gains, 0});
    std::vector<bool> keys;
    for (const auto& b : blocks_)
        keys.push_back(b->hasKey);
    if (!checkWires(wires, keys, error))
        return false;
    wires.back().id = nextWire_++;
    wires_ = std::move(wires);
    return true;
}

bool Chain::setSidechain(size_t slot, size_t source) {
    return slot < blocks_.size() && source < blocks_.size() &&
           connect(Port::block(static_cast<int>(source)), Port::block(static_cast<int>(slot), true));
}

uint32_t Chain::setGains(Port from, Port to, WireGains gains) {
    for (Wire& w : wires_)
        if (w.from == from && w.to == to) {
            w.gains = gains;
            return w.id;
        }
    return 0;
}

void Chain::setInitialBypass(size_t slot, bool bypassed) {
    Block& b = *blocks_[slot];
    b.bypassed = bypassed;
    b.wet.set(bypassed ? 0.0f : 1.0f);
}

void Chain::setDrain(float quietLevel, uint32_t holdFrames, uint32_t capFrames) {
    quietLevel_ = quietLevel;
    drainHold_ = holdFrames;
    drainCap_ = std::max(capFrames, holdFrames);
}

// A block's own buffers: its summed inputs, its output, its key.
void Chain::allocate(Block& b) {
    const size_t frames = maxFrames_;
    b.storage.assign(frames * kChainChannels * (b.hasKey ? 3 : 2), 0.0f);
    float* p = b.storage.data();
    for (int c = 0; c < kChainChannels; ++c, p += frames)
        b.in[c] = p;
    for (int c = 0; c < kChainChannels; ++c, p += frames)
        b.out[c] = p;
    for (int c = 0; c < kChainChannels; ++c) {
        b.key[c] = b.hasKey ? p : nullptr;
        if (b.hasKey)
            p += frames;
    }
    for (int c = 0; c < kChainChannels; ++c)
        b.res[c] = silence_[c];
    // A key with no wire into it is silence, as it would be without one.
    if (b.hasKey)
        b.processor->setSidechain(b.key);
}

bool Chain::activate(double sampleRate, uint32_t maxFrames) {
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    // The input, a scratch pair, the outputs' sums and silence.
    storage_.assign(static_cast<size_t>(maxFrames) * kChainChannels * 4, 0.0f);
    float* p = storage_.data();
    for (float** pair : {inBuf_, tmp_, mixBuf_, silence_})
        for (int c = 0; c < kChainChannels; ++c, p += maxFrames)
            pair[c] = p;
    gainScratch_.assign(maxFrames, 0.0f);
    wetScratch_.assign(maxFrames, 0.0f);
    rampScratch_.assign(maxFrames, 0.0f);
    for (size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = *blocks_[i];
        if (!b.processor->activate(sampleRate, maxFrames)) {
            for (size_t j = 0; j < i; ++j) {
                if (blocks_[j]->hasKey)
                    blocks_[j]->processor->setSidechain(nullptr);
                blocks_[j]->processor->deactivate();
                blocks_[j]->activated = false;
            }
            return false;
        }
        b.activated = true;
        allocate(b);
    }
    plan_ = makePlan(blocks_, wires_).release();
    for (const auto& st : plan_->order)
        st.block->inputs = st.inputs;
    active_ = true;
    return true;
}

void Chain::deactivate() {
    if (!active_)
        return;
    PlanDeleter()(pending_);
    PlanDeleter()(plan_);
    pending_ = plan_ = nullptr;
    for (auto& b : blocks_) {
        if (!b->activated)
            continue;
        if (b->hasKey)
            b->processor->setSidechain(nullptr);
        b->processor->deactivate();
        b->activated = false;
        b->processing = false;
    }
    active_ = false;
}

Chain::PlanPtr Chain::makePlan(std::vector<std::shared_ptr<Block>> blocks, std::vector<Wire> wires) {
    PlanPtr plan(new Plan);
    plan->slots = std::move(blocks);
    plan->spec = std::move(wires);
    const std::vector<Wire>& w = plan->spec;
    const size_t n = plan->slots.size();
    using K = Port::Kind;
    // Played: a block the input reaches, by way of blocks' inputs, and that
    // an output hears, directly or through a key.
    std::vector<char> fed(n, 0), heard(n, 0), played(n, 0);
    for (bool more = true; more;) {
        more = false;
        for (const Wire& x : w)
            if (x.to.kind == K::Block && !x.to.key && !fed[x.to.index] && (x.from.kind == K::Input || (x.from.kind == K::Block && fed[x.from.index])))
                fed[x.to.index] = more = true;
    }
    for (bool more = true; more;) {
        more = false;
        for (const Wire& x : w)
            if (x.from.kind == K::Block && !heard[x.from.index] && (x.to.kind == K::Output || (x.to.kind == K::Block && heard[x.to.index])))
                heard[x.from.index] = more = true;
    }
    for (size_t i = 0; i < n; ++i)
        played[i] = fed[i] && heard[i];
    auto live = [&](const Wire& x) {
        return (x.from.kind == K::Input || played[x.from.index]) && (x.to.kind == K::Output || (x.to.kind == K::Block && played[x.to.index]));
    };
    auto add = [&](const Wire& x) {
        Plan::LiveWire lw;
        lw.id = x.id;
        if (x.from.kind == K::Input)
            lw.src = inBuf_;
        else
            lw.src = plan->slots[x.from.index]->res;
        coefficients(x.gains, lw.to);
        std::copy(lw.to, lw.to + 4, lw.from);
        lw.plain = identity(lw.to);
        plan->wires.push_back(lw);
    };
    // Each block once everything it hears is done, the lowest slot first:
    // a chain made of lines plays line 0, then each branch.
    std::vector<int> waiting(n, 0);
    for (const Wire& x : w)
        if (live(x) && x.from.kind == K::Block && x.to.kind == K::Block)
            ++waiting[x.to.index];
    std::vector<char> done(n, 0);
    for (;;) {
        size_t next = n;
        for (size_t i = 0; i < n && next == n; ++i)
            if (played[i] && !done[i] && waiting[i] == 0)
                next = i;
        if (next == n)
            break; // the wires were checked for loops, so every block played is here
        done[next] = 1;
        for (const Wire& x : w)
            if (live(x) && x.from.kind == K::Block && x.from.index == static_cast<int>(next) && x.to.kind == K::Block)
                --waiting[x.to.index];
        Plan::Step st;
        st.block = plan->slots[next].get();
        st.slot = static_cast<uint32_t>(next);
        uint64_t h = 1469598103934665603ull;
        st.wires = static_cast<uint32_t>(plan->wires.size());
        for (const Wire& x : w)
            if (live(x) && x.to == Port::block(static_cast<int>(next))) {
                add(x);
                h = combine(h, x.id);
            }
        st.numWires = static_cast<uint32_t>(plan->wires.size()) - st.wires;
        st.keys = static_cast<uint32_t>(plan->wires.size());
        for (const Wire& x : w)
            if (live(x) && x.to == Port::block(static_cast<int>(next), true)) {
                add(x);
                h = combine(h, x.id | 1ull << 32);
            }
        st.numKeys = static_cast<uint32_t>(plan->wires.size()) - st.keys;
        st.inputs = h;
        plan->order.push_back(st);
    }
    for (size_t o = 0; o < mix_.size(); ++o) {
        const auto first = static_cast<uint32_t>(plan->wires.size());
        for (const Wire& x : w)
            if (live(x) && x.to == Port::output(static_cast<int>(o)))
                add(x);
        plan->outs.push_back({first, static_cast<uint32_t>(plan->wires.size()) - first});
    }
    return plan;
}

Chain::PlanPtr Chain::prepare(ChainEdit& edit, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return PlanPtr();
    };
    if (!active_)
        return fail("the chain is not active");
    std::vector<std::shared_ptr<Block>> blocks;
    std::vector<bool> keys;
    std::vector<Processor*> added;
    std::vector<int> renumbered(blocks_.size(), -1); // a slot now -> its slot after the edit
    size_t next = 0;
    for (size_t i = 0; i < edit.slots.size(); ++i) {
        const int s = edit.slots[i];
        std::shared_ptr<Block> b;
        if (s >= 0) {
            if (s >= static_cast<int>(blocks_.size()) || renumbered[s] >= 0)
                return fail("an edit names a plug-in that is not there, or one twice");
            b = blocks_[s];
            renumbered[s] = static_cast<int>(i);
        } else {
            if (next >= edit.added.size() || !edit.added[next].processor)
                return fail("an edit adds more plug-ins than it brings");
            ChainEdit::Added& a = edit.added[next++];
            b = std::make_shared<Block>();
            b->processor = std::move(a.processor);
            b->hasKey = b->processor->hasSidechain();
            b->activated = true;
            b->bypassed = a.bypassed;
            b->wet.set(a.bypassed ? 0.0f : 1.0f);
            b->muteOnBypass = a.muteOnBypass;
            allocate(*b);
            added.push_back(b->processor.get());
        }
        keys.push_back(b->hasKey);
        blocks.push_back(std::move(b));
    }
    if (!checkWires(edit.wires, keys, error))
        return PlanPtr();
    // A wire between the same two ports as before is the same wire.
    std::vector<Wire> wires = edit.wires;
    auto after = [&](Port p) {
        if (p.kind == Port::Kind::Block)
            p.index = p.index >= 0 && p.index < static_cast<int>(renumbered.size()) ? renumbered[p.index] : -1;
        return p;
    };
    for (Wire& w : wires) {
        w.id = 0;
        for (const Wire& o : wires_)
            if (after(o.from) == w.from && after(o.to) == w.to) {
                w.id = o.id;
                break;
            }
        if (w.id == 0)
            w.id = nextWire_++;
    }
    PlanPtr plan = makePlan(std::move(blocks), std::move(wires));
    plan->added = std::move(added);
    return plan;
}

void Chain::commit(const Plan& plan) {
    blocks_ = plan.slots;
    wires_ = plan.spec;
}

std::vector<Processor*> Chain::added(const Plan& plan) { return plan.added; }

uint32_t Chain::tailSamples() const {
    uint32_t tail = 0;
    if (!plan_)
        return 0;
    for (const auto& st : plan_->order) {
        if (st.block->bypassed)
            continue;
        const uint32_t t = st.block->processor->tailSamples();
        if (t == kInfiniteTail)
            return kInfiniteTail;
        tail = std::max(tail, t);
    }
    return tail;
}

void Chain::startProcessing() {
    for (const auto& st : plan_->order)
        if (!st.block->processing) {
            st.block->processor->startProcessing();
            st.block->processing = true;
        }
}

void Chain::stopProcessing() {
    for (const auto& st : plan_->order)
        if (st.block->processing) {
            st.block->processor->stopProcessing();
            st.block->processing = false;
        }
}

void Chain::reset() {
    resetProcessors();
    resetRamps();
}

void Chain::resetProcessors() {
    for (const auto& st : plan_->order) {
        st.block->processor->reset();
        st.block->stale = false;
    }
}

void Chain::resetRamps() {
    holdTails_ = false;
    for (const auto& b : plan_->slots) {
        b->inGain.set(1.0f);
        b->wet.set(b->bypassed ? 0.0f : 1.0f);
        b->draining = b->needsReset = false;
    }
    for (auto& w : plan_->wires) {
        w.t.set(1.0f);
        std::copy(w.to, w.to + 4, w.from);
        w.plain = identity(w.to);
    }
}

bool Chain::settled() const {
    for (const auto& st : plan_->order)
        if (st.block->draining || st.block->needsReset)
            return false;
    return true;
}

void Chain::holdTails(bool hold, uint32_t fadeFrames, uint32_t minTail) {
    holdTails_ = hold;
    tailMin_ = minTail;
    for (const auto& st : plan_->order) {
        Block& b = *st.block;
        if (b.bypassed || b.processor->tailSamples() < minTail)
            continue;
        if (hold)
            b.inGain.set(0.0f);
        else
            b.inGain.dbTo(1.0f, fadeFrames);
    }
}

void Chain::setBypass(size_t slot, bool bypassed, uint32_t fadeFrames, bool idle) {
    Plan* p = latest();
    if (!p || slot >= p->slots.size())
        return;
    Block& b = *p->slots[slot];
    if (b.bypassed == bypassed)
        return;
    b.bypassed = bypassed;
    if (idle || !b.processing) {
        // Not processed now; it starts from a reset plug-in.
        b.wet.set(bypassed ? 0.0f : 1.0f);
        return;
    }
    if (bypassed) {
        b.wet.linearTo(0.0f, fadeFrames);
        b.draining = true;
        b.quietFrames = b.drainFrames = 0;
        return;
    }
    if (b.wet.done() && b.wet.value() == 0.0f) {
        // It rang out on silence, so it starts clean; only one that never
        // went quiet (a delay held at full feedback) needs a reset.
        if (b.needsReset)
            b.processor->reset();
        b.draining = b.needsReset = false;
        fadeIn(b, fadeFrames);
    }
    b.wet.linearTo(1.0f, fadeFrames);
}

void Chain::setWireGains(uint32_t wire, WireGains gains, uint32_t fadeFrames) {
    float to[4];
    coefficients(gains, to);
    for (Plan* p : {plan_, pending_}) {
        if (!p)
            continue;
        for (auto& w : p->wires) {
            if (w.id != wire)
                continue;
            // From wherever it is now.
            const float k = w.t.value();
            for (int i = 0; i < 4; ++i)
                w.from[i] += k * (w.to[i] - w.from[i]);
            std::copy(to, to + 4, w.to);
            w.t.set(0.0f);
            w.t.linearTo(1.0f, fadeFrames);
            if (w.t.done())
                std::copy(to, to + 4, w.from);
            w.plain = w.t.done() && identity(w.to);
        }
    }
}

void Chain::flushIdle() {
    for (const auto& b : plan_->slots)
        b->processor->flushIdle();
}

void Chain::flushUnplayed() {
    for (const auto& b : plan_->slots)
        if (!b->processing)
            b->processor->flushIdle();
}

void Chain::writeTap(OutputMix mix, const float* const* signal, float* out, uint32_t frames) const {
    switch (mix) {
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

// The wires into one input, summed into dst. A single wire that passes its
// signal as it is needs no copy, unless into asks for one: then the signal
// is returned where it already is. nullptr: no wires, so silence.
const float* const* Chain::gather(Plan& plan, uint32_t first, uint32_t count, float* const* dst, uint32_t frames, bool into) {
    if (count == 0)
        return nullptr;
    Plan::LiveWire* w = plan.wires.data() + first;
    if (count == 1 && w->plain && !into)
        return w->src;
    for (uint32_t i = 0; i < count; ++i) {
        Plan::LiveWire& x = w[i];
        const float* l = x.src[0];
        const float* r = x.src[1];
        const bool sum = i > 0;
        if (x.plain) {
            for (int c = 0; c < kChainChannels; ++c) {
                if (!sum) {
                    std::memcpy(dst[c], x.src[c], frames * sizeof(float));
                } else {
                    for (uint32_t n = 0; n < frames; ++n)
                        dst[c][n] += x.src[c][n];
                }
            }
        } else if (x.t.done()) {
            const float a = x.to[0], b = x.to[1], c = x.to[2], d = x.to[3];
            for (uint32_t n = 0; n < frames; ++n) {
                const float L = a * l[n] + b * r[n], R = c * l[n] + d * r[n];
                dst[0][n] = sum ? dst[0][n] + L : L;
                dst[1][n] = sum ? dst[1][n] + R : R;
            }
        } else {
            x.t.render(rampScratch_.data(), frames);
            float k[4];
            for (uint32_t n = 0; n < frames; ++n) {
                const float t = rampScratch_[n];
                for (int j = 0; j < 4; ++j)
                    k[j] = x.from[j] + t * (x.to[j] - x.from[j]);
                const float L = k[0] * l[n] + k[1] * r[n], R = k[2] * l[n] + k[3] * r[n];
                dst[0][n] = sum ? dst[0][n] + L : L;
                dst[1][n] = sum ? dst[1][n] + R : R;
            }
            if (x.t.done()) {
                std::copy(x.to, x.to + 4, x.from);
                x.plain = identity(x.to);
            }
        }
    }
    return dst;
}

void Chain::processBlock(Block& b, const float* const* input, uint32_t frames) {
    const bool fullyBypassed = b.wet.done() && b.wet.value() == 0.0f;
    if (fullyBypassed) {
        if (b.draining)
            drain(b, frames);
        // Thru: the signal passes untouched. Mute: nothing passes.
        for (int c = 0; c < kChainChannels; ++c)
            b.res[c] = b.muteOnBypass ? silence_[c] : input[c];
        return;
    }
    const bool fading = !b.wet.done() || b.wet.value() != 1.0f;

    // The plug-in's input, faded in after a reset or an edit. The dry
    // signal for a bypass crossfade stays at full level.
    const float* const* in = input;
    if (!b.inGain.done() || b.inGain.value() != 1.0f) {
        b.inGain.render(gainScratch_.data(), frames);
        for (int c = 0; c < kChainChannels; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                tmp_[c][n] = input[c][n] * gainScratch_[n];
        in = tmp_;
    }
    b.processor->process(in, b.out, frames);
    if (fading) {
        b.wet.render(wetScratch_.data(), frames);
        if (b.muteOnBypass) {
            for (int c = 0; c < kChainChannels; ++c)
                for (uint32_t n = 0; n < frames; ++n)
                    b.out[c][n] *= wetScratch_[n];
        } else {
            for (int c = 0; c < kChainChannels; ++c)
                for (uint32_t n = 0; n < frames; ++n)
                    b.out[c][n] = input[c][n] + wetScratch_[n] * (b.out[c][n] - input[c][n]);
        }
    }
    for (int c = 0; c < kChainChannels; ++c)
        b.res[c] = b.out[c];
}

// A bypassed block, unheard, processing silence until its output dies away.
void Chain::drain(Block& b, uint32_t frames) {
    b.processor->process(silence_, b.out, frames);
    float peak = 0.0f;
    for (int c = 0; c < kChainChannels; ++c)
        for (uint32_t n = 0; n < frames; ++n)
            peak = std::max(peak, std::fabs(b.out[c][n]));
    b.quietFrames = peak < quietLevel_ ? b.quietFrames + frames : 0;
    b.drainFrames += frames;
    if (b.quietFrames >= drainHold_) {
        b.draining = false;
    } else if (b.drainFrames >= drainCap_) {
        b.draining = false;
        b.needsReset = true;
    }
}

// Its input from silence, unless a held tail keeps it there.
void Chain::fadeIn(Block& b, uint32_t fadeFrames) {
    b.inGain.set(0.0f);
    if (!(holdTails_ && b.processor->tailSamples() >= tailMin_))
        b.inGain.dbTo(1.0f, fadeFrames);
}

void Chain::process(const float* in, float* const* outs, uint32_t frames) {
    frames = std::min(frames, maxFrames_);
    if (!plan_) {
        for (size_t o = 0; o < mix_.size(); ++o)
            std::memset(outs[o], 0, frames * sizeof(float));
        return;
    }
    Plan& plan = *plan_;
    for (int c = 0; c < kChainChannels; ++c)
        std::memcpy(inBuf_[c], in, frames * sizeof(float));
    for (const Plan::Step& st : plan.order) {
        Block& b = *st.block;
        const float* const* input = gather(plan, st.wires, st.numWires, b.in, frames);
        if (b.hasKey && !gather(plan, st.keys, st.numKeys, b.key, frames, true))
            for (int c = 0; c < kChainChannels; ++c)
                std::memset(b.key[c], 0, frames * sizeof(float));
        const auto t0 = std::chrono::steady_clock::now();
        processBlock(b, input ? input : silence_, frames);
        const auto ns = static_cast<uint32_t>(std::min<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), UINT32_MAX));
        if (ns > slowNs_.load(std::memory_order_relaxed)) {
            slowNs_.store(ns, std::memory_order_relaxed);
            slowSlot_.store(static_cast<int>(st.slot), std::memory_order_relaxed);
        }
    }
    for (size_t o = 0; o < plan.outs.size(); ++o) {
        const float* const* signal = gather(plan, plan.outs[o].first, plan.outs[o].second, mixBuf_, frames);
        if (signal)
            writeTap(mix_[o], signal, outs[o], frames);
        else
            std::memset(outs[o], 0, frames * sizeof(float));
    }
}

bool Chain::takeSlowest(size_t& slot, uint32_t& ns) {
    const int s = slowSlot_.exchange(-1);
    ns = slowNs_.exchange(0);
    if (s < 0)
        return false;
    slot = static_cast<size_t>(s);
    return true;
}

Chain::Plan* Chain::receive(Plan* plan) {
    Plan* superseded = pending_;
    pending_ = plan;
    return superseded;
}

void Chain::prepareSwap(uint32_t fadeFrames) {
    if (!pending_)
        return;
    for (const auto& st : pending_->order) {
        Block& b = *st.block;
        if (b.processing && b.inputs != st.inputs && b.inGain.target() != 0.0f)
            b.inGain.dbTo(0.0f, fadeFrames);
    }
}

Chain::Plan* Chain::swap(bool processing, uint32_t fadeFrames, bool* stale) {
    Plan* old = plan_;
    Plan* next = pending_;
    if (!next)
        return nullptr;
    pending_ = nullptr;
    // mark_ + 1: played before; mark_ + 2: played from now on.
    const uint32_t was = mark_ + 1, now = mark_ + 2;
    mark_ += 2;
    if (old)
        for (const auto& st : old->order)
            st.block->mark = was;
    for (const auto& st : next->order) {
        Block& b = *st.block;
        const bool before = b.mark == was;
        b.mark = now;
        if (!before) {
            // Starts playing: from silence, as it was set when it stopped.
            if (processing && (b.stale || b.needsReset)) {
                b.processor->reset();
                b.stale = b.needsReset = false;
            }
            if (stale && (b.stale || b.needsReset))
                *stale = true;
            b.wet.set(b.bypassed ? 0.0f : 1.0f);
            b.draining = false;
            if (processing) {
                if (!b.processing) {
                    b.processor->startProcessing();
                    b.processing = true;
                }
                fadeIn(b, fadeFrames);
            }
        } else if (processing && b.inputs != st.inputs) {
            fadeIn(b, fadeFrames);
        }
        b.inputs = st.inputs;
    }
    if (old) {
        for (const auto& st : old->order) {
            Block& b = *st.block;
            if (b.mark == now)
                continue;
            if (b.processing) {
                b.processor->stopProcessing();
                b.processing = false;
            }
            b.stale = true;
        }
        // The wires both play go on as they were.
        for (auto& w : next->wires)
            for (const auto& o : old->wires)
                if (o.id == w.id) {
                    std::copy(o.from, o.from + 4, w.from);
                    std::copy(o.to, o.to + 4, w.to);
                    w.t = o.t;
                    w.plain = o.plain;
                    break;
                }
    }
    plan_ = next;
    return old;
}

} // namespace cue
