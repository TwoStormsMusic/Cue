// stagehost: the live plug-in host prototype.
//
//   stagehost run <config>             play live: ALSA audio, FCB1010 switching
//   stagehost bench <config> [opts]    offline: measure CPU per scene and render a WAV
//   stagehost doctor [--device M2]     check real-time settings
//   stagehost devices                  list sound cards and MIDI ports
//   stagehost scan <file.clap>         list the plug-ins in a CLAP file
//   stagehost midi-monitor             print incoming MIDI, decoded for EurekaPROM
//   stagehost fcb-test [--channel N]   walk the FCB1010 LEDs and display

#include "app/build.h"
#include "app/config.h"
#include "app/cpu.h"
#include "app/doctor.h"
#include "audio/alsa_backend.h"
#include "core/engine.h"
#include "host/clap_processor.h"
#include "midi/alsa_midi.h"
#include "midi/eurekaprom.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
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
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sh;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

// ------------------------------------------------------------ controller

// Turns MIDI into scene switches and engine events into LEDs. MIDI is read
// and written on one thread; other threads queue outgoing messages.
class Controller {
public:
    Controller(Engine& engine, const Config& config) : engine_(engine), config_(config) {}

    void attach(AlsaMidi* midi) { midi_ = midi; }

    void onMidi(const MidiMessage& m) {
        if (config_.midi.profile == "eurekaprom") {
            const auto in = eurekaprom::decode(m);
            if (in.kind == eurekaprom::Input::Kind::Press) {
                if (in.pedal == eurekaprom::kUp)
                    step(+1);
                else if (in.pedal == eurekaprom::kDown)
                    step(-1);
                else
                    for (size_t i = 0; i < config_.scenes.size(); ++i)
                        if (config_.scenes[i].pedal == in.pedal)
                            engine_.switchTo(static_cast<int>(i));
                return;
            }
            if (in.kind != eurekaprom::Input::Kind::None)
                return;
        }
        if (m.type() == 0xC0)
            for (size_t i = 0; i < config_.scenes.size(); ++i)
                if (config_.scenes[i].program == m.data1)
                    engine_.switchTo(static_cast<int>(i));
    }

    void step(int delta) {
        const int n = static_cast<int>(engine_.numScenes());
        const int cur = active_.load();
        engine_.switchTo(((cur < 0 ? 0 : cur) + delta + n) % n);
    }

    void onEvent(const EngineEvent& ev) {
        if (ev.type != EngineEvent::Type::SceneActive)
            return;
        const int previous = active_.exchange(ev.scene);
        if (config_.warm == "next" && engine_.numScenes() > 1)
            engine_.setWarm((ev.scene + 1) % static_cast<int>(engine_.numScenes()));
        if (config_.midi.profile == "eurekaprom") {
            const uint8_t ch = config_.midi.channel;
            if (previous >= 0 && config_.scenes[previous].pedal >= 1 && config_.scenes[previous].pedal <= 10)
                queue(eurekaprom::led(ch, config_.scenes[previous].pedal, false));
            const int pedal = config_.scenes[ev.scene].pedal;
            if (pedal >= 1 && pedal <= 10)
                queue(eurekaprom::led(ch, pedal, true));
            queue(eurekaprom::displayNumber(ch, ev.scene + 1));
        }
    }

    int active() const { return active_.load(); }

    void queue(const MidiMessage& m) {
        std::lock_guard<std::mutex> lock(outMutex_);
        out_.push_back(m);
    }

    // MIDI thread.
    void flushOut() {
        std::vector<MidiMessage> pending;
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            pending.swap(out_);
        }
        if (midi_)
            for (const auto& m : pending)
                midi_->send(m);
    }

private:
    Engine& engine_;
    const Config& config_;
    AlsaMidi* midi_ = nullptr;
    std::atomic<int> active_{-1};
    std::mutex outMutex_;
    std::vector<MidiMessage> out_;
};

// ------------------------------------------------------------ status

struct SceneSample {
    uint64_t ns = 0, blocks = 0;
};

