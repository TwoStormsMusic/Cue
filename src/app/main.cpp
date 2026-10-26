// cue: the live plug-in host prototype.
//
//   cue run <config|file.cue>    play live: ALSA audio, FCB1010 switching
//                                (a .cue takes this machine's settings from its rig profile, or --rig FILE)
//   cue convert <config> [out]   save a config as a .cue performance (--rig FILE: and its audio and MIDI settings)
//   cue bench <config> [opts]    offline: measure CPU per chain and render a WAV
//   cue doctor [--device M2]     check real-time settings
//   cue devices                  list sound cards and MIDI ports
//   cue check <config>           load everything, check the mappings, print the performance
//                                (--no-load: check without loading plug-ins)
//   cue scan <file.clap|.vst3>   list the plug-ins in a CLAP file or VST3 bundle
//   cue params <file>[#id]       list a plug-in's parameters and their ids
//   cue import-rpp <file.rpp>    make a config from a REAPER project
//   cue midi-monitor             print incoming MIDI, decoded for EurekaPROM
//   cue fcb-test [--channel N]   walk the FCB1010 LEDs and display

#include "app/build.h"
#include "app/config.h"
#include "app/cpu.h"
#include "app/cuefile.h"
#include "app/doctor.h"
#include "app/rpp.h"
#include "control/show.h"
#include "audio/alsa_backend.h"
#include "core/engine.h"
#include "host/clap_processor.h"
#include "host/vst3_processor.h"
#include "midi/alsa_midi.h"
#include "midi/eurekaprom.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace cue;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

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

struct SceneSample {
    uint64_t ns = 0, blocks = 0;
};

