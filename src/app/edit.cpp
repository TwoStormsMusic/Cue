#include "app/edit.h"

#include <algorithm>
#include <atomic>
#include <memory>

namespace cue {

ChainSpec* chainSpec(Config& config, int chain) {
    int c = 0;
    for (auto& scene : config.scenes)
        for (auto& cs : scene.chains)
            if (c++ == chain)
                return &cs;
    return nullptr;
}

namespace {

bool fail(std::string* error, const std::string& why) {
    *error = why;
    return false;
}

std::vector<int> unchanged(size_t n) {
    std::vector<int> keep(n);
    for (size_t i = 0; i < n; ++i)
        keep[i] = static_cast<int>(i);
    return keep;
}

// The chain becomes next: keep says, for each of its blocks, the slot it is
// now, or -1 for the next of added (loaded and active). The engine prepares
// the change first (priming what is added), then the show compiles the
// performance as it will be, and only then, under the show's lock, does
// the engine get it and the rig's list of plug-ins follow.
bool rewire(LiveRig& live, int chain, ChainSpec next, const std::vector<int>& keep, std::vector<std::unique_ptr<Processor>> added,
            std::string* error) {
    Rig& rig = live.rig;
    Engine& engine = *rig.engine;
    ChainEdit edit;
    edit.slots = keep;
    std::vector<Processor*> brought;
    size_t a = 0;
    for (size_t i = 0; i < keep.size(); ++i)
        if (keep[i] < 0) {
            if (a >= added.size())
                return fail(error, "an edit adds a plug-in it does not have");
            brought.push_back(added[a].get());
            edit.added.push_back({std::move(added[a++]), next.blocks[i].bypassed, next.blocks[i].muteOnBypass});
        }
    for (const WireSpec& w : next.wires)
        edit.wires.push_back({w.from, w.to, w.gains});
    Engine::PreparedEdit prepared;
    if (!engine.prepareEdit(chain, edit, prepared, error))
        return false;

    // The rig's plug-ins, chain by chain: this one's in its new order.
    std::vector<RigPlugin> plugins;
    std::vector<bool> bypassed;
    std::vector<Processor*> removed;
    std::vector<int> oldIndex; // where each of this chain's old plug-ins is in rig.plugins, by slot
    for (size_t k = 0; k < rig.plugins.size(); ++k)
        if (rig.plugins[k].chain == chain) {
            if (oldIndex.size() <= rig.plugins[k].slot)
                oldIndex.resize(rig.plugins[k].slot + 1, -1);
            oldIndex[rig.plugins[k].slot] = static_cast<int>(k);
        }
    for (size_t s = 0; s < oldIndex.size(); ++s)
        if (oldIndex[s] >= 0 && std::find(keep.begin(), keep.end(), static_cast<int>(s)) == keep.end())
            removed.push_back(rig.plugins[oldIndex[s]].processor);
    bool placed = false;
    auto ours = [&] {
        size_t b = 0;
        for (size_t i = 0; i < keep.size(); ++i) {
            RigPlugin rp;
            bool now;
            if (keep[i] >= 0) {
                const int k = static_cast<size_t>(keep[i]) < oldIndex.size() ? oldIndex[keep[i]] : -1;
                if (k < 0)
                    continue;
                rp = rig.plugins[k];
                now = rig.bypassedNow[k].load(std::memory_order_relaxed);
            } else {
                Processor* p = brought[b++];
                const PluginSpec& ps = next.blocks[i];
                rp.chain = chain;
                rp.processor = p;
                rp.label = ps.label.empty() ? p->name() : ps.label;
                rp.midi = ps.midi && p->acceptsMidi();
                rp.bypassed = ps.bypassed;
                now = ps.bypassed;
            }
            rp.slot = i;
            plugins.push_back(rp);
            bypassed.push_back(now);
        }
        placed = true;
    };
    for (size_t k = 0; k < rig.plugins.size(); ++k) {
        if (rig.plugins[k].chain == chain) {
            if (!placed)
                ours();
            continue;
        }
        if (!placed && rig.plugins[k].chain > chain)
            ours();
        plugins.push_back(rig.plugins[k]);
        bypassed.push_back(rig.bypassedNow[k].load(std::memory_order_relaxed));
    }
    if (!placed)
        ours();

    Config config = live.config;
    *chainSpec(config, chain) = std::move(next);
    std::vector<ShowPlugin> show;
    for (size_t k = 0; k < plugins.size(); ++k)
        show.push_back({plugins[k].chain, plugins[k].slot, plugins[k].label, plugins[k].midi, bypassed[k]});
    const bool ok = live.show.reload(config, show, error, [&](std::string* why) {
        if (!engine.applyEdit(prepared, why))
            return false;
        rig.plugins = std::move(plugins);
        rig.bypassedNow.reset(new std::atomic<bool>[rig.plugins.size()]);
        for (size_t k = 0; k < rig.plugins.size(); ++k)
            rig.bypassedNow[k].store(bypassed[k], std::memory_order_relaxed);
        return true;
    });
    if (!ok)
        return false;
    live.config = std::move(config);
    // A removed plug-in lives on until the audio thread lets it go; its
    // editor closes now, and nothing services it any more.
    for (Processor* p : removed) {
        if (live.editors)
            live.editors->close(*p);
        rig.untrack(p);
    }
    const ChainSpec& now = *chainSpec(live.config, chain);
    size_t b = 0;
    for (size_t i = 0; i < keep.size(); ++i)
        if (keep[i] < 0)
            rig.track(brought[b++], now.blocks[i]);
    return true;
}

int findWire(const ChainSpec& c, Port from, Port to) {
    for (size_t i = 0; i < c.wires.size(); ++i)
        if (c.wires[i].from == from && c.wires[i].to == to)
            return static_cast<int>(i);
    return -1;
}

} // namespace

bool addPlugin(LiveRig& live, int chain, PluginSpec spec, const WireSpec* on, size_t* slot, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    ChainSpec next = *cs;
    const int n = static_cast<int>(next.blocks.size());
    if (on) {
        const int w = findWire(next, on->from, on->to);
        if (w < 0)
            return fail(error, "there is no such wire to put it on");
        const WireSpec was = next.wires[w];
        next.wires[w] = {was.from, Port::block(n), {}, was.where};
        next.wires.insert(next.wires.begin() + w + 1, {Port::block(n), was.to, was.gains, was.where});
    }
    std::unique_ptr<Processor> p;
    if (!loadLivePlugin(spec, live.rig.engine->settings(), p, error))
        return false;
    spec.sidechain.clear(); // keys are wires now
    next.blocks.push_back(std::move(spec));
    std::vector<int> keep = unchanged(n);
    keep.push_back(-1);
    std::vector<std::unique_ptr<Processor>> added;
    added.push_back(std::move(p));
    if (!rewire(live, chain, std::move(next), keep, std::move(added), error))
        return false;
    if (slot)
        *slot = static_cast<size_t>(n);
    return true;
}

bool removePlugin(LiveRig& live, int chain, size_t slot, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs || slot >= cs->blocks.size())
        return fail(error, "there is no plug-in " + std::to_string(slot + 1) + " in chain number " + std::to_string(chain + 1));
    ChainSpec next = *cs;
    const Port self = Port::block(static_cast<int>(slot));
    std::vector<WireSpec> into, outOf, wires;
    for (const WireSpec& w : next.wires) {
        if (w.to == self)
            into.push_back(w);
        else if (w.from == self)
            outOf.push_back(w);
        else if (!(w.to.kind == Port::Kind::Block && w.to.index == self.index))
            wires.push_back(w); // its key's wires go with it
    }
    // What fed it feeds what it fed.
    for (const WireSpec& a : into)
        for (const WireSpec& b : outOf) {
            const bool there = std::any_of(wires.begin(), wires.end(), [&](const WireSpec& w) { return w.from == a.from && w.to == b.to; });
            if (!there)
                wires.push_back({a.from, b.to, b.gains, b.where});
        }
    for (WireSpec& w : wires)
        for (Port* p : {&w.from, &w.to})
            if (p->kind == Port::Kind::Block && p->index > self.index)
                --p->index;
    next.wires = std::move(wires);
    next.blocks.erase(next.blocks.begin() + static_cast<long>(slot));
    std::vector<int> keep;
    for (size_t i = 0; i < cs->blocks.size(); ++i)
        if (i != slot)
            keep.push_back(static_cast<int>(i));
    std::string why;
    if (!rewire(live, chain, std::move(next), keep, {}, &why))
        return fail(error, why.find("no plug-in named") != std::string::npos ? "the mappings still use it: " + why : why);
    return true;
}

