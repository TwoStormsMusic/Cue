// cue doctor: checks the real-time settings a live rig depends on.
// Read-only: it reports and suggests, it never changes the system.

#include "cpu.h"
#include "doctor.h"

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/utsname.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace cue {

namespace {

enum class Level { Ok, Info, Warn, Fail };

struct Result {
    Level level;
    std::string check, detail, fix;
};

const char* label(Level l) {
    switch (l) {
    case Level::Ok: return "\033[32m  OK \033[0m";
    case Level::Info: return "\033[35m INFO\033[0m";
    case Level::Warn: return "\033[33m WARN\033[0m";
    case Level::Fail: return "\033[31m FAIL\033[0m";
    }
    return "";
}

bool processRunning(const std::string& name) {
    DIR* d = opendir("/proc");
    if (!d)
        return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        if (readFileTrimmed(std::string("/proc/") + e->d_name + "/comm") == name) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string limitText(rlim_t v) { return v == RLIM_INFINITY ? "unlimited" : std::to_string(v); }

} // namespace

int runDoctor(const DoctorOptions& opt) {
    std::vector<Result> results;
    auto add = [&](Level l, std::string c, std::string d, std::string f = "") { results.push_back({l, c, d, f}); };

    // Real-time scheduling.
    rlimit rt{};
    getrlimit(RLIMIT_RTPRIO, &rt);
    bool gotFifo = false;
    std::thread([&] {
        sched_param sp{};
        sp.sched_priority = opt.rtPriority;
        gotFifo = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
    }).join();
    if (gotFifo)
        add(Level::Ok, "Real-time priority", "SCHED_FIFO " + std::to_string(opt.rtPriority) + " granted (limit " + limitText(rt.rlim_cur) +
            (rt.rlim_cur != RLIM_INFINITY && rt.rlim_cur < static_cast<rlim_t>(opt.rtPriority) ? ", allowed by root or CAP_SYS_NICE)" : ")"));
    else
        add(Level::Fail, "Real-time priority", "SCHED_FIFO " + std::to_string(opt.rtPriority) + " refused, limit is " + limitText(rt.rlim_cur),
            "add your user to the audio group with '@audio - rtprio 95' in /etc/security/limits.d/audio.conf, then log in again");

    rlimit ml{};
    getrlimit(RLIMIT_MEMLOCK, &ml);
    if (ml.rlim_cur == RLIM_INFINITY || ml.rlim_cur >= (rlim_t(1) << 32))
        add(Level::Ok, "Memory locking", "limit " + limitText(ml.rlim_cur));
    else
        add(Level::Warn, "Memory locking", "limit " + limitText(ml.rlim_cur) + " bytes: audio memory may be paged out",
            "'@audio - memlock unlimited' in /etc/security/limits.d/audio.conf");

    // Kernel.
    utsname u{};
    uname(&u);
    const std::string version = u.version;
    const std::string cmdline = readFileTrimmed("/proc/cmdline");
    const bool rtKernel = version.find("PREEMPT_RT") != std::string::npos;
    const bool dynamic = version.find("PREEMPT_DYNAMIC") != std::string::npos;
    const std::string preemptMode = readFileTrimmed("/sys/kernel/debug/sched/preempt");
    if (rtKernel)
        add(Level::Ok, "Kernel preemption", std::string(u.release) + " is PREEMPT_RT");
    else if (version.find("PREEMPT") != std::string::npos)
        add(Level::Ok, "Kernel preemption", std::string(u.release) + (dynamic ? " (PREEMPT_DYNAMIC" : " (PREEMPT") +
            (preemptMode.empty() ? "" : ", mode " + preemptMode) + ")" +
            (cmdline.find("preempt=") != std::string::npos ? ", boot option " + cmdline.substr(cmdline.find("preempt="), 16) : ""));
    else
        add(Level::Warn, "Kernel preemption", std::string(u.release) + " is not a preemptible kernel",
            "use a low-latency or real-time kernel (Liquorix, or a PREEMPT_RT build)");
    const bool threadirqs = rtKernel || cmdline.find("threadirqs") != std::string::npos;
    add(threadirqs ? Level::Ok : Level::Warn, "Threaded interrupts",
        threadirqs ? "on: the USB interrupt can be given real-time priority" : "off: USB audio interrupts cannot be prioritised",
        threadirqs ? "" : "add 'threadirqs' to the kernel command line, then use rtirq to raise the xhci_hcd interrupt");

    // CPU.
    const auto pcores = performanceCpus(), ecores = efficiencyCpus();
    const int audioCpu = pickAudioCpu();
    if (!pcores.empty())
        add(Level::Info, "Hybrid CPU", std::to_string(primaryThreads(pcores).size()) + " performance cores (CPUs " + readFileTrimmed("/sys/devices/cpu_core/cpus") +
            "), " + std::to_string(ecores.size()) + " efficiency cores (CPUs " + readFileTrimmed("/sys/devices/cpu_atom/cpus") + "). Audio thread goes on CPU " + std::to_string(audioCpu));
    std::map<std::string, int> governors, epps;
    for (int c = 0;; ++c) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/";
        const std::string g = readFileTrimmed(base + "scaling_governor");
        if (g.empty())
            break;
        governors[g]++;
        const std::string e = readFileTrimmed(base + "energy_performance_preference");
        if (!e.empty())
            epps[e]++;
    }
    auto summary = [](const std::map<std::string, int>& m) {
        std::string s;
        for (auto& [k, v] : m)
            s += (s.empty() ? "" : ", ") + k + " x" + std::to_string(v);
        return s;
    };
    if (governors.empty())
        add(Level::Info, "CPU governor", "not exposed (virtual machine or no cpufreq)");
    else if (governors.size() == 1 && governors.count("performance"))
        add(Level::Ok, "CPU governor", summary(governors));
    else
        add(Level::Warn, "CPU governor", summary(governors), "set 'performance' on all CPUs, for example with cpupower frequency-set -g performance");
    if (!epps.empty())
        add(epps.size() == 1 && epps.count("performance") ? Level::Ok : Level::Warn, "Energy preference", summary(epps),
            epps.size() == 1 && epps.count("performance") ? "" : "write 'performance' to each CPU's energy_performance_preference");
    const std::string noTurbo = readFileTrimmed("/sys/devices/system/cpu/intel_pstate/no_turbo");
    if (!noTurbo.empty())
        add(Level::Info, "Turbo", noTurbo == "1" ? "off: steady clocks, less peak speed" : "on: more speed, clocks vary with heat (35 W limit)");
    const std::string smt = readFileTrimmed("/sys/devices/system/cpu/smt/control");
    if (!smt.empty())
        add(Level::Info, "Hyper-threading", smt + (smt == "on" ? ": the audio core's sibling thread can steal time; the prototype measures whether it matters" : ""));

    // Interrupts and USB.
    const std::string autosuspend = readFileTrimmed("/sys/module/usbcore/parameters/autosuspend");
    if (!autosuspend.empty())
        add(autosuspend == "-1" ? Level::Ok : Level::Warn, "USB autosuspend", autosuspend == "-1" ? "off" : autosuspend + " s: the interface may be suspended",
            autosuspend == "-1" ? "" : "add 'usbcore.autosuspend=-1' to the kernel command line");
    if (processRunning("irqbalance"))
        add(Level::Warn, "irqbalance", "running: it can move the USB interrupt between CPUs mid-show", "disable it: systemctl disable --now irqbalance");
    const bool rtirq = !readFileTrimmed("/etc/default/rtirq").empty() || !readFileTrimmed("/etc/rtirq.conf").empty();
    add(rtirq ? Level::Ok : Level::Info, "rtirq", rtirq ? "installed" : "not found", rtirq ? "" : "install rtirq to give the USB interrupt real-time priority");

    // Memory.
    const std::string swappiness = readFileTrimmed("/proc/sys/vm/swappiness");
    if (!swappiness.empty())
        add(std::stoi(swappiness) <= 10 ? Level::Ok : Level::Info, "Swappiness", swappiness, std::stoi(swappiness) <= 10 ? "" : "vm.swappiness=10 keeps loaded plug-ins in RAM");

    // Sound servers that may hold the interface.
    std::string servers;
    for (const char* name : {"pipewire", "pulseaudio", "jackd", "jackdbus", "wireplumber"})
        if (processRunning(name))
            servers += (servers.empty() ? "" : ", ") + std::string(name);
    if (!servers.empty())
        add(Level::Warn, "Sound servers", servers + " running: they may hold the interface or add load",
            "stop them for ALSA-direct use, or reserve the M2 for this program");
    else
        add(Level::Ok, "Sound servers", "none running");

    // The interface.
    const std::string cards = readFileTrimmed("/proc/asound/cards");
    if (!opt.deviceMatch.empty())
        add(cards.find(opt.deviceMatch) != std::string::npos ? Level::Ok : Level::Fail, "Audio interface",
            cards.find(opt.deviceMatch) != std::string::npos ? "'" + opt.deviceMatch + "' present" : "'" + opt.deviceMatch + "' not in /proc/asound/cards");

    int fails = 0, warns = 0;
    std::printf("Cue doctor\n\n");
    for (const auto& r : results) {
        std::printf("%s  %-20s %s\n", label(r.level), r.check.c_str(), r.detail.c_str());
        if (!r.fix.empty())
            std::printf("       %-20s fix: %s\n", "", r.fix.c_str());
        fails += r.level == Level::Fail;
        warns += r.level == Level::Warn;
    }
    std::printf("\n%d failed, %d warnings\n", fails, warns);
    return fails ? 1 : 0;
}

} // namespace cue