// The chains that are running, with their CPU use since the last call.
std::string chainStatus(Engine& engine, const Rig& rig, std::vector<SceneSample>& last) {
    std::string s;
    char buf[160];
    for (size_t i = 0; i < engine.numChains(); ++i) {
        ChainStats& st = engine.stats(i);
        const uint64_t ns = st.processNs.load(), blocks = st.processBlocks.load();
        const uint64_t dNs = ns - last[i].ns, dBlocks = blocks - last[i].blocks;
        last[i] = {ns, blocks};
        const double avgUs = dBlocks ? dNs / 1000.0 / dBlocks : 0.0;
        const double maxUs = st.maxBlockNs.exchange(0) / 1000.0;
        const auto state = static_cast<ChainState>(st.state.load());
        if (state == ChainState::Idle)
            continue;
        std::snprintf(buf, sizeof buf, "%s%s %s %.0f/%.0fus", s.empty() ? "" : "  ", rig.chainNames[i].c_str(), toString(state), avgUs, maxUs);
        s += buf;
    }
    return s;
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

int cmdRun(const std::string& path) {
    Config config;
    if (!load(path, config))
        return 2;
    Rig rig;
    std::string err;
    if (!buildRig(config, rig, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    Engine& engine = *rig.engine;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "warning: could not lock memory (see cue doctor)\n");

    AlsaBackend audio;
    if (!audio.open(config.audio, &err)) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    std::printf("audio: %s\n", audio.describe().c_str());

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
    if (config.midi.enabled) {
        if (midi.open("Cue", &err)) {
            const int ins = midi.connectInputFrom(config.midi.in);
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

    std::atomic<bool> marked{false};
    audio.start([&](const float* in, float* const* outs, uint32_t frames) {
        if (!marked.load(std::memory_order_relaxed)) {
            markClapAudioThread();
            marked.store(true, std::memory_order_relaxed);
        }
        engine.process(in, outs, frames);
    }, &err);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    show.start();
    std::printf("keys: 1-9 select preset, n/p next/previous, q quit\n");

    RawTerminal term;
    std::vector<SceneSample> last(engine.numChains());
    uint64_t lastCycles = 0, lastNs = 0;
    auto nextStatus = Clock::now();
    const double periodUs = 1e6 * config.audio.period / config.audio.rate;
    bool reportedSched = false;
    uint64_t reportedXruns = 0, reportedStartup = 0;

    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rig.pumpMainThread();
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

        auto& st = const_cast<AlsaStats&>(audio.stats());
        if (st.failed) {
            std::fprintf(stderr, "\naudio device failed and could not be recovered\n");
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
            std::printf("%sstart-up: the sound card stalled as it started (%s at block %llu), before anything was heard; not counted as an xrun\n",
                        term.active ? "\r\033[K" : "", st.lastXrunPlayback ? "playback ran dry" : "capture overflowed",
                        static_cast<unsigned long long>(st.lastXrunCycle.load()));
        }
        // Each xrun gets its own line, with the slowest block around it.
        BlockReport report;
        while (engine.nextBlockReport(report)) {
            const uint64_t xruns = st.xruns.load();
            if (xruns > reportedXruns) {
                std::printf("%s%s\n", term.active ? "\r\033[K" : "",
                            describeXrun(xruns - reportedXruns, st, report, rig, periodUs, config.audio.rate).c_str());
                reportedXruns = xruns;
            }
        }
        if (Clock::now() >= nextStatus) {
            nextStatus += std::chrono::milliseconds(500);
            const uint64_t cycles = st.cycles, ns = st.totalCycleNs;
            const double avgUs = cycles > lastCycles ? (ns - lastNs) / 1000.0 / (cycles - lastCycles) : 0.0;
            lastCycles = cycles;
            lastNs = ns;
            const double maxUs = st.maxCycleNs.exchange(0) / 1000.0;
            engine.requestBlockReport();
            // The numbers first, and no wider than the terminal: a line that
            // wraps cannot be redrawn in place.
            // Margin: the least audio left to play when a block was written.
            const int32_t queued = st.minQueuedFrames.exchange(INT32_MAX);
            char margin[32] = "-";
            if (queued != INT32_MAX)
                std::snprintf(margin, sizeof margin, "%.2f ms", 1000.0 * std::max(0, queued) / config.audio.rate);
            char head[128];
            std::snprintf(head, sizeof head, "dsp %3.0f%% (max %3.0f%%) margin %s xruns %llu | ", 100.0 * avgUs / periodUs, 100.0 * maxUs / periodUs,
                          margin, static_cast<unsigned long long>(st.xruns.load()));
            std::string line = head + show.status() + " | " + chainStatus(engine, rig, last);
            const size_t width = term.active ? terminalWidth() : 0;
            if (width > 1 && line.size() > width - 1)
                line.resize(width - 1);
            std::printf("%s%s\033[K%s", term.active ? "\r" : "", line.c_str(), term.active ? "" : "\n");
            // A plug-in that took most of a period in one block gets its own
            // line, so a click can be traced to what caused it.
            for (size_t i = 0; i < engine.numChains(); ++i) {
                size_t slot = 0;
                uint32_t slotNs = 0;
                if (engine.chain(i).takeSlowest(slot, slotNs) && slotNs / 1000.0 > 0.8 * periodUs)
                    std::printf("%sslow block: %s / %s took %.1f ms (the period is %.2f ms)\n", term.active ? "\n" : "",
                                rig.chainNames[i].c_str(), engine.chain(i).slot(slot).name().c_str(), slotNs / 1e6, periodUs / 1000.0);
            }
            std::fflush(stdout);
        }
    }
    std::printf("\nstopping\n");
    audio.stop();
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

void writeWav(const std::string& path, const std::vector<float>& a, const std::vector<float>& b, unsigned rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t frames = static_cast<uint32_t>(a.size());
    f.write("RIFF", 4);
    u32(36 + frames * 8);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(rate);
    u32(rate * 8);
    u16(8);
    u16(32);
    f.write("data", 4);
    u32(frames * 8);
    for (uint32_t i = 0; i < frames; ++i) {
        f.write(reinterpret_cast<const char*>(&a[i]), 4);
        f.write(reinterpret_cast<const char*>(&b[i]), 4);
    }
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
    std::vector<float> wav1, wav2;
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
        std::vector<float> in(frames), o1(frames), o2(frames);
        float* outs[2] = {o1.data(), o2.data()};
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
            engine.process(in.data(), outs, frames);
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            times.push_back(us);
            worstUs = std::max(worstUs, us);
            if (!wavPath.empty()) {
                wav1.insert(wav1.end(), o1.begin(), o1.end());
                wav2.insert(wav2.end(), o2.begin(), o2.end());
            }
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
        writeWav(wavPath, wav1, wav2, config.audio.rate);
        std::printf("wrote %s (left = out 1, right = out 2)\n", wavPath.c_str());
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
        for (const auto& c : scene.chains) {
            auto where = [&](const TapSpec& t) {
                const LineSpec& l = c.lines[t.line];
                const std::string line = t.line == 0 ? "main line" : "branch " + l.name;
                return t.after >= l.plugins.size() ? "end of " + line : line + " after " + std::to_string(t.after) + " plug-in(s)";
            };
            std::printf("  chain %s: %zu plug-in(s); out 1 = %s, out 2 = %s\n", c.name.c_str(), chainPlugins(c).size(), where(c.out[0]).c_str(),
                        where(c.out[1]).c_str());
        }
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
                 "  cue run <config.ini|file.cue> [--rig rig.json]\n"
                 "  cue convert <config.ini> [out.cue] [--rig rig.json]\n"
                 "  cue bench <config.ini|file.cue> [--switch-every S] [--seconds S] [--wav out.wav] [--warm none|next] [--workers N] [--fast yes]\n"
                 "  cue doctor [--device NAME] [--priority N]\n"
                 "  cue devices\n"
                 "  cue check <config.ini|file.cue> [--no-load] [--rig rig.json]\n"
                 "  cue scan <file.clap|bundle.vst3>\n"
                 "  cue params <file.clap|bundle.vst3>[#id]\n"
                 "  cue import-rpp <project.rpp> [--out DIR] [--mapping FILE] [--search DIR]... [--no-scan]\n"
                 "  cue midi-monitor [port-match]\n"
                 "  cue fcb-test <port-match> [--channel N]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
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
    if (cmd == "run" && argc >= 3)
        return cmdRun(argv[2]);
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
    if (cmd == "check" && argc >= 3) {
        bool loadPlugins = true;
        for (int i = 3; i < argc; ++i)
            loadPlugins = loadPlugins && std::strcmp(argv[i], "--no-load") != 0;
        return cmdCheck(argv[2], loadPlugins);
    }
    if (cmd == "params" && argc >= 3)
        return cmdParams(argv[2]);
    if (cmd == "scan" && argc >= 3)
        return cmdScan(argv[2]);
    if (cmd == "midi-monitor")
        return cmdMidiMonitor(argc >= 3 && argv[2][0] != '-' ? argv[2] : "any");
    if (cmd == "fcb-test" && argc >= 3)
        return cmdFcbTest(argv[2], static_cast<uint8_t>(std::stoi(opt("--channel", "1")) - 1));
    return usage();
}