std::string sceneStatus(Engine& engine, std::vector<SceneSample>& last, double periodUs) {
    std::string s;
    char buf[160];
    for (size_t i = 0; i < engine.numScenes(); ++i) {
        SceneStats& st = engine.stats(i);
        const uint64_t ns = st.processNs.load(), blocks = st.processBlocks.load();
        const uint64_t dNs = ns - last[i].ns, dBlocks = blocks - last[i].blocks;
        last[i] = {ns, blocks};
        const double avgUs = dBlocks ? dNs / 1000.0 / dBlocks : 0.0;
        const double maxUs = st.maxBlockNs.exchange(0) / 1000.0;
        const auto state = static_cast<SceneState>(st.state.load());
        std::snprintf(buf, sizeof buf, "%s%zu:%s %.0f/%.0fus", s.empty() ? "" : "  ", i + 1, toString(state), avgUs, maxUs);
        s += buf;
    }
    (void)periodUs;
    return s;
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

bool load(const std::string& path, Config& config) {
    std::string err;
    if (!loadConfig(path, config, &err)) {
        std::fprintf(stderr, "config: %s\n", err.c_str());
        return false;
    }
    if (config.cpuAuto)
        config.audio.cpu = pickAudioCpu();
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
        std::fprintf(stderr, "warning: could not lock memory (see stagehost doctor)\n");

    AlsaBackend audio;
    if (!audio.open(config.audio, &err)) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    std::printf("audio: %s\n", audio.describe().c_str());

    Controller controller(engine, config);
    AlsaMidi midi;
    std::thread midiThread;
    std::atomic<bool> midiRun{true};
    if (config.midi.enabled) {
        if (midi.open("Stagehost", &err)) {
            const int ins = midi.connectInputFrom(config.midi.in);
            const int outs = config.midi.out.empty() ? 0 : midi.connectOutputTo(config.midi.out);
            std::printf("midi: input from %d port(s) matching '%s', output to %d port(s) matching '%s', profile %s\n", ins,
                        config.midi.in.c_str(), outs, config.midi.out.c_str(), config.midi.profile.c_str());
            controller.attach(&midi);
            midiThread = std::thread([&] {
                MidiMessage m;
                while (midiRun) {
                    if (midi.read(m, 5))
                        controller.onMidi(m);
                    controller.flushOut();
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
    engine.switchTo(0);
    std::printf("keys: 1-9 select scene, n/p next/previous, q quit\n");

    RawTerminal term;
    std::vector<SceneSample> last(engine.numScenes());
    uint64_t lastCycles = 0, lastNs = 0;
    auto nextStatus = Clock::now();
    const double periodUs = 1e6 * config.audio.period / config.audio.rate;
    bool reportedSched = false;

    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (ClapProcessor* p : rig.clapPlugins)
            p->pumpMainThread();
        EngineEvent ev;
        while (engine.nextEvent(ev))
            controller.onEvent(ev);
        const int k = term.key();
        if (k == 'q')
            break;
        if (k >= '1' && k <= '9')
            engine.switchTo(k - '1');
        if (k == 'n')
            controller.step(+1);
        if (k == 'p')
            controller.step(-1);

        auto& st = const_cast<AlsaStats&>(audio.stats());
        if (st.failed) {
            std::fprintf(stderr, "\naudio device failed and could not be recovered\n");
            break;
        }
        if (!reportedSched && st.cycles > 0) {
            reportedSched = true;
            std::printf("audio thread: %s, %s\n", st.realtime ? "real-time" : "NOT real-time (see stagehost doctor)",
                        st.pinned ? ("pinned to CPU " + std::to_string(config.audio.cpu)).c_str() : "not pinned");
        }
        if (Clock::now() >= nextStatus) {
            nextStatus += std::chrono::milliseconds(500);
            const uint64_t cycles = st.cycles, ns = st.totalCycleNs;
            const double avgUs = cycles > lastCycles ? (ns - lastNs) / 1000.0 / (cycles - lastCycles) : 0.0;
            lastCycles = cycles;
            lastNs = ns;
            const double maxUs = st.maxCycleNs.exchange(0) / 1000.0;
            const int a = controller.active();
            std::printf("%s[%s] dsp %3.0f%% (max %3.0f%%) xruns %llu | %s   %s",
                        term.active ? "\r" : "", a >= 0 ? config.scenes[a].name.c_str() : "-", 100.0 * avgUs / periodUs,
                        100.0 * maxUs / periodUs, static_cast<unsigned long long>(st.xruns.load()),
                        sceneStatus(engine, last, periodUs).c_str(), term.active ? "" : "\n");
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

int cmdBench(const std::string& path, double switchSeconds, double totalSeconds, const std::string& wavPath) {
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
    const uint32_t frames = config.audio.period;
    const double periodUs = 1e6 * frames / config.audio.rate;

    // Measure under the same conditions as live: real-time priority on the
    // audio CPU, if allowed.
    double worstUs = 0;
    std::vector<double> blockUs;
    std::vector<float> wav1, wav2;
    bool realtime = false, pinned = false;
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
        const size_t blocksPerSwitch = static_cast<size_t>(switchSeconds * config.audio.rate / frames);
        const size_t total = static_cast<size_t>(totalSeconds * config.audio.rate / frames);
        engine.switchTo(0);
        int next = 1;
        for (size_t b = 0; b < total && !g_quit; ++b) {
            if (b > 0 && b % blocksPerSwitch == 0 && engine.numScenes() > 1) {
                engine.switchTo(next);
                next = (next + 1) % static_cast<int>(engine.numScenes());
            }
            for (auto& v : in)
                v = input();
            const auto t0 = Clock::now();
            engine.process(in.data(), outs, frames);
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            blockUs.push_back(us);
            worstUs = std::max(worstUs, us);
            if (!wavPath.empty()) {
                wav1.insert(wav1.end(), o1.begin(), o1.end());
                wav2.insert(wav2.end(), o2.begin(), o2.end());
            }
            // Events are drained on this thread here; live, the main thread does it.
            EngineEvent ev;
            while (engine.nextEvent(ev))
                if (ev.type == EngineEvent::Type::SceneActive && config.warm == "next" && engine.numScenes() > 1)
                    engine.setWarm((ev.scene + 1) % static_cast<int>(engine.numScenes()));
        }
    });
    worker.join();

    std::vector<double> sorted = blockUs;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]; };
    std::printf("\nbench: %zu blocks of %u frames (budget %.0f us), switching every %.1f s, %s, %s\n", blockUs.size(), frames, periodUs,
                switchSeconds, realtime ? "real-time" : "not real-time", pinned ? ("CPU " + std::to_string(config.audio.cpu)).c_str() : "unpinned");
    std::printf("block time: median %.0f us (%.0f%%), 99%% %.0f us (%.0f%%), 99.9%% %.0f us (%.0f%%), worst %.0f us (%.0f%%)\n", pct(0.5),
                100 * pct(0.5) / periodUs, pct(0.99), 100 * pct(0.99) / periodUs, pct(0.999), 100 * pct(0.999) / periodUs, worstUs,
                100 * worstUs / periodUs);
    std::printf("\n%-24s %10s %12s %12s %10s\n", "scene", "blocks", "avg us/blk", "worst us", "share");
    uint64_t totalBlocks = blockUs.size();
    for (size_t i = 0; i < engine.numScenes(); ++i) {
        SceneStats& st = engine.stats(i);
        const uint64_t blocks = st.processBlocks.load();
        std::printf("%-24s %10llu %12.1f %12.1f %9.0f%%\n", config.scenes[i].name.c_str(), static_cast<unsigned long long>(blocks),
                    blocks ? st.processNs.load() / 1000.0 / blocks : 0.0, st.maxBlockNs.load() / 1000.0, 100.0 * blocks / totalBlocks);
    }
    std::printf("\n'share' is the fraction of blocks in which the scene was processed at all. Idle scenes cost nothing.\n");
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
    if (!midi.open("Stagehost devices", &err)) {
        std::printf("MIDI: %s\n", err.c_str());
        return 0;
    }
    std::printf("MIDI ports:\n");
    for (const auto& p : midi.listPorts())
        std::printf("  %3d:%-2d  %-28s %-28s %s%s\n", p.client, p.port, p.clientName.c_str(), p.portName.c_str(), p.canRead ? "in " : "",
                    p.canWrite ? "out" : "");
    return 0;
}

int cmdScan(const std::string& path) {
    std::string err;
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
    if (!midi.open("Stagehost monitor", &err)) {
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
    if (!midi.open("Stagehost FCB test", &err)) {
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
                 "  stagehost run <config.ini>\n"
                 "  stagehost bench <config.ini> [--switch-every S] [--seconds S] [--wav out.wav]\n"
                 "  stagehost doctor [--device NAME] [--priority N]\n"
                 "  stagehost devices\n"
                 "  stagehost scan <file.clap>\n"
                 "  stagehost midi-monitor [port-match]\n"
                 "  stagehost fcb-test <port-match> [--channel N]\n");
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
    if (cmd == "run" && argc >= 3)
        return cmdRun(argv[2]);
    if (cmd == "bench" && argc >= 3)
        return cmdBench(argv[2], std::stod(opt("--switch-every", "2")), std::stod(opt("--seconds", "30")), opt("--wav", ""));
    if (cmd == "doctor") {
        DoctorOptions o;
        o.deviceMatch = opt("--device", "M2");
        o.rtPriority = std::stoi(opt("--priority", "80"));
        return runDoctor(o);
    }
    if (cmd == "devices")
        return cmdDevices();
    if (cmd == "scan" && argc >= 3)
        return cmdScan(argv[2]);
    if (cmd == "midi-monitor")
        return cmdMidiMonitor(argc >= 3 && argv[2][0] != '-' ? argv[2] : "any");
    if (cmd == "fcb-test" && argc >= 3)
        return cmdFcbTest(argv[2], static_cast<uint8_t>(std::stoi(opt("--channel", "1")) - 1));
    return usage();
}
