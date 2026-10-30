#include "app/edit.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>

namespace cue {

ChainSpec* chainSpec(Config& config, int chain) {
    int c = 0;
    for (auto& scene : config.scenes)
        for (auto& cs : scene.chains) {
            if (cs.id >= 0 ? cs.id == chain : c == chain)
                return &cs;
            ++c;
        }
    return nullptr;
}

namespace {

bool fail(std::string* error, const std::string& why) {
    *error = why;
    return false;
}

std::string lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
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
        edit.wires.push_back({w.from, w.to, w.gains, 0, w.feedback});
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
        // On a feedback wire, the new plug-in's way on is the one heard late.
        next.wires.insert(next.wires.begin() + w + 1, {Port::block(n), was.to, was.gains, was.where, was.feedback});
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

bool removePlugin(LiveRig& live, int chain, size_t slot, Removed* removed, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs || slot >= cs->blocks.size())
        return fail(error, "there is no plug-in " + std::to_string(slot + 1) + " in chain number " + std::to_string(chain + 1));
    Removed taken{chain, slot, cs->blocks[slot], cs->wires, cs->loose, cs->blocks.size()};
    if (removed)
        if (Processor* p = live.rig.at(chain, slot))
            captureState(*p, taken.spec);
    ChainSpec next = *cs;
    const Port self = Port::block(static_cast<int>(slot));
    std::vector<WireSpec> into, outOf, wires;
    for (const WireSpec& w : next.wires) {
        if (w.from == self && w.to.kind == Port::Kind::Block && w.to.index == self.index)
            continue; // into itself: it goes with it
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
                wires.push_back({a.from, b.to, b.gains, b.where, a.feedback || b.feedback});
        }
    for (WireSpec& w : wires)
        for (Port* p : {&w.from, &w.to})
            if (p->kind == Port::Kind::Block && p->index > self.index)
                --p->index;
    next.wires = std::move(wires);
    // Its loose wires go with it; the others' plug-ins are renumbered.
    std::vector<LooseSpec> loose;
    for (LooseSpec l : next.loose) {
        if (l.port.kind == Port::Kind::Block && l.port.index == self.index)
            continue;
        if (l.port.kind == Port::Kind::Block && l.port.index > self.index)
            --l.port.index;
        loose.push_back(l);
    }
    next.loose = std::move(loose);
    next.blocks.erase(next.blocks.begin() + static_cast<long>(slot));
    std::vector<int> keep;
    for (size_t i = 0; i < cs->blocks.size(); ++i)
        if (i != slot)
            keep.push_back(static_cast<int>(i));
    std::string why;
    if (!rewire(live, chain, std::move(next), keep, {}, &why))
        return fail(error, why.find("no plug-in named") != std::string::npos ? "the mappings still use it: " + why : why);
    if (removed)
        *removed = std::move(taken);
    return true;
}

bool restorePlugin(LiveRig& live, const Removed& removed, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, removed.chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(removed.chain + 1));
    if (cs->blocks.size() + 1 != removed.blocks || removed.slot > cs->blocks.size())
        return fail(error, "chain " + cs->name + " has changed since that plug-in was taken out, so it cannot go back");
    std::unique_ptr<Processor> p;
    if (!loadLivePlugin(removed.spec, live.rig.engine->settings(), p, error))
        return false;
    ChainSpec next = *cs;
    next.blocks.insert(next.blocks.begin() + static_cast<long>(removed.slot), removed.spec);
    next.wires = removed.wires;
    next.loose = removed.loose;
    std::vector<int> keep;
    for (size_t i = 0; i < next.blocks.size(); ++i)
        keep.push_back(i < removed.slot ? static_cast<int>(i) : i == removed.slot ? -1 : static_cast<int>(i) - 1);
    std::vector<std::unique_ptr<Processor>> added;
    added.push_back(std::move(p));
    return rewire(live, removed.chain, std::move(next), keep, std::move(added), error);
}

