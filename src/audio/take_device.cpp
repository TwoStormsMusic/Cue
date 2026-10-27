#include "audio/take_device.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <type_traits>

namespace cue {

bool cardOf(const std::string& device, int& card, int& pcm) {
    const size_t colon = device.find(':');
    if (colon == std::string::npos)
        return false;
    const std::string kind = device.substr(0, colon);
    if (kind != "hw" && kind != "plughw")
        return false;
    std::string name;
    pcm = 0;
    size_t at = colon + 1;
    for (int field = 0; at <= device.size(); ++field) {
        size_t comma = device.find(',', at);
        if (comma == std::string::npos)
            comma = device.size();
        std::string part = device.substr(at, comma - at);
        at = comma + 1;
        if (part.rfind("CARD=", 0) == 0)
            name = part.substr(5);
        else if (part.rfind("DEV=", 0) == 0)
            pcm = std::atoi(part.c_str() + 4);
        else if (field == 0)
            name = part;
        else if (field == 1)
            pcm = std::atoi(part.c_str());
    }
    if (name.empty())
        return false;
    card = snd_card_get_index(name.c_str());
    return card >= 0;
}

std::vector<DeviceHolder> deviceHolders(int card, int pcm) {
    std::vector<DeviceHolder> out;
    char want[64];
    const int wantLen = std::snprintf(want, sizeof want, "/dev/snd/pcmC%dD%d", card, pcm);
    DIR* proc = opendir("/proc");
    if (!proc)
        return out;
    const pid_t self = getpid();
    while (dirent* e = readdir(proc)) {
        char* end;
        const long pid = std::strtol(e->d_name, &end, 10);
        if (*end || pid <= 0 || pid == self)
            continue;
        const std::string dir = std::string("/proc/") + e->d_name;
        DIR* fds = opendir((dir + "/fd").c_str());
        if (!fds)
            continue; // someone else's program
        bool holds = false;
        while (dirent* f = readdir(fds)) {
            char link[256];
            const ssize_t n = readlinkat(dirfd(fds), f->d_name, link, sizeof link - 1);
            if (n <= wantLen)
                continue;
            link[n] = 0;
            // pcmC0D0c or pcmC0D0p: capture or playback.
            if (std::strncmp(link, want, wantLen) == 0 && (link[wantLen] == 'c' || link[wantLen] == 'p') && !link[wantLen + 1])
                holds = true;
        }
        closedir(fds);
        if (!holds)
            continue;
        std::string name;
        std::ifstream(dir + "/comm") >> name;
        out.push_back({static_cast<pid_t>(pid), name});
    }
    closedir(proc);
    return out;
}

bool isSoundServer(const std::string& name) {
    for (const char* s : {"pipewire", "wireplumber", "pulseaudio", "jackd", "jackdbus"})
        if (name == s)
            return true;
    return false;
}

namespace {

// libdbus, opened when it is wanted so Cue runs where there is none. Only
// the few calls a device reservation takes, declared as dbus/dbus.h has them.
struct DBusErrorC {
    const char* name;
    const char* message;
    unsigned int dummy;
    void* padding;
};
constexpr int kSessionBus = 0;
constexpr int kTypeInvalid = 0, kTypeBoolean = 'b', kTypeInt32 = 'i';
constexpr int kMethodCall = 1;
constexpr unsigned kAllowReplacement = 1, kReplaceExisting = 2, kDoNotQueue = 4;
constexpr int kPrimaryOwner = 1, kAlreadyOwner = 4, kExists = 3;

struct DBus {
    void (*errorInit)(DBusErrorC*);
    void (*errorFree)(DBusErrorC*);
    unsigned (*errorIsSet)(const DBusErrorC*);
    unsigned (*threadsInit)();
    void* (*busGetPrivate)(int, DBusErrorC*);
    void (*setExitOnDisconnect)(void*, unsigned);
    int (*requestName)(void*, const char*, unsigned, DBusErrorC*);
    int (*releaseName)(void*, const char*, DBusErrorC*);
    void* (*newMethodCall)(const char*, const char*, const char*, const char*);
    void* (*newMethodReturn)(void*);
    void* (*newError)(void*, const char*, const char*);
    unsigned (*appendArgs)(void*, int, ...);
    unsigned (*getArgs)(void*, DBusErrorC*, int, ...);
    void* (*sendBlock)(void*, void*, int, DBusErrorC*);
    unsigned (*send)(void*, void*, uint32_t*);
    void (*flush)(void*);
    unsigned (*readWrite)(void*, int);
    void* (*popMessage)(void*);
    int (*messageType)(void*);
    const char* (*member)(void*);
    unsigned (*noReply)(void*);
    void (*unrefMessage)(void*);
    void (*close)(void*);
    void (*unref)(void*);

