#include "host/editor_host.h"

#include "core/processor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifdef CUE_HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace cue {

#ifdef CUE_HAVE_X11

namespace {
// Keeps the plug-in alive if its window dies under it: X errors are global
// and fatal by default.
int ignoreXError(Display*, XErrorEvent* e) {
    std::fprintf(stderr, "editor: the display refused something (request %d, error %d); the editor may be blank\n", e->request_code,
                 e->error_code);
    return 0;
}
} // namespace

struct EditorHost::Impl {
    struct Window {
        Processor* plugin = nullptr;
        ::Window window = 0;
        std::string title;
    };

    Display* display = nullptr;
    Atom deleteWindow = 0, netName = 0, utf8 = 0;
    bool tried = false;
    std::string why;
    std::vector<Window> windows;
    ::Window owner = 0; // the app's window they are kept over: when it goes, they go

    bool connect(std::string* reason) {
        if (display)
            return true;
        if (!tried) {
            tried = true;
            if (!std::getenv("DISPLAY")) {
                why = "there is no screen to open it on (DISPLAY is not set); run the engine on the computer's desktop";
            } else if (!(display = XOpenDisplay(nullptr))) {
                why = std::string("cannot reach the screen at DISPLAY=") + std::getenv("DISPLAY");
            } else {
                XSetErrorHandler(ignoreXError);
                deleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
                netName = XInternAtom(display, "_NET_WM_NAME", False);
                utf8 = XInternAtom(display, "UTF8_STRING", False);
            }
        }
        if (!display && reason)
            *reason = why;
        return display != nullptr;
    }

    Window* find(const Processor* plugin) {
        for (Window& w : windows)
            if (w.plugin == plugin)
                return &w;
        return nullptr;
    }

    // Over the app's window, which keeps the focus.
    void keepAbove(::Window w, unsigned long above) {
        if (!above)
            return;
        XSetTransientForHint(display, w, static_cast<::Window>(above));
        if (owner != above) {
            owner = static_cast<::Window>(above);
            XSelectInput(display, owner, StructureNotifyMask);
        }
    }

    void resize(const Window& w, const EditorSize& size) {
        if (size.width && size.height)
            XResizeWindow(display, w.window, size.width, size.height);
    }

    void destroy(Window& w) {
        w.plugin->closeEditor();
        XDestroyWindow(display, w.window);
        XFlush(display);
    }
};

EditorHost::EditorHost() : impl_(new Impl) {}

EditorHost::~EditorHost() {
    closeAll();
    if (impl_->display)
        XCloseDisplay(impl_->display);
}

bool EditorHost::available(std::string* why) { return impl_->connect(why); }

size_t EditorHost::count() const { return impl_->windows.size(); }

bool EditorHost::isOpen(const Processor& plugin) const { return impl_->find(&plugin) != nullptr; }

bool EditorHost::open(Processor& plugin, const std::string& title, std::string* error, unsigned long above) {
    if (!plugin.hasEditor()) {
        *error = "this plug-in has no editor of its own";
        return false;
    }
    if (Impl::Window* open = impl_->find(&plugin)) {
        impl_->keepAbove(open->window, above);
        XRaiseWindow(impl_->display, open->window);
        XFlush(impl_->display);
        return true;
    }
    if (!impl_->connect(error))
        return false;
    Display* d = impl_->display;
    const int screen = DefaultScreen(d);
    // A window of the plug-in's size, which it fills; it is told the size
    // again once the window manager has had its say.
    ::Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, 640, 480, 0, BlackPixel(d, screen), BlackPixel(d, screen));
    if (!w) {
        *error = "the screen would not make a window for it";
        return false;
    }
    // The title in UTF-8 for window managers that read it so, and plain for
    // the rest.
    XChangeProperty(d, w, impl_->netName, impl_->utf8, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(title.data()),
                    static_cast<int>(title.size()));
    std::string plain;
    for (unsigned char c : title)
        if (c < 0x80)
            plain += static_cast<char>(c);
        else if (c >= 0xC0) // the first byte of a character; the rest are left out
            plain += '-';
    XStoreName(d, w, plain.c_str());
    XSetWMProtocols(d, w, &impl_->deleteWindow, 1);
    // The window manager never gives it the focus: a tap on the app after
    // one on an editor would otherwise go to bringing the app forward. A
    // plug-in that wants the keys (a value being typed) takes them itself,
    // except one under Wine, which types only into an active window.
    XWMHints wm{};
    wm.flags = InputHint | StateHint;
    wm.input = plugin.editorNeedsFocus() ? True : False;
    wm.initial_state = NormalState;
    XSetWMHints(d, w, &wm);
    impl_->keepAbove(w, above);
    XSelectInput(d, w, StructureNotifyMask);
    XClassHint hint{};
    char cls[] = "Cue", res[] = "cue";
    hint.res_name = res;
    hint.res_class = cls;
    XSetClassHint(d, w, &hint);

    // The plug-in attaches to this window over a connection of its own, so
    // the screen must know about it first. Without this the second editor
    // opened in a run attaches to a window that is still in our buffer, and
    // draws nowhere.
    XSync(d, False);

    EditorSize size;
    std::string err;
    if (!plugin.openEditor(static_cast<unsigned long>(w), &size, &err)) {
        XDestroyWindow(d, w);
        XFlush(d);
        *error = err;
        return false;
    }
    if (size.width && size.height) {
        XResizeWindow(d, w, size.width, size.height);
        if (!size.resizable) {
            // Fixed: ask the window manager not to offer a resize.
            XSizeHints hints{};
            hints.flags = PMinSize | PMaxSize;
            hints.min_width = hints.max_width = static_cast<int>(size.width);
            hints.min_height = hints.max_height = static_cast<int>(size.height);
            XSetWMNormalHints(d, w, &hints);
        }
    }
    XMapRaised(d, w);
    XFlush(d);
    impl_->windows.push_back({&plugin, w, title});
    return true;
}

