#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace cue {

// A program that has a sound card's PCM open.
struct DeviceHolder {
    pid_t pid;
    std::string name; // its /proc comm: "reaper", "pipewire"
};

// The ALSA card and device numbers of a "hw:M2", "hw:0,0", "plughw:CARD=M2,DEV=0"
// name; false for names that are not one card (default, pipewire, ...).
bool cardOf(const std::string& device, int& card, int& pcm);

// The programs other than this one with that card's PCM open.
std::vector<DeviceHolder> deviceHolders(int card, int pcm);

// Sound servers that hand a card over when asked, rather than being closed.
bool isSoundServer(const std::string& name);

// Takes a sound card from whatever else is using it, for as long as this
// object lives.
//
// First the desktop's device reservation (org.freedesktop.ReserveDevice1 on
// the session bus): PipeWire, PulseAudio and JACK keep a card under it and
// let go when a program with a higher priority asks, and do not take it back
// while that program holds it. Then, if a program that does not take part
// (REAPER on ALSA) still has the card open, it is closed: asked to quit, and
// made to after a few seconds.
class DeviceClaim {
public:
    DeviceClaim() = default;
    DeviceClaim(const DeviceClaim&) = delete;
    DeviceClaim& operator=(const DeviceClaim&) = delete;
    ~DeviceClaim();

    // Reserves the card, asking the server that has it to let go. Each
    // thing it did goes into notes. Quietly does nothing without a session bus.
    void reserve(const std::string& device, std::vector<std::string>& notes);
    // The same for one card number, under a priority: a program asking with
    // a higher one is given it (released() is then true). Cue asks with the
    // highest there is, so it is never asked to give it back.
    bool reserveCard(int card, const std::string& device, int32_t priority, std::vector<std::string>& notes);
    bool reserved() const { return bus_ != nullptr && !released_; }
    bool released() const { return released_; }

    // Closes the programs that still have the card open, calling open until
    // it works. False if it never did (the card is not there, or a sound
    // server kept it).
    bool takeFrom(const std::string& device, const std::function<bool()>& open, std::vector<std::string>& notes);

private:
    void answer(); // the reservation's thread: answers anyone asking for the card

    void* bus_ = nullptr;
    std::string name_;
    int32_t priority_ = 0;
    std::thread thread_;
    std::atomic<bool> run_{false}, released_{false};
};

} // namespace cue
