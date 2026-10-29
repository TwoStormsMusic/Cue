// cue: the live plug-in host prototype.
//
//   cue run <config|file.cue>    play live: ALSA audio, FCB1010 switching
//                                (a .cue takes this machine's settings from its rig profile, or --rig FILE)
//   cue convert <config> [out]   save a config as a .cue performance (--rig FILE: and its audio and MIDI settings)
//   cue ctl <command>            talk to a running cue: status, watch, select, do, save, performance, quit
//   cue bench <config> [opts]    offline: measure CPU per chain and render a WAV
//   cue doctor [--device M2]     check real-time settings
//   cue devices                  list sound cards and MIDI ports
//   cue check <config>           load everything, check the mappings, print the performance
//                                (--no-load: check without loading plug-ins)
//   cue scan <file.clap|.vst3>   list the plug-ins in a CLAP file or VST3 bundle
//   cue params <file>[#id]       list a plug-in's parameters and their ids
//   cue import-rpp <file.rpp>    make a config from a REAPER project
//   cue export-rpp <file.cue>    write a performance (or config) as a REAPER project
//   cue midi-monitor             print incoming MIDI, decoded for EurekaPROM
//   cue fcb-test [--channel N]   walk the FCB1010 LEDs and display

#include "app/arrange.h"
#include "app/build.h"
#include "app/config.h"
#include "app/convert.h"
#include "app/cpu.h"
#include "app/cuefile.h"
#include "app/service.h"
#include "app/doctor.h"
#include "app/edit.h"
#include "app/rpp.h"
#include "app/scan.h"
#include "control/show.h"
#include "audio/alsa_backend.h"
#include "audio/take_device.h"
#include "core/engine.h"
#include "host/clap_processor.h"
#include "host/editor_host.h"
#include "host/vst3_processor.h"
#include "midi/alsa_midi.h"
#include "midi/eurekaprom.h"
#include "tuner/tuner.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace cue;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ------------------------------------------------------------ MIDI out

// Messages for the controller (LEDs, display), queued by any thread and
// sent by the MIDI thread.
class MidiOutQueue {
public:
    void push(const MidiMessage& m) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(m);
    }
    void flush(AlsaMidi& midi) {
        std::vector<MidiMessage> out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            out.swap(pending_);
        }
        for (const auto& m : out)
            midi.send(m);
    }

private:
    std::mutex mutex_;
    std::vector<MidiMessage> pending_;
};

// ------------------------------------------------------------ status

// The engine's and the sound card's numbers over a stretch of time: one
// sample (about 30 a second, what status events carry), or the samples
// since the terminal's last status line.
struct Window {
    uint64_t cycles = 0, ns = 0;
    uint32_t maxNs = 0;
    int32_t minQueued = INT32_MAX; // the least audio queued for playback when a block was written
    std::vector<uint64_t> chainNs, chainBlocks;
    std::vector<uint32_t> chainMaxNs;

    explicit Window(size_t chains = 0) : chainNs(chains), chainBlocks(chains), chainMaxNs(chains) {}
    void add(const Window& w) {
        cycles += w.cycles;
        ns += w.ns;
        maxNs = std::max(maxNs, w.maxNs);
        minQueued = std::min(minQueued, w.minQueued);
        for (size_t i = 0; i < chainNs.size() && i < w.chainNs.size(); ++i) {
            chainNs[i] += w.chainNs[i];
            chainBlocks[i] += w.chainBlocks[i];
            chainMaxNs[i] = std::max(chainMaxNs[i], w.chainMaxNs[i]);
        }
    }
    double avgUs() const { return cycles ? ns / 1000.0 / cycles : 0.0; }
    // A chain added since the window began has nothing in it yet.
    double chainAvgUs(size_t i) const { return i < chainBlocks.size() && chainBlocks[i] ? chainNs[i] / 1000.0 / chainBlocks[i] : 0.0; }
    double chainMaxUs(size_t i) const { return i < chainMaxNs.size() ? chainMaxNs[i] / 1000.0 : 0.0; }
};

// Takes a sample: the counters' growth since the last one, and the peaks
// since then (which reading resets).
class Sampler {
public:
    Window take(const AlsaStats& stats, Engine& engine) {
        auto& st = const_cast<AlsaStats&>(stats);
        Window w(engine.numChains());
        last_.resize(engine.numChains());
        const uint64_t cycles = st.cycles.load(), ns = st.totalCycleNs.load();
        w.cycles = cycles - cycles_;
        w.ns = ns - ns_;
        cycles_ = cycles;
        ns_ = ns;
        w.maxNs = st.maxCycleNs.exchange(0);
        w.minQueued = st.minQueuedFrames.exchange(INT32_MAX);
        for (size_t i = 0; i < engine.numChains(); ++i) {
            ChainStats& cs = engine.stats(i);
            const uint64_t cns = cs.processNs.load(), blocks = cs.processBlocks.load();
            w.chainNs[i] = cns - last_[i].first;
            w.chainBlocks[i] = blocks - last_[i].second;
            last_[i] = {cns, blocks};
            w.chainMaxNs[i] = cs.maxBlockNs.exchange(0);
        }
        return w;
    }

private:
    uint64_t cycles_ = 0, ns_ = 0;
    std::vector<std::pair<uint64_t, uint64_t>> last_;
};

// The peak of each input and output since the main thread last took it, for
// the app's meters. The audio thread only raises a peak; the main thread
// takes it and leaves zero.
class Meters {
public:
    Meters(size_t inputs, size_t outputs) : inputs_(inputs), outputs_(outputs), peaks_(new std::atomic<float>[inputs + outputs]) {
        for (size_t c = 0; c < inputs + outputs; ++c)
            peaks_[c].store(0.0f, std::memory_order_relaxed);
    }
    // Audio thread.
    void measure(const float* const* ins, const float* const* outs, uint32_t frames) {
        for (size_t c = 0; c < inputs_ + outputs_; ++c) {
            const float* x = c < inputs_ ? ins[c] : outs[c - inputs_];
            float peak = 0.0f;
            for (uint32_t i = 0; i < frames; ++i)
                peak = std::max(peak, std::fabs(x[i]));
            if (peak > peaks_[c].load(std::memory_order_relaxed))
                peaks_[c].store(peak, std::memory_order_relaxed);
        }
    }
    // Main thread: {"inputs": [{"name", "peak_db"}], "outputs": [...]}, -120 for silence.
    Json take(const Config& config) {
        Json j = {{"inputs", Json::array()}, {"outputs", Json::array()}};
        for (size_t c = 0; c < inputs_ + outputs_; ++c) {
            const float peak = peaks_[c].exchange(0.0f, std::memory_order_relaxed);
            const double db = peak > 1e-6f ? std::max(-120.0, 20 * std::log10(static_cast<double>(peak))) : -120.0;
            const bool in = c < inputs_;
            j[in ? "inputs" : "outputs"].push_back({{"name", in ? config.inputs[c] : config.outputs[c - inputs_]}, {"peak_db", std::round(db * 10) / 10}});
        }
        return j;
    }

private:
    const size_t inputs_, outputs_;
    std::unique_ptr<std::atomic<float>[]> peaks_;
};

// The chains that are running, with their cost over the window.
std::string chainStatus(Engine& engine, const Rig& rig, const Window& w) {
    std::string s;
    char buf[160];
    for (size_t i = 0; i < engine.numChains(); ++i) {
        const auto state = static_cast<ChainState>(engine.stats(i).state.load());
        if (state == ChainState::Idle)
            continue;
        std::snprintf(buf, sizeof buf, "%s%s %s %.0f/%.0fus", s.empty() ? "" : "  ", rig.chainNames[i].c_str(), toString(state), w.chainAvgUs(i),
                      w.chainMaxUs(i));
        s += buf;
    }
    return s;
}

// What a status event says: the numbers of one sample and what the show is doing.
// The tuner event: the reading and what it is measured against.
Json tunerJson(const TunerReading& r, const TunerSettings& s, const std::string& input) {
    Json j;
    j["input"] = input;
    j["signal"] = r.signal;
    j["level_db"] = std::round(r.levelDb * 10) / 10;
    j["note"] = r.note ? Json(r.name) : Json(nullptr);
    j["octave"] = r.octave;
    j["midi"] = r.midi;
    j["cents"] = std::round(r.cents * 10) / 10;
    j["hz"] = std::round(r.hz * 100) / 100;
    j["held"] = r.held;
    j["in_tune"] = r.inTune;
    j["reference"] = s.reference;
    j["range"] = tunerRangeName(s.range);
    j["tolerance"] = s.tolerance;
    j["names"] = s.flats ? "flats" : "sharps";
    return j;
}

Json statusJson(const Window& w, Engine& engine, const Rig& rig, const Show& show, const Config& config) {
    const double periodNs = 1e9 * config.audio.period / config.audio.rate;
    Json j;
    j["dsp"] = {{"avg", 100.0 * w.avgUs() * 1000.0 / periodNs}, {"max", 100.0 * w.maxNs / periodNs}};
    j["margin_ms"] = w.minQueued == INT32_MAX ? Json(nullptr) : Json(1000.0 * std::max(0, w.minQueued) / config.audio.rate);
    j["period_ms"] = periodNs / 1e6;
    j["buffer_ms"] = periodNs / 1e6 * config.audio.periods;
    const int preset = show.preset(), scene = show.scene();
    j["scene"] = {{"index", scene}, {"name", show.sceneName(scene)}};
    j["selected"] = {{"index", preset}, {"name", show.presetName(preset)}, {"chain", show.presetChain(preset)}};
    j["muted"] = show.muted();
    j["display"] = show.display();
    Json leds = Json::array();
    for (int pedal = 1; pedal <= 10; ++pedal)
        leds.push_back(show.led(pedal));
    j["leds"] = leds;
    Json controls = Json::array();
    for (const auto& name : show.controlNames())
        controls.push_back({{"name", name}, {"value", show.value(name)}, {"text", show.valueText(name)}});
    j["controls"] = controls;
    Json chains = Json::array();
    for (size_t i = 0; i < engine.numChains(); ++i)
        chains.push_back({{"name", rig.chainNames[i]},
                          {"state", toString(static_cast<ChainState>(engine.stats(i).state.load()))},
                          {"avg_us", w.chainAvgUs(i)},
                          {"max_us", w.chainMaxUs(i)},
                          {"bypassed", Json::array()}});
    for (size_t k = 0; k < rig.plugins.size(); ++k)
        if (rig.bypassedNow[k].load(std::memory_order_relaxed))
            chains[rig.plugins[k].chain]["bypassed"].push_back(rig.plugins[k].slot);
    j["chains"] = chains;
    return j;
}

// The config's plug-in at an engine chain and slot: chains are numbered scene
// by scene, slots in the order of the chain's blocks.
PluginSpec* specAt(Config& config, int chain, size_t slot) {
    ChainSpec* cs = chainSpec(config, chain);
    return cs && slot < cs->blocks.size() ? &cs->blocks[slot] : nullptr;
}

