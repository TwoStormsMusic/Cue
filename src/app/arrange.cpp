#include "app/arrange.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <tuple>

namespace cue {

namespace {

const char* kUnarranged = "this performance has presets that share a chain (an INI config): chains and scenes can be changed "
                          "here once each chain has a preset of its own";

// The FCB's display: two letters or digits, and a leading 1, - or + before them.
const char* kDisplayRule = "the FCB's display shows two letters or digits (three with a leading 1, - or +)";
bool fitsDisplay(const std::string& text) {
    return text.size() <= 2 || (text.size() == 3 && (text[0] == '1' || text[0] == '-' || text[0] == '+'));
}

bool fail(std::string* error, const std::string& why) {
    *error = why;
    return false;
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool same(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

const ChainSpec* chainNamed(const SceneSpec& s, const std::string& name, const ChainSpec* except = nullptr) {
    for (const ChainSpec& c : s.chains)
        if (&c != except && same(c.name, name))
            return &c;
    return nullptr;
}

bool sceneNamed(const Config& config, const std::string& name, int except = -1) {
    for (size_t i = 0; i < config.scenes.size(); ++i)
        if (static_cast<int>(i) != except && same(config.scenes[i].name, name))
            return true;
    return false;
}

// The name with a number after it, from from up, that is free.
template <typename Taken>
std::string numbered(const std::string& name, Taken taken, int from) {
    for (int n = from;; ++n)
        if (const std::string next = name + " " + std::to_string(n); !taken(next))
            return next;
}

// The name if it is free, else as numbered from 2.
template <typename Taken>
std::string freeName(const std::string& name, Taken taken) {
    return taken(name) ? numbered(name, taken, 2) : name;
}

// A name a chain can have: not empty, and without the / that names a
// chain's plug-in in a mapping ("Slap/Wah").
bool goodName(const std::string& name, const char* what, std::string* error) {
    if (name.empty())
        return fail(error, std::string("a ") + what + " needs a name");
    if (name.find('/') != std::string::npos)
        return fail(error, std::string("a ") + what + "'s name cannot have a / in it");
    return true;
}

bool isWarmChoice(const std::string& warm) { return warm.empty() || same(warm, "next") || same(warm, "none"); }

// A chain with nothing in it: its input wired to every output.
void emptyChain(const Config& config, ChainSpec& cs, PresetSpec& ps) {
    for (size_t o = 0; o < config.outputs.size(); ++o)
        cs.wires.push_back({Port::input(), Port::output(static_cast<int>(o)), {}, ""});
    ps.warm = config.warm;
}

// Spare engine chains claimed for the next performance's new chains, with
// the plug-ins loaded for them. Given back unless kept.
class Fills {
public:
    explicit Fills(Engine& engine) : engine_(engine) {}
    ~Fills() {
        if (!kept_)
            for (const Fill& f : list)
                engine_.releaseChain(f.id);
    }
    // Claims a chain for cs (preferring that one) and loads its plug-ins;
    // cs takes the chain's id.
    bool add(ChainSpec& cs, int preferred, std::string* error) {
        Fill f;
        f.id = engine_.claimChain(cs.inputIndex, preferred);
        if (f.id < 0)
            return fail(error, "Cue has room for no more chains until the performance is opened again: save it, then start it again");
        list.push_back(std::move(f));
        cs.id = list.back().id;
        for (const PluginSpec& ps : cs.blocks) {
            std::unique_ptr<Processor> p;
            if (!loadLivePlugin(ps, engine_.settings(), p, error))
                return false;
            list.back().processors.push_back(std::move(p));
        }
        return true;
    }
    void keep() { kept_ = true; }

    struct Fill {
        int id = -1;
        std::vector<std::unique_ptr<Processor>> processors; // one for each block, in order
    };
    std::vector<Fill> list;

private:
    Engine& engine_;
    bool kept_ = false;
};

// The performance becomes next: the claimed chains get their blocks and
// wires, the chains in empties lose theirs and are spare again, and the
// show compiles next before, under its lock, the engine and the rig follow.
bool arrange(LiveRig& live, Config next, Fills& fills, const std::vector<int>& empties, std::string* error) {
    Rig& rig = live.rig;
    Engine& engine = *rig.engine;
    std::vector<Engine::PreparedEdit> prepared(fills.list.size() + empties.size());
    std::vector<RigPlugin> brought;
    size_t e = 0;
    for (auto& f : fills.list) {
        const ChainSpec* cs = chainSpec(next, f.id);
        if (!cs || cs->blocks.size() != f.processors.size())
            return fail(error, "a new chain lost its plug-ins");
        ChainEdit edit;
        for (size_t i = 0; i < cs->blocks.size(); ++i) {
            const PluginSpec& ps = cs->blocks[i];
            Processor* p = f.processors[i].get();
            RigPlugin rp;
            rp.chain = f.id;
            rp.slot = i;
            rp.processor = p;
            rp.label = ps.label.empty() ? p->name() : ps.label;
            rp.midi = ps.midi && p->acceptsMidi();
            rp.bypassed = ps.bypassed;
            brought.push_back(rp);
            edit.slots.push_back(-1);
            edit.added.push_back({std::move(f.processors[i]), ps.bypassed, ps.muteOnBypass});
        }
        for (const WireSpec& w : cs->wires)
            edit.wires.push_back({w.from, w.to, w.gains});
        if (!engine.prepareEdit(f.id, edit, prepared[e++], error))
            return false;
    }
    for (int id : empties) {
        ChainEdit nothing;
        if (!engine.prepareEdit(id, nothing, prepared[e++], error))
            return false;
    }

    // The rig's plug-ins, by chain then slot: the emptied chains' go, the
    // new ones' come.
    struct Entry {
        RigPlugin plugin;
        bool bypassed;
    };
    std::vector<Entry> entries;
    std::vector<Processor*> removed;
    for (size_t k = 0; k < rig.plugins.size(); ++k) {
        if (std::find(empties.begin(), empties.end(), rig.plugins[k].chain) != empties.end())
            removed.push_back(rig.plugins[k].processor);
        else
            entries.push_back({rig.plugins[k], rig.bypassedNow[k].load(std::memory_order_relaxed)});
    }
    for (const RigPlugin& rp : brought)
        entries.push_back({rp, rp.bypassed});
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return std::tie(a.plugin.chain, a.plugin.slot) < std::tie(b.plugin.chain, b.plugin.slot);
    });
    std::vector<ShowPlugin> show;
    for (const Entry& x : entries)
        show.push_back({x.plugin.chain, x.plugin.slot, x.plugin.label, x.plugin.midi, x.bypassed});
    const bool ok = live.show.reload(next, show, error, [&](std::string* why) {
        for (auto& p : prepared)
            if (!engine.applyEdit(p, why))
                return false;
        rig.plugins.clear();
        for (const Entry& x : entries)
            rig.plugins.push_back(x.plugin);
        rig.bypassedNow.reset(new std::atomic<bool>[entries.size()]);
        for (size_t k = 0; k < entries.size(); ++k)
            rig.bypassedNow[k].store(entries[k].bypassed, std::memory_order_relaxed);
        return true;
    });
    if (!ok)
        return false;
    fills.keep();

    if (rig.chainNames.size() < engine.capacity())
        rig.chainNames.resize(engine.capacity());
    for (int id : empties) {
        rig.chainNames[id].clear();
        engine.releaseChain(id);
    }
    for (const SceneSpec& s : next.scenes)
        for (const ChainSpec& cs : s.chains)
            if (cs.id >= 0 && static_cast<size_t>(cs.id) < rig.chainNames.size())
                rig.chainNames[cs.id] = cs.name;
    // What was taken out lives on until the audio thread lets it go; its
    // editors close now, and nothing services it any more.
    for (Processor* p : removed) {
        if (live.editors)
            live.editors->close(*p);
        rig.untrack(p);
    }
    for (const RigPlugin& rp : brought)
        rig.track(rp.processor, chainSpec(next, rp.chain)->blocks[rp.slot]);
    live.config = std::move(next);
    return true;
}

bool settle(LiveRig& live, Config next, std::string* error) {
    Fills none(*live.rig.engine);
    return arrange(live, std::move(next), none, {}, error);
}

// The plug-ins of a playing chain, into its spec as they sound now.
void captureChain(const Rig& rig, ChainSpec& cs) {
    for (size_t i = 0; i < cs.blocks.size(); ++i)
        if (Processor* p = rig.at(cs.id, i))
            captureState(*p, cs.blocks[i]);
}

std::string noChain(int id) { return "there is no chain number " + std::to_string(id + 1); }
std::string noScene(int scene) { return "there is no scene number " + std::to_string(scene + 1); }

} // namespace

bool arranged(const Config& config) {
    for (const SceneSpec& s : config.scenes) {
        if (s.presets.size() != s.chains.size())
            return false;
        for (size_t k = 0; k < s.chains.size(); ++k)
            if (!same(s.presets[k].chain, s.chains[k].name))
                return false;
    }
    return true;
}

bool findChain(const Config& config, int id, int& scene, int& at) {
    for (size_t s = 0; s < config.scenes.size(); ++s)
        for (size_t k = 0; k < config.scenes[s].chains.size(); ++k)
            if (config.scenes[s].chains[k].id == id) {
                scene = static_cast<int>(s);
                at = static_cast<int>(k);
                return true;
            }
    return false;
}

bool addChain(LiveRig& live, const NewChain& what, int* id, std::string* error) {
    const Config& config = live.config;
    if (!arranged(config))
        return fail(error, kUnarranged);
    if (what.scene < 0 || what.scene >= static_cast<int>(config.scenes.size()))
        return fail(error, noScene(what.scene));
    Config next = config;
    SceneSpec& scene = next.scenes[what.scene];
    ChainSpec cs;
    PresetSpec ps;
    std::string name = trimmed(what.name);
    if (what.copy >= 0) {
        int from = 0, at = 0;
        if (!findChain(config, what.copy, from, at))
            return fail(error, noChain(what.copy));
        cs = config.scenes[from].chains[at];
        ps = config.scenes[from].presets[at];
        captureChain(live.rig, cs);
        // A pedal selects one chain of a scene; a program change, one of the performance.
        if (from == what.scene || std::any_of(scene.presets.begin(), scene.presets.end(), [&](const PresetSpec& p) { return p.pedal == ps.pedal; }))
            ps.pedal = 0;
        ps.program = -1;
        if (!isWarmChoice(ps.warm) && !chainNamed(scene, ps.warm))
            ps.warm = "next";
        if (name.empty())
            name = freeName(cs.name, [&](const std::string& n) { return chainNamed(scene, n) != nullptr; });
    } else {
        if (!what.input.empty()) {
            const auto in = std::find_if(config.inputs.begin(), config.inputs.end(), [&](const std::string& n) { return same(n, what.input); });
            if (in == config.inputs.end())
                return fail(error, "there is no input named " + what.input);
            cs.inputIndex = static_cast<size_t>(in - config.inputs.begin());
            cs.input = *in;
        }
        emptyChain(config, cs, ps);
        if (name.empty())
            name = numbered("Chain", [&](const std::string& n) { return chainNamed(scene, n) != nullptr; }, static_cast<int>(scene.chains.size()) + 1);
    }
    if (!goodName(name, "chain", error))
        return false;
    if (chainNamed(scene, name))
        return fail(error, "scene " + scene.name + " already has a chain named " + name);
    cs.name = ps.name = ps.chain = name;
    Fills fills(*live.rig.engine);
    if (!fills.add(cs, -1, error))
        return false;
    const size_t at = what.at < 0 ? scene.chains.size() : std::min<size_t>(what.at, scene.chains.size());
    scene.chains.insert(scene.chains.begin() + static_cast<long>(at), cs);
    scene.presets.insert(scene.presets.begin() + static_cast<long>(at), ps);
    if (!arrange(live, std::move(next), fills, {}, error))
        return false;
    if (id)
        *id = cs.id;
    return true;
}

bool removeChain(LiveRig& live, int id, RemovedChain* removed, std::string* error) {
    const Config& config = live.config;
    if (!arranged(config))
        return fail(error, kUnarranged);
    int s = 0, at = 0;
    if (!findChain(config, id, s, at))
        return fail(error, noChain(id));
    const SceneSpec& scene = config.scenes[s];
    const std::string name = scene.chains[at].name;
    if (live.show.presetChain(live.show.preset()) == id)
        return fail(error, name + " is playing: play another chain before taking it out");
    if (scene.chains.size() == 1)
        return fail(error, "a scene keeps at least one chain: take out scene " + scene.name + " instead");
    RemovedChain taken{id, s, at, scene.chains[at], scene.presets[at]};
    if (removed)
        captureChain(live.rig, taken.chain);
    Config next = config;
    SceneSpec& ns = next.scenes[s];
    ns.chains.erase(ns.chains.begin() + at);
    ns.presets.erase(ns.presets.begin() + at);
    // A chain kept warm for another goes; the next one is kept warm instead.
    for (PresetSpec& p : ns.presets)
        if (same(p.warm, name)) {
            p.warm = "next";
            taken.warmFor.push_back(p.name);
        }
    Fills none(*live.rig.engine);
    if (!arrange(live, std::move(next), none, {id}, error))
        return false;
    if (removed)
        *removed = std::move(taken);
    return true;
}

bool restoreChain(LiveRig& live, const RemovedChain& removed, int* id, std::string* error) {
    if (!arranged(live.config))
        return fail(error, kUnarranged);
    if (removed.scene >= static_cast<int>(live.config.scenes.size()))
        return fail(error, "the scene " + removed.chain.name + " was in is gone, so it cannot go back");
    Config next = live.config;
    SceneSpec& scene = next.scenes[removed.scene];
    if (chainNamed(scene, removed.chain.name))
        return fail(error, "scene " + scene.name + " has a chain named " + removed.chain.name + " again, so the old one cannot go back");
    ChainSpec cs = removed.chain;
    Fills fills(*live.rig.engine);
    if (!fills.add(cs, removed.id, error))
        return false;
    const size_t at = std::min<size_t>(removed.at, scene.chains.size());
    scene.chains.insert(scene.chains.begin() + static_cast<long>(at), cs);
    scene.presets.insert(scene.presets.begin() + static_cast<long>(at), removed.preset);
    for (PresetSpec& p : scene.presets)
        for (const std::string& warm : removed.warmFor)
            if (same(p.name, warm) && same(p.warm, "next"))
                p.warm = cs.name;
    if (!arrange(live, std::move(next), fills, {}, error))
        return false;
    if (id)
        *id = cs.id;
    return true;
}

bool setChain(LiveRig& live, int id, const ChainSettings& set, std::string* error) {
    if (!arranged(live.config))
        return fail(error, kUnarranged);
    int s = 0, at = 0;
    if (!findChain(live.config, id, s, at))
        return fail(error, noChain(id));
    Config next = live.config;
    SceneSpec& scene = next.scenes[s];
    ChainSpec& cs = scene.chains[at];
    PresetSpec& ps = scene.presets[at];
    if (set.name) {
        const std::string name = trimmed(*set.name);
        if (!goodName(name, "chain", error))
            return false;
        if (chainNamed(scene, name, &cs))
            return fail(error, "scene " + scene.name + " already has a chain named " + name);
        for (PresetSpec& p : scene.presets)
            if (same(p.warm, cs.name))
                p.warm = name;
        cs.name = ps.name = ps.chain = name;
    }
    if (set.display) {
        const std::string display = trimmed(*set.display);
        if (!fitsDisplay(display))
            return fail(error, kDisplayRule);
        ps.display = display;
    }
    if (set.pedal) {
        const int pedal = *set.pedal;
        if (pedal < 0 || pedal > 10)
            return fail(error, "a pedal is 1 to 10, or none");
        if (pedal)
            for (PresetSpec& p : scene.presets)
                if (&p != &ps && p.pedal == pedal)
                    p.pedal = 0;
        ps.pedal = pedal;
    }
    if (set.program) {
        const int program = *set.program;
        if (program < -1 || program > 127)
            return fail(error, "a program change is 0 to 127, or none");
        if (program >= 0)
            for (SceneSpec& sc : next.scenes)
                for (PresetSpec& p : sc.presets)
                    if (&p != &ps && p.program == program)
                        p.program = -1;
        ps.program = program;
    }
    if (set.onSelect) {
        ps.actions.clear();
        for (const std::string& text : *set.onSelect)
            if (!trimmed(text).empty())
                ps.actions.push_back({trimmed(text), ""});
    }
    if (set.warm) {
        const std::string warm = trimmed(*set.warm);
        if (same(warm, "next") || same(warm, "none")) {
            ps.warm = lower(warm);
        } else if (const ChainSpec* other = chainNamed(scene, warm, &cs)) {
            ps.warm = other->name;
        } else {
            return fail(error, "the chain kept warm is the next one, none, or another chain of scene " + scene.name);
        }
    }
    return settle(live, std::move(next), error);
}

bool moveChain(LiveRig& live, int id, int toScene, int at, std::string* error) {
    if (!arranged(live.config))
        return fail(error, kUnarranged);
    int s = 0, from = 0;
    if (!findChain(live.config, id, s, from))
        return fail(error, noChain(id));
    if (toScene < 0 || toScene >= static_cast<int>(live.config.scenes.size()))
        return fail(error, noScene(toScene));
    Config next = live.config;
    SceneSpec& src = next.scenes[s];
    ChainSpec cs = src.chains[from];
    PresetSpec ps = src.presets[from];
    if (toScene != s) {
        SceneSpec& dst = next.scenes[toScene];
        if (src.chains.size() == 1)
            return fail(error, "a scene keeps at least one chain: copy " + cs.name + " instead, or take out scene " + src.name);
        if (chainNamed(dst, cs.name))
            return fail(error, "scene " + dst.name + " already has a chain named " + cs.name);
        if (!isWarmChoice(ps.warm) && !chainNamed(dst, ps.warm))
            ps.warm = "next";
        if (ps.pedal && std::any_of(dst.presets.begin(), dst.presets.end(), [&](const PresetSpec& p) { return p.pedal == ps.pedal; }))
            ps.pedal = 0;
    }
    src.chains.erase(src.chains.begin() + from);
    src.presets.erase(src.presets.begin() + from);
    if (toScene != s)
        for (PresetSpec& p : src.presets)
            if (same(p.warm, cs.name))
                p.warm = "next";
    SceneSpec& dst = next.scenes[toScene];
    const size_t to = at < 0 ? dst.chains.size() : std::min<size_t>(at, dst.chains.size());
    dst.chains.insert(dst.chains.begin() + static_cast<long>(to), cs);
    dst.presets.insert(dst.presets.begin() + static_cast<long>(to), ps);
    return settle(live, std::move(next), error);
}

bool addScene(LiveRig& live, const NewScene& what, int* index, std::vector<int>* ids, std::string* error) {
    const Config& config = live.config;
    if (!arranged(config))
        return fail(error, kUnarranged);
    Config next = config;
    SceneSpec scene;
    std::string name = trimmed(what.name);
    auto taken = [&](const std::string& n) { return sceneNamed(config, n); };
    if (what.copy >= 0) {
        if (what.copy >= static_cast<int>(config.scenes.size()))
            return fail(error, noScene(what.copy));
        const SceneSpec& from = config.scenes[what.copy];
        scene = from;
        // Its own pedal and program change, should it get any.
        scene.pedal = 0;
        scene.program = -1;
        for (size_t k = 0; k < scene.chains.size(); ++k) {
            captureChain(live.rig, scene.chains[k]);
            scene.presets[k].program = -1;
        }
        if (name.empty())
            name = freeName(from.name, taken);
    } else {
        ChainSpec cs;
        PresetSpec ps;
        emptyChain(config, cs, ps);
        cs.name = ps.name = ps.chain = "Chain 1";
        scene.chains.push_back(cs);
        scene.presets.push_back(ps);
        if (name.empty())
            name = numbered("Scene", taken, static_cast<int>(config.scenes.size()) + 1);
    }
    if (!goodName(name, "scene", error))
        return false;
    if (taken(name))
        return fail(error, "there is already a scene named " + name);
    // What only worked in the copied scene works in the new one too.
    if (what.copy >= 0)
        for (const TriggerSpec& t : config.triggers)
            if (same(t.scene, config.scenes[what.copy].name)) {
                next.triggers.push_back(t);
                next.triggers.back().scene = name;
            }
    scene.name = name;
    Fills fills(*live.rig.engine);
    for (ChainSpec& cs : scene.chains)
        if (!fills.add(cs, -1, error))
            return false;
    const size_t at = what.at < 0 ? next.scenes.size() : std::min<size_t>(what.at, next.scenes.size());
    next.scenes.insert(next.scenes.begin() + static_cast<long>(at), scene);
    if (!arrange(live, std::move(next), fills, {}, error))
        return false;
    if (index)
        *index = static_cast<int>(at);
    if (ids) {
        ids->clear();
        for (const ChainSpec& cs : scene.chains)
            ids->push_back(cs.id);
    }
    return true;
}

bool removeScene(LiveRig& live, int s, RemovedScene* removed, std::string* error) {
    const Config& config = live.config;
    if (!arranged(config))
        return fail(error, kUnarranged);
    if (s < 0 || s >= static_cast<int>(config.scenes.size()))
        return fail(error, noScene(s));
    const SceneSpec& scene = config.scenes[s];
    if (config.scenes.size() == 1)
        return fail(error, "a performance keeps at least one scene");
    if (live.show.scene() == s)
        return fail(error, "scene " + scene.name + " is playing: play a chain of another scene before taking it out");
    RemovedScene taken;
    taken.at = s;
    taken.scene = scene;
    std::vector<int> ids;
    for (ChainSpec& cs : taken.scene.chains) {
        if (removed)
            captureChain(live.rig, cs);
        ids.push_back(cs.id);
    }
    Config next = config;
    next.scenes.erase(next.scenes.begin() + s);
    // The triggers that work only in it go with it.
    next.triggers.clear();
    for (size_t i = 0; i < config.triggers.size(); ++i) {
        if (same(config.triggers[i].scene, scene.name))
            taken.triggers.push_back({i, config.triggers[i]});
        else
            next.triggers.push_back(config.triggers[i]);
    }
    Fills none(*live.rig.engine);
    if (!arrange(live, std::move(next), none, ids, error))
        return false;
    if (removed)
        *removed = std::move(taken);
    return true;
}

bool restoreScene(LiveRig& live, const RemovedScene& removed, std::string* error) {
    if (!arranged(live.config))
        return fail(error, kUnarranged);
    if (sceneNamed(live.config, removed.scene.name))
        return fail(error, "there is a scene named " + removed.scene.name + " again, so the old one cannot go back");
    Config next = live.config;
    SceneSpec scene = removed.scene;
    Fills fills(*live.rig.engine);
    for (ChainSpec& cs : scene.chains)
        if (!fills.add(cs, cs.id, error))
            return false;
    const size_t at = std::min<size_t>(removed.at, next.scenes.size());
    next.scenes.insert(next.scenes.begin() + static_cast<long>(at), scene);
    for (const auto& [place, trigger] : removed.triggers)
        next.triggers.insert(next.triggers.begin() + static_cast<long>(std::min(place, next.triggers.size())), trigger);
    return arrange(live, std::move(next), fills, {}, error);
}

bool setScene(LiveRig& live, int s, const SceneSettings& set, std::string* error) {
    if (s < 0 || s >= static_cast<int>(live.config.scenes.size()))
        return fail(error, noScene(s));
    Config next = live.config;
    SceneSpec& scene = next.scenes[s];
    if (set.name) {
        const std::string name = trimmed(*set.name);
        if (!goodName(name, "scene", error))
            return false;
        if (sceneNamed(next, name, s))
            return fail(error, "there is already a scene named " + name);
        for (TriggerSpec& t : next.triggers)
            if (same(t.scene, scene.name))
                t.scene = name;
        scene.name = name;
    }
    if (set.display) {
        const std::string display = trimmed(*set.display);
        if (!fitsDisplay(display))
            return fail(error, kDisplayRule);
        scene.display = display;
    }
    if (set.pedal) {
        if (*set.pedal < 0 || *set.pedal > 10)
            return fail(error, "a pedal is 1 to 10, or none");
        if (*set.pedal)
            for (SceneSpec& other : next.scenes)
                if (&other != &scene && other.pedal == *set.pedal)
                    other.pedal = 0;
        scene.pedal = *set.pedal;
    }
    if (set.program) {
        if (*set.program < -1 || *set.program > 127)
            return fail(error, "a program change is 0 to 127, or none");
        if (*set.program >= 0)
            for (SceneSpec& other : next.scenes)
                if (&other != &scene && other.program == *set.program)
                    other.program = -1;
        scene.program = *set.program;
    }
    return settle(live, std::move(next), error);
}

bool moveScene(LiveRig& live, int s, int at, std::string* error) {
    const int n = static_cast<int>(live.config.scenes.size());
    if (s < 0 || s >= n)
        return fail(error, noScene(s));
    const int to = at < 0 || at >= n ? n - 1 : at;
    if (to == s)
        return true;
    Config next = live.config;
    SceneSpec scene = std::move(next.scenes[s]);
    next.scenes.erase(next.scenes.begin() + s);
    next.scenes.insert(next.scenes.begin() + to, std::move(scene));
    return settle(live, std::move(next), error);
}

} // namespace cue