    static const DBus* get() {
        static DBus d;
        static const bool ok = d.load();
        return ok ? &d : nullptr;
    }

private:
    bool load() {
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        bool ok = true;
        auto sym = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
            ok = ok && f;
        };
        sym(errorInit, "dbus_error_init");
        sym(errorFree, "dbus_error_free");
        sym(errorIsSet, "dbus_error_is_set");
        sym(threadsInit, "dbus_threads_init_default");
        sym(busGetPrivate, "dbus_bus_get_private");
        sym(setExitOnDisconnect, "dbus_connection_set_exit_on_disconnect");
        sym(requestName, "dbus_bus_request_name");
        sym(releaseName, "dbus_bus_release_name");
        sym(newMethodCall, "dbus_message_new_method_call");
        sym(newMethodReturn, "dbus_message_new_method_return");
        sym(newError, "dbus_message_new_error");
        sym(appendArgs, "dbus_message_append_args");
        sym(getArgs, "dbus_message_get_args");
        sym(sendBlock, "dbus_connection_send_with_reply_and_block");
        sym(send, "dbus_connection_send");
        sym(flush, "dbus_connection_flush");
        sym(readWrite, "dbus_connection_read_write");
        sym(popMessage, "dbus_connection_pop_message");
        sym(messageType, "dbus_message_get_type");
        sym(member, "dbus_message_get_member");
        sym(noReply, "dbus_message_get_no_reply");
        sym(unrefMessage, "dbus_message_unref");
        sym(close, "dbus_connection_close");
        sym(unref, "dbus_connection_unref");
        return ok;
    }
};

// How each program that keeps a card under the reservation is called.
std::string serverName(const std::string& comm) {
    if (comm == "pipewire" || comm == "wireplumber")
        return "PipeWire";
    if (comm == "pulseaudio")
        return "PulseAudio";
    if (comm == "jackd" || comm == "jackdbus")
        return "JACK";
    return comm;
}

bool alive(pid_t pid) { return kill(pid, 0) == 0 || errno == EPERM; }

} // namespace

DeviceClaim::~DeviceClaim() {
    run_ = false;
    if (thread_.joinable())
        thread_.join();
    if (bus_) {
        const DBus* d = DBus::get();
        d->close(bus_);
        d->unref(bus_);
    }
}

void DeviceClaim::reserve(const std::string& device, std::vector<std::string>& notes) {
    int card, pcm;
    if (cardOf(device, card, pcm))
        reserveCard(card, device, INT32_MAX, notes);
}

