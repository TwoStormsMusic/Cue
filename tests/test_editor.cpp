// Tests of the windows plug-in editors live in, against the test plug-in's
// own small editor (plugins/test, cue.test.gain).
//
// They need a screen. Without one (no DISPLAY) they say so and pass, as they
// do on a build machine; CI starts Xvfb for them, and on the rig they use the
// desktop.

#include "check.h"

#include "host/clap_processor.h"
#include "host/editor_host.h"

#include <dlfcn.h>

#ifdef CUE_HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace cue;

namespace {

const char* kTestClap = std::getenv("CUE_TEST_CLAP") ? std::getenv("CUE_TEST_CLAP") : CUE_TEST_CLAP;

long stat(const char* name) {
    static auto fn = [] {
        void* h = dlopen(kTestClap, RTLD_NOW | RTLD_NOLOAD);
        return h ? reinterpret_cast<long (*)(const char*)>(dlsym(h, "cue_test_stat")) : nullptr;
    }();
    return fn ? fn(name) : -1;
}

std::unique_ptr<ClapProcessor> loadGain() {
    std::string err;
    auto p = ClapProcessor::load(kTestClap, "cue.test.gain", &err);
    if (!p)
        std::printf("    load failed: %s\n", err.c_str());
    return p;
}

// Turns the loop for a while, as `cue run` does: plug-in callbacks and timers,
// then the editors' own events.
void turn(EditorHost& editors, ClapProcessor& plugin, int times = 40) {
    for (int i = 0; i < times; ++i) {
        plugin.pumpMainThread();
        editors.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Says why, once, when there is no screen to test on.
bool noScreen(EditorHost& editors) {
    std::string why;
    if (editors.available(&why))
        return false;
    std::printf("    skipped: %s\n", why.c_str());
    return true;
}

} // namespace

TEST(editor_opens_in_a_window_of_the_hosts_and_draws) {
    EditorHost editors;
    if (noScreen(editors))
        return;
    auto gain = loadGain();
    CHECK(gain != nullptr);
    CHECK(gain->hasEditor());
    CHECK(!gain->editorOpen());

    const long drawsBefore = stat("editor_draws");
    std::string err;
    CHECK_MSG(editors.open(*gain, "Test Gain", &err), "%s", err.c_str());
    CHECK(gain->editorOpen());
    CHECK(editors.isOpen(*gain));
    CHECK(editors.count() == 1);
    // The plug-in asked the host to watch its own connection to the screen.
    std::vector<int> fds;
    editors.fds(fds);
    CHECK(fds.size() >= 2); // the host's window and the plug-in's connection

    turn(editors, *gain);
    CHECK_MSG(stat("editor_draws") > drawsBefore, "it never drew (%ld draws)", stat("editor_draws"));
    CHECK_MSG(stat("editor_open") == 1, "the plug-in counts %ld open editors", stat("editor_open"));

    editors.close(*gain);
    CHECK(!gain->editorOpen());
    CHECK(editors.count() == 0);
    CHECK(stat("editor_open") == 0);
    // Nothing is left for the loop to wait on.
    fds.clear();
    editors.fds(fds);
    CHECK(fds.empty());
}

TEST(two_editors_open_at_once_and_one_closes_without_the_other) {
    EditorHost editors;
    if (noScreen(editors))
        return;
    auto first = loadGain(), second = loadGain();
    CHECK(first && second);
    std::string err;
    CHECK_MSG(editors.open(*first, "First", &err), "%s", err.c_str());
    // The second window must reach the screen before the plug-in attaches to
    // it; when it does not, this one comes up blank.
    const long drawsBefore = stat("editor_draws");
    CHECK_MSG(editors.open(*second, "Second", &err), "%s", err.c_str());
    CHECK(editors.count() == 2);
    CHECK(stat("editor_open") == 2);
    turn(editors, *second);
    CHECK_MSG(stat("editor_draws") > drawsBefore, "the second editor never drew");

    editors.close(*first);
    CHECK(!first->editorOpen());
    CHECK(second->editorOpen());
    CHECK(editors.count() == 1);
    turn(editors, *second, 10);
    editors.closeAll();
    CHECK(editors.count() == 0);
    CHECK(stat("editor_open") == 0);
}

TEST(editor_closes_with_the_host_and_the_plugin_outlives_it) {
    EditorHost editors;
    if (noScreen(editors))
        return;
    auto gain = loadGain();
    CHECK(gain != nullptr);
    CHECK(gain->activate(48000, 64));
    std::string err;
    CHECK(editors.open(*gain, "Test Gain", &err));
    turn(editors, *gain, 10);
    {
        // An editor left open when the host goes closes with it. The plug-in
        // outlives its host here, as it does in the engine.
        auto second = loadGain();
        {
            EditorHost other;
            CHECK(other.open(*second, "Second", &err));
            turn(other, *second, 5);
        }
        CHECK(!second->editorOpen());
    }
    CHECK(stat("editor_open") == 1);
    // The plug-in still works: the sound never depends on its editor.
    CHECK(gain->setParam(0, 0.5));
    gain->startProcessing();
    float left[64] = {}, right[64] = {};
    for (float& v : left)
        v = 1.0f;
    for (float& v : right)
        v = 1.0f;
    const float* in[2] = {left, right};
    float outL[64] = {}, outR[64] = {};
    float* out[2] = {outL, outR};
    gain->process(in, out, 64);
    CHECK_MSG(std::abs(outL[63] - 0.5f) < 1e-5f, "the gain is %f, not 0.5", outL[63]);
    gain->stopProcessing();
    editors.closeAll();
    gain->deactivate();
}

#ifdef CUE_HAVE_X11
TEST(editor_stays_over_the_apps_window_and_leaves_it_the_focus) {
    EditorHost editors;
    if (noScreen(editors))
        return;
    // The app's window, as the app would have it.
    Display* d = XOpenDisplay(nullptr);
    CHECK(d != nullptr);
    const ::Window root = DefaultRootWindow(d);
    const ::Window app = XCreateSimpleWindow(d, root, 0, 0, 400, 300, 0, 0, 0);
    XMapWindow(d, app);
    XSync(d, False);

    auto gain = loadGain();
    CHECK(gain != nullptr);
    std::string err;
    CHECK_MSG(editors.open(*gain, "Gain Stage — Punk", &err, app), "%s", err.c_str());
    turn(editors, *gain, 5);
    XSync(d, False);

    // The editor's window is the one the window manager is told to keep over the app's.
    ::Window editor = 0, parent = 0, *children = nullptr;
    unsigned n = 0;
    XQueryTree(d, root, &parent, &parent, &children, &n);
    for (unsigned i = 0; i < n; ++i) {
        ::Window over = 0;
        if (XGetTransientForHint(d, children[i], &over) && over == app)
            editor = children[i];
    }
    if (children)
        XFree(children);
    CHECK_MSG(editor != 0, "no window is kept over the app's");
    // It never takes the focus from the app.
    XWMHints* hints = XGetWMHints(d, editor);
    CHECK(hints && (hints->flags & InputHint) && !hints->input);
    if (hints)
        XFree(hints);
    // Its title reads as written, dash and all.
    Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* title = nullptr;
    XGetWindowProperty(d, editor, XInternAtom(d, "_NET_WM_NAME", False), 0, 256, False, XInternAtom(d, "UTF8_STRING", False), &type, &format,
                       &count, &after, &title);
    CHECK(title && std::string(reinterpret_cast<char*>(title), count) == "Gain Stage — Punk");
    if (title)
        XFree(title);
    char* plain = nullptr;
    CHECK(XFetchName(d, editor, &plain) && std::string(plain) == "Gain Stage - Punk");
    if (plain)
        XFree(plain);

    editors.closeAll();
    XDestroyWindow(d, app);
    XCloseDisplay(d);
}
#endif