// Whether the app's DISPLAY is the engine's (":0" and ":0.0" are the same).
bool sameScreen(std::string theirs) {
    const char* env = std::getenv("DISPLAY");
    if (!env || theirs.empty())
        return false;
    std::string ours = env;
    for (std::string* d : {&ours, &theirs}) {
        const size_t colon = d->rfind(':'), dot = d->rfind('.');
        if (dot != std::string::npos && colon != std::string::npos && dot > colon)
            d->erase(dot);
    }
    return ours == theirs;
}

// A wire's end in a request: a plug-in by slot (from 0); "in" or the
// chain's input by name, where a wire starts; an output by name, or out1,
// out2..., where it ends. key: the plug-in's side-chain input.
bool portOf(const Json& j, bool from, bool key, const Config& config, const ChainSpec& cs, Port& out, std::string* error) {
    if (j.is_number_integer()) {
        const long n = j.get<long>();
        if (n < 0 || static_cast<size_t>(n) >= cs.blocks.size()) {
            *error = "chain " + cs.name + " has no plug-in " + std::to_string(n + 1);
            return false;
        }
        out = Port::block(static_cast<int>(n), !from && key);
        return true;
    }
    const std::string name = j.is_string() ? lower(j.get<std::string>()) : "";
    if (from) {
        if (name == "in" || name == lower(config.inputs.at(cs.inputIndex))) {
            out = Port::input();
            return true;
        }
        *error = "a wire starts at the chain's input (" + config.inputs.at(cs.inputIndex) + ") or a plug-in";
        return false;
    }
    for (size_t o = 0; o < config.outputs.size(); ++o)
        if (name == lower(config.outputs[o]) || name == "out" + std::to_string(o + 1)) {
            out = Port::output(static_cast<int>(o));
            return true;
        }
    *error = "a wire ends at a plug-in or one of the outputs (" + [&] {
        std::string all;
        for (const auto& o : config.outputs)
            all += (all.empty() ? "" : ", ") + o;
        return all;
    }() + ")";
    return false;
}

// A wire from a request: its ends, and its gains (level_db, width and pan
// in per cent, muted) over those it has, or a plain wire's.
bool wireOf(const Json& req, const Config& config, const ChainSpec& cs, WireSpec& w, std::string* error) {
    if (!portOf(req.at("from"), true, false, config, cs, w.from, error) || !portOf(req.at("to"), false, req.value("key", false), config, cs, w.to, error))
        return false;
    for (const WireSpec& had : cs.wires)
        if (had.from == w.from && had.to == w.to) {
            w.gains = had.gains;
            w.feedback = had.feedback;
            w.jump = had.jump;
            w.jumpOut = had.jumpOut;
            w.jumpIn = had.jumpIn;
        }
    w.feedback = req.value("feedback", w.feedback);
    w.jump = req.value("jump", w.jump);
    for (auto [key, at] : {std::pair{"jump_out", &w.jumpOut}, std::pair{"jump_in", &w.jumpIn}})
        if (req.contains(key) && req.at(key).is_array() && req.at(key).size() == 2)
            *at = req.at(key).get<std::vector<double>>();
    w.gains.levelDb = req.value("level_db", w.gains.levelDb);
    w.gains.width = req.value("width", w.gains.width * 100.0f) / 100.0f;
    w.gains.pan = req.value("pan", w.gains.pan * 100.0f) / 100.0f;
    w.gains.muted = req.value("muted", w.gains.muted);
    if (w.gains.width < 0 || w.gains.width > 2 || w.gains.pan < -1 || w.gains.pan > 1) {
        *error = "a wire's width is 0 to 200 % and its pan -100 to 100";
        return false;
    }
    w.where = "the app";
    return true;
}

// A plug-in for the app: what it is, whether it is bypassed, and its
// parameters with their values now.
Json pluginJson(const Rig& rig, int k, const PluginSpec& ps, const std::vector<Show::ParamTie>& ties, const EditorHost* editors = nullptr) {
    const RigPlugin& rp = rig.plugins[k];
    Processor* p = rp.processor;
    Json j;
    j["chain"] = rp.chain;
    j["slot"] = rp.slot;
    j["label"] = rp.label;
    j["name"] = p->name();
    j["format"] = ps.kind;
    j["bypassed"] = rig.bypassedNow[k].load(std::memory_order_relaxed);
    j["editor"] = p->hasEditor();
    j["editor_open"] = editors && editors->isOpen(*p);
    Json params = Json::array();
    for (const ParamInfo& info : p->params()) {
        Json q = {{"id", info.id}, {"name", info.name}, {"module", info.module}, {"min", info.min}, {"max", info.max},
                  {"default", info.def}, {"steps", info.steps}};
        double value = info.def;
        const bool known = p->paramValue(info.id, value);
        q["value"] = known ? Json(value) : Json(nullptr);
        q["text"] = known ? p->paramText(info.id, value) : "";
        // What moves it from the pedals or MIDI: the app always shows these.
        Json tied = Json::array();
        for (const Show::ParamTie& t : ties)
            if (t.param == info.id) {
                Json tj;
                if (!t.control.empty())
                    tj = {{"control", t.control}, {"follows", t.follows}, {"pedal", t.pedal}};
                else
                    tj = {{"trigger", t.trigger}};
                tied.push_back(tj);
            }
        if (!tied.empty())
            q["tied"] = tied;
        params.push_back(q);
    }
    j["params"] = params;
    if (ps.shownSet)
        j["shown"] = ps.shown;
    return j;
}

// Each plug-in's state as it is now, for saving what is playing. On the main thread.
void captureStates(const Rig& rig, Config& config) {
    int c = 0;
    for (auto& scene : config.scenes)
        for (auto& chain : scene.chains) {
            const int index = c++;
            for (size_t slot = 0; slot < chain.blocks.size(); ++slot)
                if (const int k = rig.find(index, slot); k >= 0)
                    captureState(*rig.plugins[k].processor, chain.blocks[slot]);
        }
}

// Where an xrun came from: when, how late and slow the block before it
// was, and what the slowest block around it was doing.
std::string describeXrun(uint64_t xruns, const AlsaStats& st, const BlockReport& r, const Rig& rig, double periodUs, unsigned rate) {
    char buf[200];
    const uint32_t ms = st.lastXrunMs.load();
    std::snprintf(buf, sizeof buf, "xrun%s at %u:%04.1f, %s: the block before took %.2f ms and started %.2f ms late (the period is %.2f ms)",
                  xruns > 1 ? (" x" + std::to_string(xruns)).c_str() : "", ms / 60000, (ms % 60000) / 1000.0,
                  st.lastXrunPlayback ? "playback ran dry" : "capture overflowed", st.lastXrunBlockNs.load() / 1e6,
                  1000.0 * st.lastXrunLateFrames.load() / rate, periodUs / 1000.0);
    std::string s = buf;
    std::snprintf(buf, sizeof buf, "; the slowest block around it took %.2f ms:", r.ns / 1e6);
    s += buf;
    for (size_t k = 0; k < r.count; ++k) {
        const auto& c = r.chains[k];
        std::snprintf(buf, sizeof buf, "%s %s %s %.2f ms%s", k ? "," : "", rig.chainNames[c.chain].c_str(), toString(c.state), c.ns / 1e6,
                      c.background ? " (background)" : c.helper ? " (helper)" : "");
        s += buf;
    }
    return s;
}

// Columns of the terminal, or 0 when it is not one.
size_t terminalWidth() {
    winsize w{};
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 ? w.ws_col : 0;
}

// ------------------------------------------------------------ keyboard

struct RawTerminal {
    termios old{};
    bool active = false;
    RawTerminal() {
        if (!isatty(STDIN_FILENO))
            return;
        tcgetattr(STDIN_FILENO, &old);
        termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
    }
    ~RawTerminal() {
        if (active)
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
    }
    int key() const {
        if (!active)
            return -1;
        char c;
        return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
    }
};

// ------------------------------------------------------------ commands

std::string rigPath; // --rig: the rig profile a .cue plays with