bool DeviceClaim::reserveCard(int card, const std::string& device, int32_t priority, std::vector<std::string>& notes) {
    const DBus* d = DBus::get();
    if (bus_ || !d || !std::getenv("DBUS_SESSION_BUS_ADDRESS"))
        return false;
    d->threadsInit();
    DBusErrorC err;
    d->errorInit(&err);
    void* bus = d->busGetPrivate(kSessionBus, &err);
    d->errorFree(&err);
    if (!bus)
        return false;
    d->setExitOnDisconnect(bus, 0);
    name_ = "org.freedesktop.ReserveDevice1.Audio" + std::to_string(card);
    const std::string path = "/org/freedesktop/ReserveDevice1/Audio" + std::to_string(card);
    // Under the highest priority there is, nobody can ask for it back.
    const unsigned flags = kDoNotQueue | (priority < INT32_MAX ? kAllowReplacement : 0);
    d->errorInit(&err);
    int got = d->requestName(bus, name_.c_str(), flags, &err);
    d->errorFree(&err);
    if (got == kExists) {
        // Someone keeps it: asked to let go.
        d->errorInit(&err);
        void* call = d->newMethodCall(name_.c_str(), path.c_str(), "org.freedesktop.ReserveDevice1", "RequestRelease");
        d->appendArgs(call, kTypeInt32, &priority, kTypeInvalid);
        void* reply = d->sendBlock(bus, call, 3000, &err);
        d->unrefMessage(call);
        uint32_t released = 0;
        if (reply) {
            d->getArgs(reply, &err, kTypeBoolean, &released, kTypeInvalid);
            d->unrefMessage(reply);
        }
        d->errorFree(&err);
        if (released) {
            d->errorInit(&err);
            got = d->requestName(bus, name_.c_str(), flags | kReplaceExisting, &err);
            d->errorFree(&err);
        }
        notes.push_back(got == kPrimaryOwner ? "the sound server that had " + device + " let go of it"
                                             : "the sound server that has " + device + " would not let go of it");
    }
    if (got != kPrimaryOwner && got != kAlreadyOwner) {
        d->close(bus);
        d->unref(bus);
        return false;
    }
    bus_ = bus;
    priority_ = priority;
    released_ = false;
    run_ = true;
    thread_ = std::thread([this] { answer(); });
    return true;
}

void DeviceClaim::answer() {
    const DBus* d = DBus::get();
    while (run_ && d->readWrite(bus_, 50)) {
        while (void* m = d->popMessage(bus_)) {
            if (d->messageType(m) == kMethodCall && !d->noReply(m)) {
                void* reply;
                const char* member = d->member(m);
                if (member && std::strcmp(member, "RequestRelease") == 0) {
                    // Only to a higher priority, which nothing has over Cue's.
                    int32_t asked = INT32_MIN;
                    DBusErrorC err;
                    d->errorInit(&err);
                    d->getArgs(m, &err, kTypeInt32, &asked, kTypeInvalid);
                    d->errorFree(&err);
                    const uint32_t yes = asked > priority_ && !released_;
                    if (yes) {
                        d->errorInit(&err);
                        d->releaseName(bus_, name_.c_str(), &err);
                        d->errorFree(&err);
                        released_ = true;
                    }
                    reply = d->newMethodReturn(m);
                    d->appendArgs(reply, kTypeBoolean, &yes, kTypeInvalid);
                } else {
                    reply = d->newError(m, "org.freedesktop.DBus.Error.UnknownMethod", "Cue has this card");
                }
                d->send(bus_, reply, nullptr);
                d->unrefMessage(reply);
                d->flush(bus_);
            }
            d->unrefMessage(m);
        }
    }
}

bool DeviceClaim::takeFrom(const std::string& device, const std::function<bool()>& open, std::vector<std::string>& notes) {
    int card, pcm;
    if (!cardOf(device, card, pcm))
        return false;
    // A program that is closing can keep the card a moment longer.
    auto waitOpen = [&](std::chrono::milliseconds limit) {
        const auto until = std::chrono::steady_clock::now() + limit;
        do {
            if (open())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (std::chrono::steady_clock::now() < until);
        return false;
    };
    std::vector<DeviceHolder> holders = deviceHolders(card, pcm);
    if (holders.empty())
        return waitOpen(std::chrono::milliseconds(500));
    std::vector<DeviceHolder> closing;
    for (const auto& h : holders) {
        if (isSoundServer(h.name)) {
            notes.push_back(serverName(h.name) + " has " + device + " and would not let go of it");
            continue;
        }
        if (kill(h.pid, SIGTERM) == 0) {
            notes.push_back("closed " + h.name + " (process " + std::to_string(h.pid) + "), which had " + device + " open");
            closing.push_back(h);
        }
    }
    if (closing.empty())
        return false;
    if (waitOpen(std::chrono::seconds(5)))
        return true;
    // Still there (asking to save, say): made to quit.
    for (const auto& h : closing)
        if (alive(h.pid) && kill(h.pid, SIGKILL) == 0)
            notes.push_back(h.name + " did not quit when asked, so it was stopped");
    return waitOpen(std::chrono::seconds(2));
}

} // namespace cue
