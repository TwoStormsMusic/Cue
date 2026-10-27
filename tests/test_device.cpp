// Taking the sound card from what else has it: the card names, and the
// desktop's device reservation on a session bus of the test's own.

#include "check.h"

#include "audio/take_device.h"

#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace cue;

TEST(device_names_that_are_not_one_card_are_left_alone) {
    int card, pcm;
    CHECK(!cardOf("default", card, pcm));
    CHECK(!cardOf("pipewire", card, pcm));
    CHECK(!cardOf("dmix:M2", card, pcm));
    CHECK(!cardOf("hw:", card, pcm));
    CHECK(!cardOf("hw:NoSuchCardHere", card, pcm));
    CHECK(isSoundServer("pipewire") && isSoundServer("wireplumber") && isSoundServer("jackd"));
    CHECK(!isSoundServer("reaper") && !isSoundServer("cue"));
}

TEST(the_card_is_reserved_from_a_sound_server_and_kept) {
    // A bus of its own, so nothing on the machine's is touched.
    FILE* out = popen("dbus-daemon --session --fork --print-address=1 --print-pid=1 2>/dev/null", "r");
    if (!out) {
        std::printf("  (no dbus-daemon: skipped)\n");
        return;
    }
    char address[512] = {}, pid[32] = {};
    const bool started = std::fgets(address, sizeof address, out) && std::fgets(pid, sizeof pid, out);
    pclose(out);
    if (!started) {
        std::printf("  (no dbus-daemon: skipped)\n");
        return;
    }
    address[std::string(address).find_last_not_of("\n") + 1] = 0;
    const char* before = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const std::string saved = before ? before : "";
    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
    {
        std::vector<std::string> notes;
        // As PipeWire keeps a card: under a low priority.
        DeviceClaim server;
        CHECK(server.reserveCard(42, "hw:Test", -20, notes) && server.reserved());
        CHECK(notes.empty());

        DeviceClaim cue;
        CHECK(cue.reserveCard(42, "hw:Test", INT32_MAX, notes));
        CHECK(cue.reserved() && server.released());
        CHECK_MSG(notes.size() == 1 && notes[0].find("let go of it") != std::string::npos && notes[0].find("would not") == std::string::npos,
                  "%s", notes.empty() ? "" : notes[0].c_str());

        // Nobody gets it back while Cue plays.
        notes.clear();
        DeviceClaim another;
        CHECK(!another.reserveCard(42, "hw:Test", INT32_MAX, notes) && !another.reserved());
        CHECK(notes.size() == 1 && notes[0].find("would not let go") != std::string::npos);
        CHECK(cue.reserved());
    }
    {
        // Once Cue is gone the card is free again.
        std::vector<std::string> notes;
        DeviceClaim server;
        CHECK(server.reserveCard(42, "hw:Test", -20, notes) && notes.empty());
    }
    if (saved.empty())
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
    else
        setenv("DBUS_SESSION_BUS_ADDRESS", saved.c_str(), 1);
    kill(std::atoi(pid), SIGTERM);
}