bool setWire(LiveRig& live, int chain, const WireSpec& wire, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    const int w = findWire(*cs, wire.from, wire.to);
    if (w >= 0) {
        // Only its gains: they move, nothing is swapped. How it is drawn
        // is the app's.
        if (!live.rig.engine->setWire(chain, wire.from, wire.to, wire.gains))
            return fail(error, "the engine has no such wire");
        cs->wires[w].gains = wire.gains;
        cs->wires[w].jump = wire.jump;
        cs->wires[w].jumpOut = wire.jumpOut;
        cs->wires[w].jumpIn = wire.jumpIn;
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

bool setWires(LiveRig& live, int chain, const std::vector<WireSpec>* wires, const std::vector<LooseSpec>* loose, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    if (loose)
        for (const LooseSpec& l : *loose)
            if (l.port.kind == Port::Kind::Block && (l.port.index < 0 || static_cast<size_t>(l.port.index) >= cs->blocks.size()))
                return fail(error, "a loose wire names a plug-in chain " + cs->name + " does not have");
    if (wires) {
        ChainSpec next = *cs;
        next.wires = *wires;
        if (!rewire(live, chain, std::move(next), unchanged(cs->blocks.size()), {}, error))
            return false;
        cs = chainSpec(live.config, chain);
    }
    if (loose)
        cs->loose = *loose;
    return true;
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

bool placePort(LiveRig& live, int chain, Port port, double x, double y, std::string* error) {
    ChainSpec* cs = chainSpec(live.config, chain);
    if (!cs)
        return fail(error, "there is no chain number " + std::to_string(chain + 1));
    if (port.kind == Port::Kind::Block)
        return placePlugin(live, chain, static_cast<size_t>(port.index), x, y, error);
    BoardPlace* at = &cs->inputAt;
    if (port.kind == Port::Kind::Output) {
        if (port.index < 0 || static_cast<size_t>(port.index) >= live.config.outputs.size())
            return fail(error, "there is no output " + std::to_string(port.index + 1));
        if (cs->outputsAt.size() < live.config.outputs.size())
            cs->outputsAt.resize(live.config.outputs.size());
        at = &cs->outputsAt[static_cast<size_t>(port.index)];
    }
    *at = {true, x, y};
    return true;
}

bool renamePort(LiveRig& live, bool input, size_t index, const std::string& name, std::string* error) {
    Config& config = live.config;
    std::vector<std::string>& names = input ? config.inputs : config.outputs;
    const std::string what = input ? "input" : "output";
    if (index >= names.size())
        return fail(error, "there is no " + what + " " + std::to_string(index + 1));
    const size_t first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
    const std::string trimmed = first == std::string::npos ? "" : name.substr(first, last - first + 1);
    if (trimmed.empty())
        return fail(error, "an " + what + " needs a name");
    // Requests name them so: "in" is a chain's input, in2 and out2 the second.
    const std::string low = lower(trimmed), prefix = input ? "in" : "out";
    const bool numbered = low.size() > prefix.size() && low.compare(0, prefix.size(), prefix) == 0 &&
                          std::all_of(low.begin() + static_cast<long>(prefix.size()), low.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
    if (low == "in" || (numbered && low != prefix + std::to_string(index + 1)))
        return fail(error, "'" + trimmed + "' is how requests name another " + what + "; choose another name");
    for (size_t i = 0; i < names.size(); ++i)
        if (i != index && lower(names[i]) == low)
            return fail(error, "another " + what + " is named " + names[i]);
    names[index] = trimmed;
    // What names it by name follows.
    if (input) {
        for (auto& scene : config.scenes)
            for (auto& cs : scene.chains)
                if (cs.inputIndex == index)
                    cs.input = trimmed;
        if (config.tuner.inputIndex == index)
            config.tuner.input = trimmed;
    }
    return true;
}

} // namespace cue
