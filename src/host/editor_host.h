#pragma once

// The windows plug-in editors live in. Cue makes a plain top-level window on
// the stage computer's screen and the plug-in draws inside it, so the engine
// decides where editors go and can close them all at once.
//
// All of it runs on the main thread, in step with the control socket, so an
// editor never touches the audio thread. fds() says what to wait on, so an
// editor redraws as soon as something happens rather than at the next poll.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cue {

class Processor;

class EditorHost {
public:
    EditorHost();
    ~EditorHost();

    // Whether there is a screen to put editors on (DISPLAY, and it answers).
    // Says why not when there is not.
    bool available(std::string* why = nullptr);

    // Opens the plug-in's editor in a window of its own, or says why not.
    // Opening one that is already open brings it to the front. above is the
    // app's window, when the app is on this screen: the editor stays over it,
    // even full screen, and never takes the app's focus, so the app's next
    // tap acts at once (the plug-in still takes the keys when it needs them).
    bool open(Processor& plugin, const std::string& title, std::string* error, unsigned long above = 0);
    void close(Processor& plugin);
    bool isOpen(const Processor& plugin) const;
    void closeAll();
    size_t count() const;

    // Draws what is waiting: the windows' events, the plug-ins' own file
    // descriptors, and any size a plug-in has asked for. Call it as often as
    // the main loop turns.
    void pump();
    // What to wait on beside the control socket, and what to call when one
    // of them is ready (before the next pump).
    void fds(std::vector<int>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cue