bool load(const std::string& path, Config& config) {
    std::string err;
    if (!loadPerformance(path, rigPath, config, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return false;
    }
    if (config.cpuAuto)
        config.audio.cpu = pickAudioCpu();
    // Before any plug-in loads: what it starts inherits this.
    keepOffAudioCore(config.audio.cpu);
    return true;
}

std::unique_ptr<PluginScanner> makeScanner();
Json pluginsJson(const PluginScanner& scanner);
Json scanProgressJson(const PluginScanner& scanner);

// Empty chains the engine keeps ready for chains added while it plays: it
// never adds one once it runs. A chain taken out leaves its own spare.
constexpr size_t kSpareChains = 64;

// What removals took out, by the token the app gave, so an undo can put it
// back. Past 100, the oldest go first.
template <typename T>
struct Kept {
    std::map<std::string, T> items;
    std::deque<std::string> order;
    void keep(const std::string& token, T item) {
        if (token.empty())
            return;
        if (!items.count(token))
            order.push_back(token);
        items[token] = std::move(item);
        if (order.size() > 100) {
            items.erase(order.front());
            order.pop_front();
        }
    }
    const T* find(const std::string& token) const {
        const auto it = items.find(token);
        return it == items.end() ? nullptr : &it->second;
    }
};

// A message from the show about something the app made has no file line
// to start with.
std::string tidy(std::string why) {
    while (why.rfind(": ", 0) == 0)
        why.erase(0, 2);
    return why;
}

struct RunOptions {
    bool noAudio = false;   // --audio none: no sound card, the engine on a timer
    std::string socket;     // --socket PATH; empty: the default
    bool noSocket = false;  // --socket none
    bool takeDevice = true; // the sound card is taken from what else has it; --leave-device: not
};

int cmdRun(const std::string& path, const RunOptions& opts) {
    Config config;
    if (!load(path, config))
        return 2;
    std::string err;
    // Each chain with a preset of its own, as a .cue has them, so chains can
    // be added, moved and taken out while it plays. An INI config whose
    // presets share a chain plays as it is.
    if (Config arranged = config; presetsToChains(arranged, &err))
        config = std::move(arranged);
    Rig rig;
    if (!buildRig(config, rig, &err, kSpareChains)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    Engine& engine = *rig.engine;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "warning: could not lock memory (see cue doctor)\n");

    std::unique_ptr<AudioBackend> audio;
    if (opts.noAudio)
        audio = std::make_unique<NullBackend>();
    else
        audio = std::make_unique<AlsaBackend>();
    // Kept for as long as Cue plays, so a sound server does not take the card back.
    DeviceClaim claim;
    std::vector<std::string> audioNotes;
    const bool take = opts.takeDevice && !opts.noAudio;
    if (take)
        claim.reserve(config.audio.device, audioNotes);
    bool opened = audio->open(config.audio, &err);
    if (!opened && take)
        opened = claim.takeFrom(config.audio.device, [&] { return audio->open(config.audio, &err); }, audioNotes);
    for (const auto& n : audioNotes)
        std::printf("audio: %s\n", n.c_str());
    if (!opened) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    std::printf("audio: %s\n", audio->describe().c_str());

    MidiOutQueue midiOut;
    RigOutput output(rig, [&](const MidiMessage& m) { midiOut.push(m); });
    Show show(output);
    if (!show.load(config, rig.showPlugins(), &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 2;
    }
    for (const auto& w : show.warnings())
        std::fprintf(stderr, "warning: %s\n", w.c_str());

    AlsaMidi midi;
    std::thread midiThread;
    std::atomic<bool> midiRun{true};
    int midiPorts = 0;
    if (config.midi.enabled) {
        if (midi.open("Cue", &err)) {
            const int ins = midiPorts = midi.connectInputFrom(config.midi.in);
            const int outs = config.midi.out.empty() ? 0 : midi.connectOutputTo(config.midi.out);
            std::printf("midi: input from %d port(s) matching '%s', output to %d port(s) matching '%s', profile %s\n", ins,
                        config.midi.in.c_str(), outs, config.midi.out.c_str(), config.midi.profile.c_str());
            if (ins == 0)
                std::fprintf(stderr, "warning: no MIDI input matches '%s' (cue devices lists the ports)\n", config.midi.in.c_str());
            midiThread = std::thread([&] {
                MidiMessage m;
                while (midiRun) {
                    if (midi.read(m, 5))
                        show.onMidi(m);
                    midiOut.flush(midi);
                }
            });
        } else {
            std::fprintf(stderr, "midi: %s (continuing without MIDI)\n", err.c_str());
        }
    }

    // The built-in tuner hears its input before any plug-in.
    Tuner tuner(config.audio.rate);
    tuner.setSettings(config.tuner.settings);
    const size_t tunerInput = config.tuner.inputIndex;

    Meters meters(config.inputs.size(), config.outputs.size());
    Json levels = meters.take(config);

    std::atomic<bool> marked{false};
    audio->start([&](const float* const* ins, float* const* outs, uint32_t frames) {
        if (!marked.load(std::memory_order_relaxed)) {
            markClapAudioThread();
            marked.store(true, std::memory_order_relaxed);
        }
        tuner.write(ins[tunerInput], frames);
        engine.process(ins, outs, frames);
        meters.measure(ins, outs, frames);
    }, &err);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    show.start();

    // The windows plug-in editors open in, on the computer's own screen.
    EditorHost editors;
    // Changes to the chains while they play.
    LiveRig live{config, rig, show, &editors};
    // The plug-ins on this machine, scanned when the app asks.
    auto scanner = makeScanner();
    Kept<Removed> removedPlugins;
    Kept<RemovedChain> removedChains;
    Kept<RemovedScene> removedScenes;
    // The performance as the app sees it: the file's, each chain's number
    // (its index, which stays while it plays), and which plug-ins have a
    // side-chain input to wire into.
    auto livePerformance = [&](Json& out, std::string* why) {
        std::string text;
        if (!performanceText(config, text, why))
            return false;
        out = Json::parse(text);
        Json& scenes = out.at("scenes");
        for (size_t s = 0; s < scenes.size() && s < config.scenes.size(); ++s)
            for (Json& chain : scenes[s].at("chains")) {
                const std::string name = chain.value("name", std::string());
                int id = -1;
                for (const ChainSpec& cs : config.scenes[s].chains)
                    if (lower(cs.name) == lower(name))
                        id = cs.id;
                chain["index"] = id;
                int slot = 0;
                for (Json& block : chain.at("blocks")) {
                    if (Processor* p = rig.at(id, slot++); p && p->hasSidechain())
                        block["has_key"] = true;
                }
            }
        return true;
    };

    // The control socket: the app and cue ctl.
    Service service;
    Window lastSample(engine.numChains());
    auto status = [&] {
        Json j = statusJson(lastSample, engine, rig, show, config);
        j["xruns"] = audio->stats().xruns.load();
        j["levels"] = levels;
        return j;
    };
    std::string savePath = isCueFile(path) ? path : "";
    auto handle = [&](const Json& req, Json& reply) {
        const std::string cmd = req.at("cmd").get<std::string>();
        auto refuse = [&](const std::string& why) {
            reply["ok"] = false;
            reply["error"] = why;
        };
        if (cmd == "hello") {
            reply["cue"] = CUE_VERSION;
            reply["protocol"] = kProtocol;
            reply["performance"] = config.performance;
            reply["file"] = path;
            reply["audio"] = audio->describe();
            reply["audio_settings"] = {{"device", opts.noAudio ? "none" : config.audio.device},
                                       {"rate", config.audio.rate},
                                       {"period", config.audio.period},
                                       {"periods", config.audio.periods}};
            reply["audio_notes"] = audioNotes;
            reply["midi"] = {{"enabled", config.midi.enabled}, {"in", config.midi.in}, {"profile", config.midi.profile}, {"ports", midiPorts}};
        } else if (cmd == "status") {
            reply["status"] = status();
        } else if (cmd == "performance") {
            Json now;
            if (!livePerformance(now, &err))
                return refuse(err);
            reply["performance"] = now;
        } else if (cmd == "select") {
            int index = -1;
            if (req.contains("step")) {
                show.stepPreset(req.at("step").get<int>());
            } else if (req.contains("index")) {
                index = req.at("index").get<int>();
                if (index < 0 || index >= static_cast<int>(show.numPresets()))
                    return refuse("there is no chain number " + std::to_string(index + 1));
                show.selectPreset(index);
            } else if (req.contains("chain") && req.at("chain").is_number_integer()) {
                // By the chain's own number, as performance lists it.
                const int chain = req.at("chain").get<int>();
                for (int i = 0; i < static_cast<int>(show.numPresets()) && index < 0; ++i)
                    if (show.presetChain(i) == chain)
                        index = i;
                if (index < 0)
                    return refuse("there is no chain number " + std::to_string(chain + 1));
                show.selectPreset(index);
            } else {
                const std::string name = req.value("chain", "");
                for (int i = 0; i < static_cast<int>(show.numPresets()) && index < 0; ++i)
                    if (lower(show.presetName(i)) == lower(name))
                        index = i;
                if (index < 0)
                    return refuse("no chain named '" + name + "'");
                show.selectPreset(index);
            }
            reply["selected"] = {{"index", show.preset()}, {"name", show.presetName(show.preset())}, {"chain", show.presetChain(show.preset())}};
        } else if (cmd == "do") {
            if (!show.run(req.at("action").get<std::string>(), &err))
                return refuse(err);
        } else if (cmd == "save") {
            const std::string to = req.value("path", savePath);
            if (to.empty())
                return refuse("this performance came from an INI config: say where to save it as a .cue file");
            if (!isCueFile(to))
                return refuse("a performance is saved as a .cue file");
            Config snapshot = config;
            captureStates(rig, snapshot);
            std::vector<std::string> warnings;
            if (!saveCue(snapshot, to, &err, &warnings))
                return refuse(err);
            savePath = to;
            reply["path"] = to;
            reply["warnings"] = warnings;
            std::printf("\nsaved %s\n", to.c_str());
        } else if (cmd == "tuner") {
            TunerSettings t = config.tuner.settings;
            t.reference = req.value("reference", t.reference);
            if (t.reference < 415 || t.reference > 466)
                throw std::runtime_error("the reference is 415 to 466 Hz");
            if (req.contains("range")) {
                const int range = tunerRange(req.at("range").get<std::string>());
                if (range < 0)
                    throw std::runtime_error("the range is guitar, bass, vocals or any");
                t.range = range;
            }
            t.tolerance = req.value("tolerance", t.tolerance);
            if (t.tolerance <= 0 || t.tolerance > 10)
                throw std::runtime_error("the tolerance is above 0 and at most 10 cents");
            if (req.contains("names"))
                t.flats = req.at("names").get<std::string>() == "flats";
            config.tuner.settings = t;
            tuner.setSettings(t);
            reply["tuner"] = tunerJson(tuner.reading(), t, config.inputs.at(tunerInput));
        } else if (cmd == "plugin" || cmd == "bypass" || cmd == "param" || cmd == "editor" || cmd == "shown") {
            const int chain = req.at("chain").get<int>();
            const size_t slot = req.at("slot").get<size_t>();
            const int k = rig.find(chain, slot);
            PluginSpec* ps = specAt(config, chain, slot);
            if (k < 0 || !ps)
                return refuse("there is no plug-in " + std::to_string(slot + 1) + " in chain number " + std::to_string(chain + 1));
            Processor* p = rig.plugins[k].processor;
            if (cmd == "bypass") {
                // Saved with the performance, like a change made in the file.
                const bool bypassed = req.at("bypassed").get<bool>();
                output.setBypass(chain, slot, bypassed);
                ps->bypassed = bypassed;
            } else if (cmd == "param") {
                // "param", not "id": a request's "id" is its number on the socket.
                const uint32_t id = req.at("param").get<uint32_t>();
                double value = 0;
                if (req.contains("text")) {
                    // Typed: as the plug-in reads it, else a plain number
                    // (a VST3 plug-in's in %, as the app shows it).
                    const std::string typed = req.at("text").get<std::string>();
                    if (!p->paramFromText(id, typed, value)) {
                        char* end = nullptr;
                        const double n = std::strtod(typed.c_str(), &end);
                        if (end == typed.c_str())
                            return refuse(p->name() + " cannot read '" + typed + "' as a value");
                        const bool vst3 = dynamic_cast<Vst3Processor*>(p) != nullptr;
                        value = vst3 ? n / 100.0 : n;
                    }
                    for (const ParamInfo& info : p->params())
                        if (info.id == id)
                            value = std::clamp(value, info.min, info.max);
                } else {
                    value = req.at("value").get<double>();
                }
                if (!p->setParam(id, value))
                    return refuse(p->name() + " did not take the change (too many at once)");
                // The plug-in's state keeps it; a param line in the file
                // would set it back when the performance loads, so it follows.
                for (auto& [pid, v] : ps->params)
                    if (pid == id)
                        v = value;
                reply["text"] = p->paramText(id, value);
                reply["value"] = value;
                return;
            } else if (cmd == "editor") {
                // The plug-in's own window, on the computer's screen, not the
                // app's: the app may be on another machine.
                const bool wanted = req.value("open", true);
                if (!wanted) {
                    editors.close(*p);
                } else {
                    const std::string title = rig.plugins[k].label + " — " + rig.chainNames.at(chain);
                    // The app's window, when the app is on this screen: the
                    // editor stays over it.
                    unsigned long above = 0;
                    if (req.contains("above") && sameScreen(req.value("display", std::string())))
                        above = req.at("above").get<unsigned long>();
                    std::string why;
                    if (!editors.open(*p, title, &why, above))
                        return refuse(why);
                }
            } else if (cmd == "shown") {
                // The parameters the app shows for it, in its order: kept for the next save.
                ps->shown = req.at("params").get<std::vector<uint32_t>>();
                ps->shownSet = true;
            }
            reply["plugin"] = pluginJson(rig, k, *ps, show.ties(chain, slot), &editors);
        } else if (cmd == "add" || cmd == "remove" || cmd == "restore" || cmd == "wire" || cmd == "unwire" || cmd == "wires" || cmd == "place") {
            // restore: the chain is the one it was taken out of.
            const std::string token = req.value("token", std::string());
            const Removed* taken = removedPlugins.find(token);
            if (cmd == "restore" && !taken)
                return refuse("nothing taken out is kept as '" + token + "'");
            const int chain = cmd == "restore" ? taken->chain : req.at("chain").get<int>();
            ChainSpec* cs = chainSpec(config, chain);
            if (!cs)
                return refuse("there is no chain number " + std::to_string(chain + 1));
            std::string why;
            if (cmd == "add") {
                // As a plugin line has it, or by its parts.
                PluginSpec spec;
                const Json& pj = req.at("plugin");
                if (pj.is_string()) {
                    if (!pluginFromText(pj.get<std::string>(), spec, &why))
                        return refuse(why);
                } else {
                    std::string text = pj.value("format", std::string("clap")) + ":";
                    if (pj.contains("args")) {
                        for (const auto& a : pj.at("args"))
                            text += a.get<std::string>() + " ";
                    } else {
                        text += pj.value("path", std::string()) + (pj.contains("id") ? "#" + pj.at("id").get<std::string>() : "");
                    }
                    if (!pluginFromText(text, spec, &why))
                        return refuse(why);
                }
                spec.label = req.value("label", spec.label);
                spec.bypassed = req.value("bypassed", false);
                spec.muteOnBypass = config.muteOnBypass;
                if (req.contains("x") || req.contains("y")) {
                    spec.placed = true;
                    spec.x = req.value("x", 0.0);
                    spec.y = req.value("y", 0.0);
                }
                WireSpec on;
                const bool onWire = req.contains("on");
                if (onWire && !wireOf(req.at("on"), config, *cs, on, &why))
                    return refuse(why);
                size_t slot = 0;
                if (!addPlugin(live, chain, spec, onWire ? &on : nullptr, &slot, &why))
                    return refuse(why);
                reply["slot"] = slot;
            } else if (cmd == "remove") {
                Removed removed;
                if (!removePlugin(live, chain, req.at("slot").get<size_t>(), &removed, &why))
                    return refuse(why);
                removedPlugins.keep(token, std::move(removed));
            } else if (cmd == "restore") {
                if (!restorePlugin(live, *taken, &why))
                    return refuse(why);
                reply["slot"] = taken->slot;
            } else if (cmd == "wire" || cmd == "unwire") {
                WireSpec w;
                if (!wireOf(req, config, *cs, w, &why))
                    return refuse(why);
                const bool had = std::any_of(cs->wires.begin(), cs->wires.end(), [&](const WireSpec& o) { return o.from == w.from && o.to == w.to; });
                if (!(cmd == "wire" ? setWire(live, chain, w, &why) : removeWire(live, chain, w.from, w.to, &why)))
                    return refuse(why);
                if (cmd == "wire" && had) {
                    // Only its gains: the wire, not the whole performance.
                    Json wj = {{"from", req.at("from")}, {"to", req.at("to")}, {"level_db", w.gains.levelDb},
                               {"width", std::round(w.gains.width * 1000) / 10}, {"pan", std::round(w.gains.pan * 1000) / 10}, {"muted", w.gains.muted},
                               {"jump", w.jump}};
                    if (w.jumpOut.size() == 2)
                        wj["jump_out"] = w.jumpOut;
                    if (w.jumpIn.size() == 2)
                        wj["jump_in"] = w.jumpIn;
                    if (w.to.key)
                        wj["key"] = true;
                    reply["wire"] = wj;
                    service.publish("wire", {{"chain", chain}, {"wire", wj}});
                    return;
                }
            } else if (cmd == "wires") {
                // The chain's wires all at once, as the performance lists them.
                std::vector<WireSpec> wires;
                for (const Json& wj : req.at("wires")) {
                    WireSpec w;
                    if (!wireOf(wj, config, *cs, w, &why))
                        return refuse(why);
                    wires.push_back(w);
                }
                if (!setWires(live, chain, wires, &why))
                    return refuse(why);
            } else {
                // Only where it is drawn: the app that asked knows.
                if (!placePlugin(live, chain, req.at("slot").get<size_t>(), req.at("x").get<double>(), req.at("y").get<double>(), &why))
                    return refuse(why);
                return;
            }
            // Everyone watching sees the chain as it is now.
            Json now;
            if (livePerformance(now, &why)) {
                reply["performance"] = now;
                service.publish("performance", now);
            }
        } else if (cmd == "add-chain" || cmd == "remove-chain" || cmd == "restore-chain" || cmd == "set-chain" || cmd == "move-chain" ||
                   cmd == "add-scene" || cmd == "remove-scene" || cmd == "restore-scene" || cmd == "set-scene" || cmd == "move-scene") {
            const std::string token = req.value("token", std::string());
            auto text = [&](const char* key) { return std::optional<std::string>(req.at(key).get<std::string>()); };
            // A number, or none (null).
            auto number = [&](const char* key, int none) { return std::optional<int>(req.at(key).is_null() ? none : req.at(key).get<int>()); };
            const auto warningsBefore = show.warnings();
            std::string why;
            bool ok = false;
            if (cmd == "add-chain") {
                NewChain what;
                what.scene = req.at("scene").get<int>();
                what.at = req.value("at", -1);
                what.name = req.value("name", std::string());
                what.copy = req.value("copy", -1);
                what.input = req.value("input", std::string());
                int id = -1;
                if ((ok = addChain(live, what, &id, &why)))
                    reply["chain"] = id;
            } else if (cmd == "remove-chain") {
                RemovedChain removed;
                if ((ok = removeChain(live, req.at("chain").get<int>(), &removed, &why)))
                    removedChains.keep(token, std::move(removed));
            } else if (cmd == "restore-chain") {
                const RemovedChain* removed = removedChains.find(token);
                if (!removed)
                    return refuse("nothing taken out is kept as '" + token + "'");
                int id = -1;
                if ((ok = restoreChain(live, *removed, &id, &why)))
                    reply["chain"] = id;
            } else if (cmd == "set-chain") {
                ChainSettings set;
                if (req.contains("name"))
                    set.name = text("name");
                if (req.contains("display"))
                    set.display = text("display");
                if (req.contains("warm"))
                    set.warm = text("warm");
                if (req.contains("pedal"))
                    set.pedal = number("pedal", 0);
                if (req.contains("program"))
                    set.program = number("program", -1);
                if (req.contains("on_select"))
                    set.onSelect = req.at("on_select").get<std::vector<std::string>>();
                ok = setChain(live, req.at("chain").get<int>(), set, &why);
            } else if (cmd == "move-chain") {
                ok = moveChain(live, req.at("chain").get<int>(), req.at("scene").get<int>(), req.value("at", -1), &why);
            } else if (cmd == "add-scene") {
                NewScene what;
                what.at = req.value("at", -1);
                what.name = req.value("name", std::string());
                what.copy = req.value("copy", -1);
                int index = -1;
                std::vector<int> ids;
                if ((ok = addScene(live, what, &index, &ids, &why))) {
                    reply["scene"] = index;
                    reply["chains"] = ids;
                }
            } else if (cmd == "remove-scene") {
                RemovedScene removed;
                if ((ok = removeScene(live, req.at("scene").get<int>(), &removed, &why)))
                    removedScenes.keep(token, std::move(removed));
            } else if (cmd == "restore-scene") {
                const RemovedScene* removed = removedScenes.find(token);
                if (!removed)
                    return refuse("nothing taken out is kept as '" + token + "'");
                if ((ok = restoreScene(live, *removed, &why)))
                    reply["scene"] = std::min<size_t>(removed->at, config.scenes.size() - 1);
            } else if (cmd == "set-scene") {
                SceneSettings set;
                if (req.contains("name"))
                    set.name = text("name");
                if (req.contains("display"))
                    set.display = text("display");
                if (req.contains("pedal"))
                    set.pedal = number("pedal", 0);
                if (req.contains("program"))
                    set.program = number("program", -1);
                ok = setScene(live, req.at("scene").get<int>(), set, &why);
            } else {
                ok = moveScene(live, req.at("scene").get<int>(), req.value("at", -1), &why);
            }
            if (!ok)
                return refuse(tidy(why));
            // What the show now thinks may be a mistake: a pedal two things use.
            Json warnings = Json::array();
            for (const auto& w : show.warnings())
                if (std::find(warningsBefore.begin(), warningsBefore.end(), w) == warningsBefore.end())
                    warnings.push_back(tidy(w));
            reply["warnings"] = warnings;
            Json now;
            if (livePerformance(now, &why)) {
                reply["performance"] = now;
                service.publish("performance", now);
            }
        } else if (cmd == "plugins") {
            // The user's folders first, then a scan if asked for.
            std::string why;
            if (req.contains("folders")) {
                std::vector<std::string> folders;
                for (const auto& d : req.at("folders"))
                    folders.push_back(d.get<std::string>());
                if (!scanner->setExtraFolders(folders, &why))
                    return refuse(why);
            }
            const std::string scan = req.value("scan", std::string());
            if (!scan.empty() && !scanner->start(scan, &why))
                return refuse(why);
            reply["plugins"] = pluginsJson(*scanner);
        } else if (cmd == "quit") {
            g_quit = true;
        } else {
            refuse("unknown command '" + cmd +
                   "' (hello, status, performance, select, do, save, tuner, plugin, bypass, param, editor, shown, add, remove, restore, wire, unwire, wires, "
                   "place, add-chain, remove-chain, restore-chain, set-chain, move-chain, add-scene, remove-scene, restore-scene, set-scene, move-scene, "
                   "plugins, quit, subscribe, unsubscribe)");
        }
    };
    if (!opts.noSocket) {
        const std::string socketPath = opts.socket.empty() ? defaultSocketPath() : opts.socket;
        if (service.listen(socketPath, handle, &err))
            std::printf("control: %s\n", socketPath.c_str());
        else
            std::fprintf(stderr, "control socket: %s (continuing without it)\n", err.c_str());
    }
    std::printf("keys: 1-9 select preset, n/p next/previous, q quit\n");

    RawTerminal term;
    // A line for the terminal and the app's log.
    auto log = [&](const char* level, const std::string& text) {
        std::printf("%s%s\n", term.active ? "\r\033[K" : "", text.c_str());
        service.publish("log", {{"level", level}, {"text", text}});
    };
    Sampler sampler;
    Window window(engine.numChains());
    auto nextSample = Clock::now(), nextStatus = Clock::now();
    const double periodUs = 1e6 * config.audio.period / config.audio.rate;
    bool reportedSched = false;
    uint64_t reportedXruns = 0, reportedStartup = 0;

    std::vector<int> editorFds;
    std::vector<ParamEdit> editorEdits;
    while (!g_quit) {
        editorFds.clear();
        editors.fds(editorFds);
        // An editor redraws as soon as something happens on its screen,
        // rather than at the next turn of the loop.
        service.poll(10, editorFds);
        rig.pumpMainThread();
        editors.pump();
        // What a plug-in's own editor changed, for the app to undo.
        for (const RigPlugin& rp : rig.plugins) {
            editorEdits.clear();
            rp.processor->takeEditorEdits(editorEdits);
            if (editorEdits.empty() || !rp.processor->editorOpen())
                continue;
            const std::vector<ParamInfo> infos = rp.processor->params();
            PluginSpec* ps = specAt(config, rp.chain, rp.slot);
            for (const ParamEdit& e : editorEdits) {
                std::string name;
                for (const ParamInfo& info : infos)
                    if (info.id == e.id)
                        name = info.name;
                if (ps)
                    for (auto& [pid, v] : ps->params)
                        if (pid == e.id)
                            v = e.to;
                if (service.subscribed("edited"))
                    service.publish("edited", {{"chain", rp.chain}, {"slot", rp.slot}, {"param", e.id}, {"name", name}, {"from", e.from},
                                               {"to", e.to}, {"text", rp.processor->paramText(e.id, e.to)}});
            }
        }
        // What edits replaced, now that the audio thread is done with it.
        engine.collect();
        // A plug-in scan: how far it is, and what it found once it ends.
        if (scanner->takeProgressed() && service.subscribed("scan"))
            service.publish("scan", scanProgressJson(*scanner));
        if (scanner->takeFinished() && service.subscribed("plugins"))
            service.publish("plugins", pluginsJson(*scanner));
        EngineEvent ev;
        while (engine.nextEvent(ev)) {
        }
        const int k = term.key();
        if (k > 0 && !show.onKey(static_cast<char>(k))) {
            if (k == 'q')
                break;
            if (k >= '1' && k <= '9')
                show.selectPreset(k - '1');
            if (k == 'n')
                show.stepPreset(+1);
            if (k == 'p')
                show.stepPreset(-1);
        }

        const AlsaStats& st = audio->stats();
        if (st.failed) {
            log("error", "audio device failed and could not be recovered");
            break;
        }
        if (!reportedSched && st.cycles > 0) {
            reportedSched = true;
            std::printf("audio thread: %s, %s; %zu helper thread(s)%s\n", st.realtime ? "real-time" : "NOT real-time (see cue doctor)",
                        st.pinned ? ("pinned to CPU " + std::to_string(config.audio.cpu)).c_str() : "not pinned", engine.numWorkers(),
                        engine.numWorkers() == 0 ? "" : engine.realtimeWorkers() == static_cast<int>(engine.numWorkers()) ? ", real-time" : ", NOT real-time");
        }
        if (st.startupXruns.load() > reportedStartup) {
            reportedStartup = st.startupXruns.load();
            char buf[200];
            std::snprintf(buf, sizeof buf, "start-up: the sound card stalled as it started (%s at block %llu), before anything was heard; not counted as an xrun",
                          st.lastXrunPlayback ? "playback ran dry" : "capture overflowed", static_cast<unsigned long long>(st.lastXrunCycle.load()));
            log("info", buf);
        }
        // Each xrun gets its own line, with the slowest block around it.
        BlockReport report;
        while (engine.nextBlockReport(report)) {
            const uint64_t xruns = st.xruns.load();
            if (xruns > reportedXruns) {
                log("warning", describeXrun(xruns - reportedXruns, st, report, rig, periodUs, config.audio.rate));
                reportedXruns = xruns;
            }
        }
        const auto now = Clock::now();
        if (now >= nextSample) {
            // About 30 times a second: a status event, and the terminal's numbers.
            nextSample += std::chrono::milliseconds(33);
            if (nextSample < now)
                nextSample = now + std::chrono::milliseconds(33);
            lastSample = sampler.take(st, engine);
            window.add(lastSample);
            levels = meters.take(config);
            if (service.subscribed("status"))
                service.publish("status", status());
            // The tuner listens while someone looks at it.
            tuner.setListening(service.subscribed("tuner"));
            if (tuner.listening())
                service.publish("tuner", tunerJson(tuner.reading(), tuner.settings(), config.inputs.at(tunerInput)));
        }
        if (now >= nextStatus) {
            nextStatus += std::chrono::milliseconds(500);
            engine.requestBlockReport();
            // The numbers first, and no wider than the terminal: a line that
            // wraps cannot be redrawn in place.
            char margin[32] = "-";
            if (window.minQueued != INT32_MAX)
                std::snprintf(margin, sizeof margin, "%.2f ms", 1000.0 * std::max(0, window.minQueued) / config.audio.rate);
            char head[128];
            std::snprintf(head, sizeof head, "dsp %3.0f%% (max %3.0f%%) margin %s xruns %llu | ", 100.0 * window.avgUs() / periodUs,
                          100.0 * window.maxNs / 1000.0 / periodUs, margin, static_cast<unsigned long long>(st.xruns.load()));
            std::string line = head + show.status() + " | " + chainStatus(engine, rig, window);
            window = Window(engine.numChains());
            const size_t width = term.active ? terminalWidth() : 0;
            if (width > 1 && line.size() > width - 1)
                line.resize(width - 1);
            std::printf("%s%s\033[K%s", term.active ? "\r" : "", line.c_str(), term.active ? "" : "\n");
            // A plug-in that took most of a period in one block gets its own
            // line, so a click can be traced to what caused it.
            for (size_t i = 0; i < engine.numChains(); ++i) {
                size_t slot = 0;
                uint32_t slotNs = 0;
                if (engine.chain(i).takeSlowest(slot, slotNs) && slotNs / 1000.0 > 0.8 * periodUs) {
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "slow block: %s / %s took %.1f ms (the period is %.2f ms)", rig.chainNames[i].c_str(),
                                  engine.chain(i).slot(slot).name().c_str(), slotNs / 1e6, periodUs / 1000.0);
                    if (term.active)
                        std::printf("\n");
                    log("warning", buf);
                }
            }
            std::fflush(stdout);
        }
    }
    std::printf("\nstopping\n");
    // The editors go before the plug-ins they belong to.
    editors.closeAll();
    service.close();
    audio->stop();
    midiRun = false;
    if (midiThread.joinable())
        midiThread.join();
    return 0;
}

// Plucked saw notes at about -12 dBFS, two per second: bass-like test input.
struct BassInput {
    double rate;
    uint64_t n = 0;
    float operator()() {
        static const float notes[] = {41.2f, 55.0f, 49.0f, 36.7f};
        const uint64_t period = static_cast<uint64_t>(rate / 2);
        const uint64_t i = n / period, t = n % period;
        const float f = notes[i % 4];
        const float phase = std::fmod(static_cast<float>(t) * f / static_cast<float>(rate), 1.0f);
        const float env = std::exp(-static_cast<float>(t) / static_cast<float>(rate * 0.6));
        ++n;
        return 0.25f * env * (2.0f * phase - 1.0f);
    }
};

// One WAV channel per output.
void writeWav(const std::string& path, const std::vector<std::vector<float>>& channels, unsigned rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t frames = static_cast<uint32_t>(channels.at(0).size());
    const uint32_t n = static_cast<uint32_t>(channels.size());
    f.write("RIFF", 4);
    u32(36 + frames * 4 * n);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(static_cast<uint16_t>(n));
    u32(rate);
    u32(rate * 4 * n);
    u16(static_cast<uint16_t>(4 * n));
    u16(32);
    f.write("data", 4);
    u32(frames * 4 * n);
    for (uint32_t i = 0; i < frames; ++i)
        for (const auto& c : channels)
            f.write(reinterpret_cast<const char*>(&c[i]), 4);
}

int cmdBench(const std::string& path, double switchSeconds, double totalSeconds, const std::string& wavPath, bool paced, const std::string& warm,
             int workers) {
    Config config;
    if (!load(path, config))
        return 2;
    if (workers >= 0)
        config.workers = workers;
    Rig rig;
    std::string err;
    if (!buildRig(config, rig, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    Engine& engine = *rig.engine;
    const uint32_t frames = config.audio.period;
    const double periodUs = 1e6 * frames / config.audio.rate;
    if (!warm.empty())
        config.warm = warm;

    // Measure under the same conditions as live: real-time priority on the
    // audio CPU, if allowed.
    double worstUs = 0;
    std::vector<double> aloneUs, blockUs;
    uint64_t othersProcessed = 0;
    std::vector<std::vector<float>> wav(config.outputs.size());
    bool realtime = false, pinned = false;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        enableFlushToZero();
        markClapAudioThread();
        sched_param sp{};
        sp.sched_priority = config.audio.rtPriority;
        realtime = config.audio.rtPriority > 0 && pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
        if (config.audio.cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(config.audio.cpu, &set);
            pinned = pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
        }
        BassInput input{static_cast<double>(config.audio.rate)};
        // Every input hears the same bass.
        std::vector<float> in(frames);
        std::vector<const float*> ins(config.inputs.size(), in.data());
        std::vector<std::vector<float>> o(config.outputs.size(), std::vector<float>(frames));
        std::vector<float*> outs;
        for (auto& b : o)
            outs.push_back(b.data());
        const size_t blocksPerSwitch = std::max<size_t>(1, static_cast<size_t>(switchSeconds * config.audio.rate / frames));
        const size_t total = static_cast<size_t>(totalSeconds * config.audio.rate / frames);
        // Phase 1: chain 1 alone, nothing warm, to show what one chain costs
        // with every other chain loaded but idle.
        const size_t aloneBlocks = std::min(total, static_cast<size_t>(3.0 * config.audio.rate / frames));
        // Paced like live audio: one block per period. Unpaced, a real-time
        // thread that never sleeps is throttled by the kernel (sched_rt_runtime_us),
        // which shows up as false 50 ms stalls.
        timespec deadline{};
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        const long periodNs = static_cast<long>(1e9 * frames / config.audio.rate);
        auto runBlock = [&](std::vector<double>& times) {
            if (paced) {
                deadline.tv_nsec += periodNs;
                while (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_nsec -= 1000000000L;
                    deadline.tv_sec++;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
            }
            for (auto& v : in)
                v = input();
            const auto t0 = Clock::now();
            engine.process(ins.data(), outs.data(), frames);
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            times.push_back(us);
            worstUs = std::max(worstUs, us);
            if (!wavPath.empty())
                for (size_t k = 0; k < o.size(); ++k)
                    wav[k].insert(wav[k].end(), o[k].begin(), o[k].end());
        };
        engine.switchTo(0);
        for (size_t b = 0; b < aloneBlocks && !g_quit; ++b) {
            runBlock(aloneUs);
            EngineEvent ev;
            while (engine.nextEvent(ev)) {
            }
        }
        for (size_t i = 0; i < engine.numChains(); ++i)
            othersProcessed += i == 0 ? 0 : engine.stats(i).processBlocks.load();

        // Phase 2: switch through every chain, with the warm setting from the config.
        if (config.warm == "next" && engine.numChains() > 1)
            engine.setWarm(1);
        int next = 1;
        for (size_t b = 0; b + aloneBlocks < total && !g_quit; ++b) {
            if (b % blocksPerSwitch == 0 && engine.numChains() > 1) {
                engine.switchTo(next);
                next = (next + 1) % static_cast<int>(engine.numChains());
            }
            runBlock(blockUs);
            // Events are drained on this thread here; live, the main thread does it.
            EngineEvent ev;
            while (engine.nextEvent(ev))
                if (ev.type == EngineEvent::Type::ChainActive && config.warm == "next" && engine.numChains() > 1)
                    engine.setWarm((ev.chain + 1) % static_cast<int>(engine.numChains()));
        }
        done = true;
    });
    // Plug-in callbacks and timers, as cue run services them.
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rig.pumpMainThread();
    }
    worker.join();

    auto stats = [&](std::vector<double> v, const char* what) {
        if (v.empty())
            return;
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))]; };
        std::printf("%-34s median %5.0f us (%3.0f%%)  99%% %5.0f us (%3.0f%%)  99.9%% %5.0f us (%3.0f%%)  worst %5.0f us (%3.0f%%)\n", what,
                    pct(0.5), 100 * pct(0.5) / periodUs, pct(0.99), 100 * pct(0.99) / periodUs, pct(0.999), 100 * pct(0.999) / periodUs,
                    v.back(), 100 * v.back() / periodUs);
    };
    std::printf("\nbench: %u-frame blocks (budget %.0f us), %s, %s, %zu helper thread(s)\n", frames, periodUs, realtime ? "real-time" : "not real-time",
                pinned ? ("CPU " + std::to_string(config.audio.cpu)).c_str() : "unpinned", engine.numWorkers());
    stats(aloneUs, ("chain 1 alone, " + std::to_string(engine.numChains() - 1) + " others idle:").c_str());
    std::printf("%-34s %llu blocks processed by the idle chains (should be 0)\n", "", static_cast<unsigned long long>(othersProcessed));
    char what[96];
    std::snprintf(what, sizeof what, "switching every %.1f s, warm %s:", switchSeconds, config.warm.c_str());
    stats(blockUs, what);
    const uint64_t totalBlocks = aloneUs.size() + blockUs.size();
    std::printf("\n%-24s %10s %12s %12s %10s  %s\n", "chain", "blocks", "avg us/blk", "worst us", "share", "slowest plug-in in one block");
    for (size_t i = 0; i < engine.numChains(); ++i) {
        ChainStats& st = engine.stats(i);
        const uint64_t blocks = st.processBlocks.load();
        size_t slot = 0;
        uint32_t slotNs = 0;
        char slowest[96] = "";
        if (engine.chain(i).takeSlowest(slot, slotNs))
            std::snprintf(slowest, sizeof slowest, "%s %.0f us", engine.chain(i).slot(slot).name().c_str(), slotNs / 1000.0);
        std::printf("%-24s %10llu %12.1f %12.1f %9.0f%%  %s\n", rig.chainNames[i].c_str(), static_cast<unsigned long long>(blocks),
                    blocks ? st.processNs.load() / 1000.0 / blocks : 0.0, st.maxBlockNs.load() / 1000.0, 100.0 * blocks / totalBlocks, slowest);
    }
    std::printf("\n'share' is the fraction of blocks in which the chain was processed at all. Idle chains cost nothing.\n");
    if (worstUs > periodUs)
        std::printf("WARNING: the worst block took longer than the period; live, that would be a dropout.\n");
    if (!wavPath.empty()) {
        writeWav(wavPath, wav, config.audio.rate);
        std::string order;
        for (size_t k = 0; k < config.outputs.size(); ++k)
            order += (k ? ", " : "") + config.outputs[k];
        std::printf("wrote %s (a channel per output: %s)\n", wavPath.c_str(), order.c_str());
    }
    return 0;
}