bool setWire(LiveRig& live, int chain, const WireSpec& wire, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    const int w = findWire(*cs, wire.from, wire.to);
    if (w >= 0) {
        // Only its gains: they move, nothing is swapped.
        if (!live.rig.engine->setWire(chain, wire.from, wire.to, wire.gains))
            return fail(error, "the engine has no such wire");
        cs->wires[w].gains = wire.gains;
        return true;
    }
    ChainSpec next = *cs;
    next.wires.push_back(wire);
    return rewire(live, chain, std::move(next), unchanged(cs->blocks.size()), {}, error);
}

bool removeWire(LiveRig& live, int chain, Port from, Port to, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    const int w = findWire(*cs, from, to);
    if (w < 0)
        return fail(error, "there is no such wire");
    ChainSpec next = *cs;
    next.wires.erase(next.wires.begin() + w);
    return rewire(live, chain, std::move(next), unchanged(cs->blocks.size()), {}, error);
}

bool setWires(LiveRig& live, int chain, const std::vector<WireSpec>& wires, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    ChainSpec next = *cs;
    next.wires = wires;
    return rewire(live, chain, std::move(next), unchanged(cs->blocks.size()), {}, error);
}

bool placePlugin(LiveRig& live, int chain, size_t slot, double x, double y, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs || slot >= cs->blocks.size())
        return fail(error, "there is no plug-in " + std::to_string(slot + 1) + " in chain number " + std::to_string(chain + 1));
    PluginSpec& p = cs->blocks[slot];
    p.placed = true;
    p.x = x;
    p.y = y;
    return true;
}

} // namespace cue