void EditorHost::close(Processor& plugin) {
    Impl::Window* w = impl_->find(&plugin);
    if (!w)
        return;
    impl_->destroy(*w);
    impl_->windows.erase(impl_->windows.begin() + (w - impl_->windows.data()));
}

void EditorHost::closeAll() {
    if (!impl_->display)
        return;
    for (Impl::Window& w : impl_->windows)
        impl_->destroy(w);
    impl_->windows.clear();
}

void EditorHost::fds(std::vector<int>& out) const {
    if (impl_->display && !impl_->windows.empty())
        out.push_back(ConnectionNumber(impl_->display));
    for (const Impl::Window& w : impl_->windows)
        w.plugin->editorFds(out);
}

void EditorHost::pump() {
    if (!impl_->display || impl_->windows.empty())
        return;
    Display* d = impl_->display;
    // The plug-ins' own connections first: that is where their drawing comes
    // from. Each one is asked whether it has anything to do.
    std::vector<int> fds;
    for (const Impl::Window& w : impl_->windows) {
        fds.clear();
        w.plugin->editorFds(fds);
        for (int fd : fds)
            w.plugin->onEditorFd(fd);
    }
    // Then our own windows: a resize to pass on, or a close to obey.
    std::vector<Processor*> closing;
    bool ownerGone = false;
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        // The app was closed (or stopped): its editors close with it.
        if (e.type == DestroyNotify && impl_->owner && e.xdestroywindow.window == impl_->owner) {
            ownerGone = true;
            continue;
        }
        Impl::Window* w = nullptr;
        for (Impl::Window& candidate : impl_->windows)
            if (candidate.window == e.xany.window)
                w = &candidate;
        if (!w)
            continue;
        if (e.type == ConfigureNotify) {
            const auto& c = e.xconfigure;
            w->plugin->editorResized(static_cast<uint32_t>(c.width), static_cast<uint32_t>(c.height));
        } else if (e.type == ClientMessage && static_cast<Atom>(e.xclient.data.l[0]) == impl_->deleteWindow) {
            closing.push_back(w->plugin);
        }
    }
    // A plug-in that asked for another size (its own zoom, say).
    for (const Impl::Window& w : impl_->windows) {
        EditorSize size;
        if (w.plugin->takeEditorResize(&size))
            impl_->resize(w, size);
    }
    for (Processor* p : closing)
        close(*p);
    if (ownerGone) {
        impl_->owner = 0;
        closeAll();
    }
    XFlush(d);
}

#else // no X11 at build time

struct EditorHost::Impl {};

EditorHost::EditorHost() : impl_(new Impl) {}
EditorHost::~EditorHost() = default;

bool EditorHost::available(std::string* why) {
    if (why)
        *why = "this build has no X11, so it cannot open plug-in editors";
    return false;
}
bool EditorHost::open(Processor&, const std::string&, std::string* error, unsigned long) {
    available(error);
    return false;
}
void EditorHost::close(Processor&) {}
bool EditorHost::isOpen(const Processor&) const { return false; }
void EditorHost::closeAll() {}
size_t EditorHost::count() const { return 0; }
void EditorHost::pump() {}
void EditorHost::fds(std::vector<int>&) const {}

#endif

} // namespace cue