int cmdDevices() {
    std::printf("Sound cards:\n");
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char* name = nullptr;
        char* longName = nullptr;
        snd_card_get_name(card, &name);
        snd_card_get_longname(card, &longName);
        std::printf("  hw:%d  %s  (%s)\n", card, name ? name : "?", longName ? longName : "");
        free(name);
        free(longName);
    }
    AlsaMidi midi;
    std::string err;
    if (!midi.open("Cue devices", &err)) {
        std::printf("MIDI: %s\n", err.c_str());
        return 0;
    }
    std::printf("MIDI ports:\n");
    for (const auto& p : midi.listPorts())
        std::printf("  %3d:%-2d  %-28s %-28s %s%s\n", p.client, p.port, p.clientName.c_str(), p.portName.c_str(), p.canRead ? "in " : "",
                    p.canWrite ? "out" : "");
    return 0;
}

// Loads the config and every plug-in, compiles the mappings, and prints
// what the performance holds. No audio or MIDI is opened.
int cmdCheck(const std::string& path, bool loadPlugins) {
    Config config;
    if (!load(path, config))
        return 2;
    Rig rig;
    std::string err;
    std::vector<ShowPlugin> plugins;
    size_t chains = 0;
    if (loadPlugins) {
        if (!buildRig(config, rig, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        plugins = rig.showPlugins();
        chains = rig.chainNames.size();
    } else {
        // Plug-ins named by their name = lines, or their file name.
        for (const auto& scene : config.scenes)
            for (const auto& c : scene.chains) {
                size_t slot = 0;
                std::vector<std::string> labels;
                for (const PluginSpec* ps : chainPlugins(c)) {
                    std::string label = ps->label;
                    if (label.empty())
                        label = ps->kind == "builtin" ? ps->args[0] : std::filesystem::path(ps->path).stem().string();
                    plugins.push_back({static_cast<int>(chains), slot++, label, ps->midi, ps->bypassed});
                    labels.push_back(label);
                }
                std::vector<std::pair<size_t, size_t>> feeds;
                if (!chainSidechains(c, labels, feeds, &err)) {
                    std::fprintf(stderr, "config: %s\n", err.c_str());
                    return 2;
                }
                ++chains;
            }
    }
    struct NoOutput : ShowOutput {
        void switchTo(int) override {}
        void setWarm(int) override {}
        void setBypass(int, size_t, bool) override {}
        void setParam(int, size_t, uint32_t, double) override {}
        void setMute(bool) override {}
        void sendMidi(const MidiMessage&) override {}
        void forwardMidi(int, size_t, const MidiMessage&) override {}
    } none;
    Show show(none);
    if (!show.load(config, plugins, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 2;
    }
    show.start(); // values as the first preset leaves them
    std::printf("\nperformance%s%s: %zu scene(s), %zu chain(s), %zu preset(s)\n", config.performance.empty() ? "" : " ",
                config.performance.c_str(), config.scenes.size(), chains, show.numPresets());
    int presetNo = 0;
    for (const auto& scene : config.scenes) {
        std::printf("scene %s%s\n", scene.name.c_str(), scene.pedal ? (", pedal " + std::to_string(scene.pedal)).c_str() : "");
        for (const auto& c : scene.chains)
            std::printf("  chain %s: %zu plug-in(s)%s; wires %s\n", c.name.c_str(), chainPlugins(c).size(),
                        config.inputs.size() > 1 ? (", from " + config.inputs.at(c.inputIndex)).c_str() : "", wireList(c, config.outputs).c_str());
        for (const auto& p : scene.presets)
            std::printf("  preset %d %s: chain %s%s%s, %zu action(s)\n", ++presetNo, p.name.c_str(), p.chain.c_str(),
                        p.pedal ? (", pedal " + std::to_string(p.pedal)).c_str() : "", p.display.empty() ? "" : (", shows " + p.display).c_str(),
                        p.actions.size());
    }
    for (const auto& c : config.controls) {
        std::string values;
        for (const auto& l : c.labels)
            values += (values.empty() ? "" : ", ") + l;
        if (values.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g to %g", c.min, c.max);
            values = buf;
        }
        std::printf("control %s: %s, starts at %s%s\n", c.name.c_str(), values.c_str(), show.valueText(c.name).c_str(),
                    c.pedal ? (", pedal " + std::to_string(c.pedal)).c_str() : "");
    }
    for (const auto& t : config.triggers)
        std::printf("trigger %s: %zu input(s), %zu action(s)\n", t.name.c_str(), t.inputs.size(), t.actions.size());
    for (const auto& g : config.groups)
        std::printf("group %s: %zu action(s)\n", g.name.c_str(), g.actions.size());
    for (const auto& w : show.warnings())
        std::printf("warning: %s\n", w.c_str());
    std::printf(loadPlugins ? "config OK\n" : "config OK (plug-ins not loaded)\n");
    return 0;
}

int cmdParams(const std::string& spec) {
    const auto hash = spec.rfind('#');
    const std::string path = hash == std::string::npos ? spec : spec.substr(0, hash);
    const std::string id = hash == std::string::npos ? "" : spec.substr(hash + 1);
    std::string err;
    std::unique_ptr<Processor> p;
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".vst3") == 0)
        p = Vst3Processor::load(path, id, &err);
    else
        p = ClapProcessor::load(path, id, &err);
    if (!p || !p->activate(48000, 64)) {
        std::fprintf(stderr, "%s: %s\n", spec.c_str(), err.empty() ? "could not activate" : err.c_str());
        return 1;
    }
    const auto params = p->params();
    std::printf("%s: %zu parameter(s)%s\n", p->name().c_str(), params.size(),
                dynamic_cast<Vst3Processor*>(p.get()) ? " (VST3 values are 0 to 1)" : "");
    std::printf("%10s  %-32s %10s %10s %10s\n", "id", "name", "min", "max", "default");
    for (const auto& i : params)
        std::printf("%10u  %-32s %10g %10g %10g%s\n", i.id, i.name.c_str(), i.min, i.max, i.def,
                    i.steps > 0 ? ("  " + std::to_string(i.steps + 1) + " steps").c_str() : "");
    p->deactivate();
    return 0;
}

int cmdImportRpp(const std::string& rppPath, std::string outDir, const std::vector<std::string>& search, std::string mapping, bool scan) {
    namespace fs = std::filesystem;
    std::ifstream f(rppPath);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", rppPath.c_str());
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string project = fs::path(rppPath).stem().string();
    if (outDir.empty())
        outDir = expandPath("~/cue/" + project);
    if (!mapping.empty()) {
        std::error_code ec;
        const auto abs = fs::absolute(mapping, ec);
        if (!fs::exists(abs, ec)) {
            std::fprintf(stderr, "mapping file %s not found\n", mapping.c_str());
            return 1;
        }
        mapping = abs.string();
    }
    std::printf("looking for plug-ins%s...\n", scan ? " (opening native .clap files if needed)" : "");
    SystemLocator locator(search, "~/.config/REAPER", scan);
    Import result;
    std::string err;
    if (!convertRpp(ss.str(), project, locator, mapping, result, &err)) {
        std::fprintf(stderr, "%s: %s\n", rppPath.c_str(), err.c_str());
        return 1;
    }
    std::error_code ec;
    fs::create_directories(fs::path(outDir) / "states", ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", outDir.c_str(), ec.message().c_str());
        return 1;
    }
    for (const auto& file : result.files) {
        std::ofstream o(fs::path(outDir) / file.path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
    }
    const fs::path configPath = fs::path(outDir) / (project + ".ini");
    if (fs::exists(configPath, ec)) {
        fs::copy_file(configPath, configPath.string() + ".bak", fs::copy_options::overwrite_existing, ec);
        std::printf("kept the previous config as %s.bak\n", configPath.c_str());
    }
    std::ofstream(configPath) << result.config;
    std::printf("\nwrote %s\n  %d chain(s), %d plug-in(s), %zu state file(s); %d left out, %d not found\n", configPath.c_str(), result.chains,
                result.plugins, result.files.size(), result.skipped, result.missing);
    for (const auto& n : result.notes)
        std::printf("  - %s\n", n.c_str());
    std::printf("\nnext: cue check \"%s\"\n", configPath.c_str());
    return result.missing ? 3 : 0;
}

int cmdExportRpp(const std::string& in, const std::string& out) {
    ConvertOptions options;
    options.rig = rigPath;
    ConvertResult result;
    std::string err;
    if (!convertFile(in, out, options, result, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s\n  %d chain(s), %d plug-in(s); %d left out\n", result.out.c_str(), result.chains, result.plugins, result.skipped);
    for (const auto& n : result.notes)
        std::printf("  - %s\n", n.c_str());
    return 0;
}

int cmdConvert(const std::string& in, std::string out, std::string rigOut) {
    namespace fs = std::filesystem;
    Config config;
    std::string err;
    if (!loadConfig(in, config, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return 2;
    }
    if (out.empty())
        out = (fs::path(in).parent_path() / (fs::path(in).stem().string() + ".cue")).string();
    if (config.performance.empty())
        config.performance = fs::path(in).stem().string();
    std::error_code ec;
    if (fs::exists(out, ec)) {
        fs::copy_file(out, out + ".bak", fs::copy_options::overwrite_existing, ec);
        std::printf("kept the previous file as %s.bak\n", out.c_str());
    }
    std::vector<std::string> warnings;
    if (!saveCue(config, out, &err, &warnings)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    size_t chains = 0, plugins = 0, states = 0;
    for (const auto& scene : config.scenes)
        for (const auto& c : scene.chains) {
            ++chains;
            for (const PluginSpec* p : chainPlugins(c)) {
                ++plugins;
                states += !p->stateFile.empty() + !p->controllerStateFile.empty();
            }
        }
    std::printf("wrote %s\n  %zu scene(s), %zu chain(s), %zu plug-in(s), %zu state(s)\n", out.c_str(), config.scenes.size(), chains, plugins, states);
    for (const auto& w : config.warnings)
        std::printf("  - %s\n", w.c_str());
    for (const auto& w : warnings)
        std::printf("  - %s\n", w.c_str());
    if (rigOut.empty()) {
        std::printf("\nThe config's audio and MIDI settings are not part of a performance. To keep them as this machine's rig\n"
                    "profile: cue convert \"%s\" \"%s\" --rig %s\n",
                    in.c_str(), out.c_str(), defaultRigPath().c_str());
    } else {
        rigOut = expandPath(rigOut);
        fs::create_directories(fs::path(rigOut).parent_path(), ec);
        if (!saveRig(config, rigOut, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote the rig profile %s\n", rigOut.c_str());
    }
    std::printf("\nnext: cue check \"%s\"\n", out.c_str());
    return 0;
}

// One line about a status event.
std::string describeStatus(const Json& s) {
    char buf[200], margin[32] = "-";
    if (!s.at("margin_ms").is_null())
        std::snprintf(margin, sizeof margin, "%.2f ms", s.at("margin_ms").get<double>());
    std::snprintf(buf, sizeof buf, "dsp %3.0f%% (max %3.0f%%) margin %s xruns %llu", s.at("dsp").at("avg").get<double>(),
                  s.at("dsp").at("max").get<double>(), margin, static_cast<unsigned long long>(s.value("xruns", 0ull)));
    std::string line = buf;
    line += " | " + s.at("selected").value("name", std::string("-"));
    if (!s.at("display").get<std::string>().empty())
        line += " [" + s.at("display").get<std::string>() + "]";
    if (s.at("muted").get<bool>())
        line += " MUTED";
    for (const auto& c : s.at("controls"))
        line += " | " + c.at("name").get<std::string>() + " " + c.at("text").get<std::string>();
    return line;
}

int cmdCtl(int argc, char** argv) {
    std::string socketPath = defaultSocketPath();
    bool asJson = false;
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socketPath = argv[++i];
        else if (std::strcmp(argv[i], "--json") == 0)
            asJson = true;
        else
            args.push_back(argv[i]);
    }
    if (args.empty()) {
        std::fprintf(stderr, "cue ctl [--socket PATH] [--json] status | watch | tuner | select <chain|number|next|previous> |\n"
                             "        do \"<action>\" | save [file.cue] | performance | plugin <chain> <n> | bypass <chain> <n> on|off |\n"
                             "        param <chain> <n> <id> <value> | editor <chain> <n> [close] | quit\n");
        return 2;
    }
    ServiceClient client;
    std::string err;
    if (!client.connect(socketPath, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const std::string what = args[0];
    auto ask = [&](const Json& request, Json& reply) {
        if (!client.request(request, reply, 10000, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        if (!reply.value("ok", false)) {
            std::fprintf(stderr, "%s\n", reply.value("error", std::string("refused")).c_str());
            return false;
        }
        return true;
    };
    Json reply;
    if (what == "status") {
        if (!ask({{"cmd", "status"}}, reply))
            return 1;
        if (asJson)
            std::printf("%s\n", reply.at("status").dump(2).c_str());
        else
            std::printf("%s\n", describeStatus(reply.at("status")).c_str());
        return 0;
    }
    if (what == "watch") {
        if (!ask({{"cmd", "subscribe"}, {"events", {"status", "log"}}}, reply))
            return 1;
        std::signal(SIGINT, onSignal);
        auto nextLine = Clock::now();
        while (!g_quit) {
            Json m;
            if (!client.next(m, 500, &err)) {
                if (err.find("in time") != std::string::npos)
                    continue;
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            const std::string event = m.value("event", "");
            if (asJson) {
                std::printf("%s\n", m.dump().c_str());
            } else if (event == "log") {
                std::printf("%s\n", m.value("text", "").c_str());
            } else if (event == "status" && Clock::now() >= nextLine) {
                nextLine = Clock::now() + std::chrono::milliseconds(500);
                std::printf("%s\n", describeStatus(m).c_str());
            }
            std::fflush(stdout);
        }
        return 0;
    }
    if (what == "tuner") {
        if (!ask({{"cmd", "subscribe"}, {"events", {"tuner"}}}, reply))
            return 1;
        std::signal(SIGINT, onSignal);
        while (!g_quit) {
            Json m;
            if (!client.next(m, 500, &err)) {
                if (err.find("in time") != std::string::npos)
                    continue;
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            if (m.value("event", "") != "tuner")
                continue;
            if (asJson) {
                std::printf("%s\n", m.dump().c_str());
            } else {
                char line[160];
                if (m.at("note").is_null())
                    std::snprintf(line, sizeof line, "%s  level %.0f dB", m.value("signal", false) ? "listening" : "no signal",
                                  m.value("level_db", -200.0));
                else
                    std::snprintf(line, sizeof line, "%-2s%d  %+6.1f cents  %8.2f Hz  %s", m.value("note", "").c_str(), m.value("octave", 0),
                                  m.value("cents", 0.0), m.value("hz", 0.0),
                                  m.value("held", true) ? "(held)" : m.value("in_tune", false) ? "IN TUNE" : "");
                std::printf("\r%-60s", line);
            }
            std::fflush(stdout);
        }
        std::printf("\n");
        return 0;
    }
    Json request;
    if (what == "select" && args.size() >= 2) {
        const std::string a = args[1];
        char* end = nullptr;
        const long n = std::strtol(a.c_str(), &end, 10);
        if (a == "next" || a == "previous")
            request = {{"cmd", "select"}, {"step", a == "next" ? 1 : -1}};
        else if (end && *end == '\0' && !a.empty())
            request = {{"cmd", "select"}, {"index", n - 1}};
        else
            request = {{"cmd", "select"}, {"chain", a}};
    } else if (what == "do" && args.size() >= 2) {
        std::string action;
        for (size_t i = 1; i < args.size(); ++i)
            action += (i > 1 ? " " : "") + args[i];
        request = {{"cmd", "do"}, {"action", action}};
    } else if (what == "save") {
        request = {{"cmd", "save"}};
        if (args.size() >= 2)
            request["path"] = std::filesystem::absolute(args[1]).string();
    } else if (what == "performance" || what == "quit" || what == "hello") {
        request = {{"cmd", what}};
    } else if ((what == "plugin" && args.size() >= 3) || (what == "bypass" && args.size() >= 4) || (what == "param" && args.size() >= 5) ||
               (what == "editor" && args.size() >= 3)) {
        // Chains and plug-ins by number from 1, as cue check lists them.
        request = {{"cmd", what}, {"chain", std::atoi(args[1].c_str()) - 1}, {"slot", std::atoi(args[2].c_str()) - 1}};
        if (what == "bypass")
            request["bypassed"] = args[3] == "on" || args[3] == "yes";
        if (what == "param") {
            request["param"] = std::strtoul(args[3].c_str(), nullptr, 10);
            request["value"] = std::atof(args[4].c_str());
        }
        if (what == "editor")
            request["open"] = args.size() < 4 || !(args[3] == "close" || args[3] == "off" || args[3] == "no");
    } else {
        std::fprintf(stderr,
                     "unknown or incomplete: cue ctl %s (status, watch, tuner, select, do, save, performance, plugin, bypass, param, editor, quit)\n",
                     what.c_str());
        return 2;
    }
    if (!ask(request, reply))
        return 1;
    if (asJson || what == "hello")
        std::printf("%s\n", reply.dump(2).c_str());
    else if (what == "performance")
        std::printf("%s\n", reply.at("performance").dump(2).c_str());
    else if (what == "select")
        std::printf("playing %s\n", reply.at("selected").value("name", "").c_str());
    else if (what == "param")
        std::printf("%s\n", reply.value("text", "").c_str());
    else if (what == "editor") {
        const Json& p = reply.at("plugin");
        std::printf("%s: editor %s\n", p.value("label", "").c_str(), p.value("editor_open", false) ? "open" : "closed");
    } else if (what == "plugin" || what == "bypass") {
        const Json& p = reply.at("plugin");
        std::printf("%s (%s, %s)%s\n", p.value("label", "").c_str(), p.value("name", "").c_str(), p.value("format", "").c_str(),
                    p.value("bypassed", false) ? "  bypassed" : "");
        for (const auto& q : p.at("params"))
            std::printf("  %6u  %-28s %s\n", q.value("id", 0u), q.value("name", "").c_str(),
                        q.value("text", "").empty() && q.at("value").is_number() ? std::to_string(q.at("value").get<double>()).c_str()
                                                                                    : q.value("text", "").c_str());
    }
    else if (what == "save") {
        std::printf("saved %s\n", reply.value("path", "").c_str());
        for (const auto& w : reply.value("warnings", Json::array()))
            std::printf("  - %s\n", w.get<std::string>().c_str());
    }
    return 0;
}

// This program, to run its scan helper.
std::string selfExe() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "cue";
}

std::unique_ptr<PluginScanner> makeScanner() {
    return std::make_unique<PluginScanner>(std::vector<std::string>{selfExe(), "scan-file"}, PluginScanner::defaultCache(),
                                           PluginScanner::defaultSettings());
}

// What the scan found, where it looked and how far it is, for the app.
Json pluginsJson(const PluginScanner& scanner) {
    const auto pr = scanner.progress();
    const PluginFolders folders = scanner.folders();
    Json j = {{"scanning", pr.scanning}, {"done", pr.done}, {"total", pr.total}, {"current", pr.current}, {"scanned_at", pr.scannedAt}};
    j["folders"] = {{"clap", folders.clap}, {"vst3", folders.vst3}, {"extra", scanner.extraFolders()}};
    Json files = Json::array();
    for (const ScannedFile& f : scanner.files())
        files.push_back(toJson(f));
    j["files"] = files;
    return j;
}

Json scanProgressJson(const PluginScanner& scanner) {
    const auto pr = scanner.progress();
    return {{"scanning", pr.scanning}, {"done", pr.done}, {"total", pr.total}, {"current", pr.current}, {"scanned_at", pr.scannedAt}};
}

// Every plug-in in the usual folders and the user's, each file in a helper.
int cmdScanAll(bool all) {
    auto scanner = makeScanner();
    std::string err;
    if (!scanner->start(all ? "all" : "changed", &err)) {
        std::fprintf(stderr, "scan: %s\n", err.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::string shown;
    for (;;) {
        const auto pr = scanner->progress();
        if (!pr.scanning)
            break;
        if (g_quit) {
            scanner->stop();
            std::printf("\nstopped\n");
            return 1;
        }
        if (pr.current != shown && !pr.current.empty()) {
            shown = pr.current;
            std::printf("[%zu/%zu] %s\n", pr.done + 1, pr.total, shown.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    size_t plugins = 0, skipped = 0;
    for (const ScannedFile& f : scanner->files()) {
        if (!f.error.empty()) {
            ++skipped;
            std::printf("skipped  %s: %s\n", f.path.c_str(), f.error.c_str());
            continue;
        }
        for (const FoundPlugin& p : f.plugins) {
            if (!p.error.empty()) {
                ++skipped;
                std::printf("skipped  %s (%s): %s\n", p.name.c_str(), f.path.c_str(), p.error.c_str());
                continue;
            }
            ++plugins;
            std::printf("%-5s %-32s %-20s %s%s\n", p.format == "clap" ? "CLAP" : "VST3", p.name.c_str(), p.vendor.c_str(),
                        p.bridged ? "(yabridge) " : "", (p.path + (p.format == "clap" ? "#" + p.id : "")).c_str());
        }
    }
    std::printf("%zu plug-in(s), %zu skipped; kept in %s\n", plugins, skipped, PluginScanner::defaultCache().c_str());
    return 0;
}

int cmdScan(const std::string& path) {
    std::string err;
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".vst3") == 0) {
        auto list = scanVst3(path, &err);
        if (list.empty()) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.empty() ? "no audio effects" : err.c_str());
            return 1;
        }
        for (const auto& p : list)
            std::printf("%s#%s\n    by %s, version %s, %s\n", p.path.c_str(), p.name.c_str(), p.vendor.c_str(), p.version.c_str(), p.category.c_str());
        return 0;
    }
    auto list = scanClapFile(path, &err);
    if (list.empty()) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.empty() ? "no plug-ins" : err.c_str());
        return 1;
    }
    for (const auto& p : list)
        std::printf("%s#%s\n    %s by %s, version %s\n", p.path.c_str(), p.id.c_str(), p.name.c_str(), p.vendor.c_str(), p.version.c_str());
    return 0;
}

int cmdMidiMonitor(const std::string& match) {
    AlsaMidi midi;
    std::string err;
    if (!midi.open("Cue monitor", &err)) {
        std::fprintf(stderr, "midi: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening to %d port(s) matching '%s'. Ctrl+C to stop.\n", midi.connectInputFrom(match), match.c_str());
    std::signal(SIGINT, onSignal);
    MidiMessage m;
    while (!g_quit) {
        if (!midi.read(m, 100))
            continue;
        std::printf("ch %2d  %02X %3d %3d", m.channel() + 1, m.type(), m.data1, m.data2);
        const auto in = eurekaprom::decode(m);
        switch (in.kind) {
        case eurekaprom::Input::Kind::Press: std::printf("   EurekaPROM: pedal %d pressed", in.pedal); break;
        case eurekaprom::Input::Kind::Release: std::printf("   EurekaPROM: pedal %d released", in.pedal); break;
        case eurekaprom::Input::Kind::Expression: std::printf("   EurekaPROM: %s expression %d", in.expression == 1 ? "left" : "right", in.value); break;
        default: break;
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}

int cmdFcbTest(const std::string& match, uint8_t channel) {
    AlsaMidi midi;
    std::string err;
    if (!midi.open("Cue FCB test", &err)) {
        std::fprintf(stderr, "midi: %s\n", err.c_str());
        return 1;
    }
    const int n = midi.connectOutputTo(match);
    std::printf("sending to %d port(s) matching '%s' on channel %d\n", n, match.c_str(), channel + 1);
    if (n == 0)
        return 1;
    for (int pedal = 1; pedal <= 10; ++pedal) {
        midi.send(eurekaprom::led(channel, pedal, true));
        midi.send(eurekaprom::displayNumber(channel, pedal));
        std::printf("LED %d on, display %d\n", pedal, pedal);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        midi.send(eurekaprom::led(channel, pedal, false));
    }
    midi.send(eurekaprom::displayLetter(channel, true, 'O'));
    midi.send(eurekaprom::displayLetter(channel, false, 'K'));
    std::printf("display shows OK\n");
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  cue run <config.ini|file.cue> [--rig rig.json] [--audio none] [--socket PATH|none] [--leave-device]\n"
                 "  cue ctl [--socket PATH] [--json] status|watch|tuner|select <chain>|do \"<action>\"|save [file.cue]|performance|quit\n"
                 "  cue convert <config.ini> [out.cue] [--rig rig.json]\n"
                 "  cue bench <config.ini|file.cue> [--switch-every S] [--seconds S] [--wav out.wav] [--warm none|next] [--workers N] [--fast yes]\n"
                 "  cue doctor [--device NAME] [--priority N]\n"
                 "  cue devices\n"
                 "  cue check <config.ini|file.cue> [--no-load] [--rig rig.json]\n"
                 "  cue scan [--all]           (every plug-in in the usual folders; --all opens unchanged files again)\n"
                 "  cue scan <file.clap|bundle.vst3>\n"
                 "  cue params <file.clap|bundle.vst3>[#id]\n"
                 "  cue import-rpp <project.rpp> [--out DIR] [--mapping FILE] [--search DIR]... [--no-scan]\n"
                 "  cue export-rpp <file.cue|config.ini> [out.rpp] [--rig rig.json]\n"
                 "  cue midi-monitor [port-match]\n"
                 "  cue fcb-test <port-match> [--channel N]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    // Line by line even into a file (the app's --start writes engine.log), so
    // what it says is there when it is said.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2)
        return usage();
    const std::string cmd = argv[1];
    auto opt = [&](const char* name, const char* def) -> std::string {
        for (int i = 2; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], name) == 0)
                return argv[i + 1];
        return def;
    };
    rigPath = opt("--rig", "");
    if (cmd == "convert" && argc >= 3)
        return cmdConvert(argv[2], argc >= 4 && argv[3][0] != '-' ? argv[3] : "", opt("--rig", ""));
    if (cmd == "run" && argc >= 3) {
        RunOptions o;
        o.noAudio = opt("--audio", "") == "none";
        o.socket = opt("--socket", "");
        o.noSocket = o.socket == "none";
        for (int i = 3; i < argc; ++i)
            if (std::strcmp(argv[i], "--leave-device") == 0)
                o.takeDevice = false;
        return cmdRun(argv[2], o);
    }
    if (cmd == "ctl")
        return cmdCtl(argc, argv);
    if (cmd == "bench" && argc >= 3)
        return cmdBench(argv[2], std::stod(opt("--switch-every", "2")), std::stod(opt("--seconds", "30")), opt("--wav", ""),
                        opt("--fast", "no") == "no", opt("--warm", ""),
                        std::stoi(opt("--workers", "-1")));
    if (cmd == "doctor") {
        DoctorOptions o;
        o.deviceMatch = opt("--device", "M2");
        o.rtPriority = std::stoi(opt("--priority", "80"));
        return runDoctor(o);
    }
    if (cmd == "devices")
        return cmdDevices();
    if (cmd == "import-rpp" && argc >= 3) {
        std::vector<std::string> search;
        for (int i = 3; i + 1 < argc; ++i)
            if (std::strcmp(argv[i], "--search") == 0)
                search.push_back(argv[i + 1]);
        bool scan = true;
        for (int i = 3; i < argc; ++i)
            scan = scan && std::strcmp(argv[i], "--no-scan") != 0;
        return cmdImportRpp(argv[2], opt("--out", ""), search, opt("--mapping", ""), scan);
    }
    if (cmd == "export-rpp" && argc >= 3 && !isRppFile(argv[2]))
        return cmdExportRpp(argv[2], argc >= 4 && argv[3][0] != '-' ? argv[3] : "");
    if (cmd == "check" && argc >= 3) {
        bool loadPlugins = true;
        for (int i = 3; i < argc; ++i)
            loadPlugins = loadPlugins && std::strcmp(argv[i], "--no-load") != 0;
        return cmdCheck(argv[2], loadPlugins);
    }
    if (cmd == "params" && argc >= 3)
        return cmdParams(argv[2]);
    if (cmd == "scan-file" && argc >= 5) {
        // A scan started while playing (from a terminal too) stays off the
        // audio core, as cue run keeps its own helpers and Wine hosts. Not a
        // lower priority: a Wine server it starts would serve the show too.
        keepOffAudioCore(pickAudioCpu());
        return scanFileMain(argv[2], argv[3], std::atoi(argv[4]), argc >= 6 ? std::strtoul(argv[5], nullptr, 10) : 0);
    }
    if (cmd == "scan" && argc >= 3 && argv[2][0] != '-')
        return cmdScan(argv[2]);
    if (cmd == "scan")
        return cmdScanAll(argc >= 3 && std::strcmp(argv[2], "--all") == 0);
    if (cmd == "midi-monitor")
        return cmdMidiMonitor(argc >= 3 && argv[2][0] != '-' ? argv[2] : "any");
    if (cmd == "fcb-test" && argc >= 3)
        return cmdFcbTest(argv[2], static_cast<uint8_t>(std::stoi(opt("--channel", "1")) - 1));
    return usage();
}
