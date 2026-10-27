// Test CLAP plug-ins for the host's own tests. One .clap file, six plug-ins:
//
//   cue.test.gain   stereo gain, parameter "Gain" (0..2, default 1)
//   cue.test.delay  feedback delay with a tail (500 ms, feedback 0.5)
//   cue.test.burn   burns CPU per sample, parameter "Load" (0..1)
//   cue.test.crash  passes audio through; parameter "Crash" >= 0.5 crashes it
//   cue.test.pedal  a volume pedal: gain = MIDI CC 11 / 127 (MIDI in, starts at 1)
//   cue.test.key    plays its side-chain input (a second stereo port), times
//                   "Gain"; registers a 20 ms timer and counts its calls
//
// cue.test.gain also has an editor: a plain X11 window it fills with a
// colour, which registers its own screen connection with the host, so the
// host's editor windows can be tested without an outside plug-in.
//
// They also record how the host drives them (reset, start/stop processing,
// process outside start/stop) in a few exported counters, so tests can check
// the host follows the CLAP threading and lifecycle rules.

#include <clap/clap.h>

#ifdef CUE_TEST_X11
#include <X11/Xlib.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class Kind { Gain, Delay, Burn, Crash, Pedal, Key };

struct Stats {
    std::atomic<int> instances{0};
    std::atomic<long> resets{0}, starts{0}, stops{0}, processWhileStopped{0}, processCalls{0};
    std::atomic<long> midiEvents{0}, lastCc{-1}, lastCcValue{-1};
    std::atomic<long> timerCalls{0};
    std::atomic<long> editorDraws{0}, editorFdCalls{0};
    std::atomic<int> editorOpen{0};
};
Stats g_stats;

struct Plugin {
    clap_plugin_t plugin;
    const clap_host_t* host;
    Kind kind;
    double sampleRate = 48000.0;
    bool processing = false;
    double param = 1.0; // Gain, Load or Crash depending on kind
    std::vector<float> line[2];
    size_t pos = 0;
    float burnAcc = 0.0f;
    clap_id timer = CLAP_INVALID_ID;
#ifdef CUE_TEST_X11
    // The editor: a connection of its own, as a real plug-in has.
    Display* display = nullptr;
    ::Window window = 0;
    clap_id guiTimer = CLAP_INVALID_ID;
    unsigned width = 320, height = 240;
    bool shown = false;
#endif
};

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};

#define DESCRIPTOR(var, id, nm)                                                                                         \
    const clap_plugin_descriptor_t var = {CLAP_VERSION_INIT, id, nm, "TwoStorms test", "", "", "", "0.1", "Host test plug-in", kFeatures}

DESCRIPTOR(kGainDesc, "cue.test.gain", "Test Gain");
DESCRIPTOR(kDelayDesc, "cue.test.delay", "Test Delay");
DESCRIPTOR(kBurnDesc, "cue.test.burn", "Test Burn");
DESCRIPTOR(kCrashDesc, "cue.test.crash", "Test Crash");
DESCRIPTOR(kPedalDesc, "cue.test.pedal", "Test Pedal");
DESCRIPTOR(kKeyDesc, "cue.test.key", "Test Key");
const clap_plugin_descriptor_t* kDescs[] = {&kGainDesc, &kDelayDesc, &kBurnDesc, &kCrashDesc, &kPedalDesc, &kKeyDesc};
constexpr uint32_t kNumDescs = 6;

constexpr double kDelayMs = 500.0;
constexpr float kFeedback = 0.5f;

// ---- audio ports: one stereo in, one stereo out; the key plug-in has a
// side-chain input too

uint32_t portsCount(const clap_plugin_t* p, bool isInput) { return isInput && self(p)->kind == Kind::Key ? 2 : 1; }
bool portsGet(const clap_plugin_t* p, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
    if (index >= portsCount(p, isInput))
        return false;
    std::memset(info, 0, sizeof *info);
    info->id = index;
    std::snprintf(info->name, sizeof info->name, "%s", index == 1 ? "Sidechain" : isInput ? "In" : "Out");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kPorts = {portsCount, portsGet};

// ---- params: one parameter, id 0

void paramRange(Kind kind, double& lo, double& hi, double& def, const char*& name) {
    switch (kind) {
    case Kind::Gain: lo = 0; hi = 2; def = 1; name = "Gain"; break;
    case Kind::Burn: lo = 0; hi = 1; def = 0.5; name = "Load"; break;
    case Kind::Crash: lo = 0; hi = 1; def = 0; name = "Crash"; break;
    case Kind::Delay: lo = 0; hi = 1; def = 0.5; name = "Mix"; break;
    case Kind::Pedal: lo = 0; hi = 1; def = 1; name = "Position"; break;
    case Kind::Key: lo = 0; hi = 2; def = 1; name = "Gain"; break;
    }
}
uint32_t paramsCount(const clap_plugin_t*) { return 1; }
bool paramsInfo(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info) {
    if (index != 0)
        return false;
    std::memset(info, 0, sizeof *info);
    const char* name = "";
    paramRange(self(p)->kind, info->min_value, info->max_value, info->default_value, name);
    info->id = 0;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof info->name, "%s", name);
    return true;
}
bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) {
    if (id != 0)
        return false;
    *v = self(p)->param;
    return true;
}
bool paramsToText(const clap_plugin_t*, clap_id, double v, char* buf, uint32_t cap) {
    std::snprintf(buf, cap, "%.3f", v);
    return true;
}
bool paramsFromText(const clap_plugin_t*, clap_id, const char* text, double* v) {
    *v = std::atof(text);
    return true;
}
void applyEvents(Plugin* s, const clap_input_events_t* in) {
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (ev->param_id == 0)
                s->param = ev->value;
        } else if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_MIDI) {
            const auto* ev = reinterpret_cast<const clap_event_midi_t*>(h);
            g_stats.midiEvents++;
            if ((ev->data[0] & 0xF0) == 0xB0) {
                g_stats.lastCc = ev->data[1];
                g_stats.lastCcValue = ev->data[2];
                if (s->kind == Kind::Pedal && ev->data[1] == 11)
                    s->param = ev->data[2] / 127.0;
            }
        }
    }
    if (s->kind == Kind::Crash && s->param >= 0.5)
        std::raise(SIGSEGV);
}
void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { applyEvents(self(p), in); }
const clap_plugin_params_t kParams = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};

// ---- state: the parameter as 8 bytes

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* out) {
    const double v = self(p)->param;
    return out->write(out, &v, sizeof v) == sizeof v;
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* in) {
    double v;
    if (in->read(in, &v, sizeof v) != sizeof v)
        return false;
    self(p)->param = v;
    return true;
}
const clap_plugin_state_t kState = {stateSave, stateLoad};

// ---- note ports (pedal only): one MIDI input

uint32_t notesCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }
bool notesGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
    if (index != 0 || !isInput)
        return false;
    std::memset(info, 0, sizeof *info);
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::snprintf(info->name, sizeof info->name, "MIDI in");
    return true;
}
const clap_plugin_note_ports_t kNotes = {notesCount, notesGet};

#ifdef CUE_TEST_X11
void guiDraw(Plugin* s);
#endif

// ---- tail (delay only)

uint32_t tailGet(const clap_plugin_t* p) {
    const Plugin* s = self(p);
    // Echoes fall 6 dB per repeat at feedback 0.5: 15 repeats reach -90 dB.
    return static_cast<uint32_t>(16 * kDelayMs * s->sampleRate / 1000.0);
}
const clap_plugin_tail_t kTail = {tailGet};

// ---- timer (key only)

void onTimer(const clap_plugin_t* p, clap_id id) {
    g_stats.timerCalls++;
#ifdef CUE_TEST_X11
    Plugin* s = self(p);
    if (id == s->guiTimer && s->guiTimer != CLAP_INVALID_ID)
        guiDraw(s);
#else
    (void)p;
    (void)id;
#endif
}
const clap_plugin_timer_support_t kTimer = {onTimer};

#ifdef CUE_TEST_X11
// What the host calls when the plug-in's own connection has something.
void onPosixFd(const clap_plugin_t* p, int fd, clap_posix_fd_flags_t) {
    Plugin* s = self(p);
    g_stats.editorFdCalls++;
    if (!s->display || fd != ConnectionNumber(s->display))
        return;
    while (XPending(s->display)) {
        XEvent e;
        XNextEvent(s->display, &e);
        if (e.type == Expose)
            guiDraw(s);
    }
}
const clap_plugin_posix_fd_support_t kPosixFd = {onPosixFd};
#endif

// ---- editor (gain only, when built with X11)

#ifdef CUE_TEST_X11
void guiDraw(Plugin* s) {
    if (!s->display || !s->window || !s->shown)
        return;
    GC gc = XCreateGC(s->display, s->window, 0, nullptr);
    XSetForeground(s->display, gc, static_cast<unsigned long>(s->param * 0x7f) << 16 | 0x2200ff);
    XFillRectangle(s->display, s->window, gc, 0, 0, s->width, s->height);
    XFreeGC(s->display, gc);
    XFlush(s->display);
    g_stats.editorDraws++;
}

bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool floating) {
    return !floating && !std::strcmp(api, CLAP_WINDOW_API_X11);
}
bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* floating) {
    *api = CLAP_WINDOW_API_X11;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) {
    Plugin* s = self(p);
    if (floating || std::strcmp(api, CLAP_WINDOW_API_X11))
        return false;
    s->display = XOpenDisplay(nullptr);
    return s->display != nullptr;
}
void guiDestroy(const clap_plugin_t* p) {
    Plugin* s = self(p);
    const auto* fds = static_cast<const clap_host_posix_fd_support_t*>(s->host->get_extension(s->host, CLAP_EXT_POSIX_FD_SUPPORT));
    const auto* timers = static_cast<const clap_host_timer_support_t*>(s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT));
    if (s->display && fds)
        fds->unregister_fd(s->host, ConnectionNumber(s->display));
    if (s->guiTimer != CLAP_INVALID_ID && timers)
        timers->unregister_timer(s->host, s->guiTimer);
    s->guiTimer = CLAP_INVALID_ID;
    if (s->window)
        XDestroyWindow(s->display, s->window);
    if (s->display)
        XCloseDisplay(s->display);
    s->window = 0;
    s->display = nullptr;
    s->shown = false;
    g_stats.editorOpen--;
}
bool guiSetScale(const clap_plugin_t*, double) { return false; }
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    *w = self(p)->width;
    *h = self(p)->height;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return true; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t*, uint32_t*, uint32_t*) { return true; }
bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    Plugin* s = self(p);
    s->width = w;
    s->height = h;
    if (s->display && s->window)
        XResizeWindow(s->display, s->window, w, h);
    guiDraw(s);
    return true;
}
bool guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    Plugin* s = self(p);
    if (!s->display || !window || std::strcmp(window->api, CLAP_WINDOW_API_X11))
        return false;
    const int screen = DefaultScreen(s->display);
    s->window = XCreateSimpleWindow(s->display, static_cast<::Window>(window->x11), 0, 0, s->width, s->height, 0, 0,
                                    BlackPixel(s->display, screen));
    if (!s->window)
        return false;
    XSelectInput(s->display, s->window, ExposureMask | StructureNotifyMask);
    XMapWindow(s->display, s->window);
    XFlush(s->display);
    if (const auto* fds = static_cast<const clap_host_posix_fd_support_t*>(s->host->get_extension(s->host, CLAP_EXT_POSIX_FD_SUPPORT)))
        fds->register_fd(s->host, ConnectionNumber(s->display), CLAP_POSIX_FD_READ);
    if (const auto* timers = static_cast<const clap_host_timer_support_t*>(s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT)))
        timers->register_timer(s->host, 16, &s->guiTimer);
    g_stats.editorOpen++;
    return true;
}
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t* p) {
    Plugin* s = self(p);
    s->shown = true;
    guiDraw(s);
    return true;
}
bool guiHide(const clap_plugin_t* p) {
    self(p)->shown = false;
    return true;
}
const clap_plugin_gui_t kGui = {guiIsApiSupported, guiGetPreferredApi, guiCreate,       guiDestroy,   guiSetScale,
                                guiGetSize,        guiCanResize,       guiGetResizeHints, guiAdjustSize, guiSetSize,
                                guiSetParent,      guiSetTransient,    guiSuggestTitle, guiShow,      guiHide};
#endif

// ---- plugin

bool init(const clap_plugin_t* p) {
    Plugin* s = self(p);
    if (s->kind == Kind::Key)
        if (auto* t = static_cast<const clap_host_timer_support_t*>(s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT)))
            t->register_timer(s->host, 20, &s->timer);
    return true;
}
void destroy(const clap_plugin_t* p) {
    Plugin* s = self(p);
    if (s->timer != CLAP_INVALID_ID)
        if (auto* t = static_cast<const clap_host_timer_support_t*>(s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT)))
            t->unregister_timer(s->host, s->timer);
    g_stats.instances--;
    delete s;
}
bool activate(const clap_plugin_t* p, double sr, uint32_t, uint32_t) {
    Plugin* s = self(p);
    s->sampleRate = sr;
    const size_t len = static_cast<size_t>(kDelayMs * sr / 1000.0);
    for (auto& l : s->line)
        l.assign(s->kind == Kind::Delay ? len : 1, 0.0f);
    s->pos = 0;
    return true;
}
void deactivate(const clap_plugin_t*) {}
bool startProcessing(const clap_plugin_t* p) {
    self(p)->processing = true;
    g_stats.starts++;
    return true;
}
void stopProcessing(const clap_plugin_t* p) {
    self(p)->processing = false;
    g_stats.stops++;
}
void reset(const clap_plugin_t* p) {
    Plugin* s = self(p);
    for (auto& l : s->line)
        std::fill(l.begin(), l.end(), 0.0f);
    s->pos = 0;
    g_stats.resets++;
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* proc) {
    Plugin* s = self(p);
    g_stats.processCalls++;
    if (!s->processing)
        g_stats.processWhileStopped++;
    applyEvents(s, proc->in_events);
    if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1)
        return CLAP_PROCESS_ERROR;
    float* const* in = proc->audio_inputs[0].data32;
    float* const* out = proc->audio_outputs[0].data32;
    const uint32_t frames = proc->frames_count;

    switch (s->kind) {
    case Kind::Gain:
    case Kind::Pedal:
        for (int c = 0; c < 2; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                out[c][n] = in[c][n] * static_cast<float>(s->param);
        break;
    case Kind::Delay: {
        const size_t len = s->line[0].size();
        size_t pos = s->pos;
        for (uint32_t n = 0; n < frames; ++n) {
            for (int c = 0; c < 2; ++c) {
                const float echo = s->line[c][pos];
                s->line[c][pos] = in[c][n] + echo * kFeedback;
                out[c][n] = in[c][n] + echo * static_cast<float>(s->param);
            }
            if (++pos == len)
                pos = 0;
        }
        s->pos = pos;
        break;
    }
    case Kind::Burn: {
        const uint32_t ops = static_cast<uint32_t>(s->param * 2000.0);
        float acc = s->burnAcc;
        for (uint32_t n = 0; n < frames; ++n) {
            for (uint32_t k = 0; k < ops; ++k)
                acc = acc * 0.999999f + 1.0e-7f;
            for (int c = 0; c < 2; ++c)
                out[c][n] = in[c][n] + acc * 1.0e-12f;
        }
        s->burnAcc = acc;
        break;
    }
    case Kind::Crash:
        for (int c = 0; c < 2; ++c)
            std::memcpy(out[c], in[c], frames * sizeof(float));
        break;
    case Kind::Key: {
        float* const* key = proc->audio_inputs_count > 1 ? proc->audio_inputs[1].data32 : nullptr;
        for (int c = 0; c < 2; ++c)
            for (uint32_t n = 0; n < frames; ++n)
                out[c][n] = key ? key[c][n] * static_cast<float>(s->param) : 0.0f;
        break;
    }
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* getExtension(const clap_plugin_t* p, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kPorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kState;
    if (!std::strcmp(id, CLAP_EXT_TAIL) && self(p)->kind == Kind::Delay)
        return &kTail;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS) && self(p)->kind == Kind::Pedal)
        return &kNotes;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) && (self(p)->kind == Kind::Key || self(p)->kind == Kind::Gain))
        return &kTimer;
#ifdef CUE_TEST_X11
    if (!std::strcmp(id, CLAP_EXT_GUI) && self(p)->kind == Kind::Gain)
        return &kGui;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) && self(p)->kind == Kind::Gain)
        return &kPosixFd;
#endif
    return nullptr;
}
void onMainThread(const clap_plugin_t*) {}

// ---- factory and entry

uint32_t factoryCount(const clap_plugin_factory_t*) { return kNumDescs; }
const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t i) { return i < kNumDescs ? kDescs[i] : nullptr; }
// For the scanner's tests: a copy of this file named scan-crash-open falls
// over as it is opened, scan-hang-open never answers, and scan-crash-make
// falls over as a plug-in is made.
std::string g_file;

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (g_file.find("scan-crash-make") != std::string::npos)
        std::abort();
    for (uint32_t i = 0; i < kNumDescs; ++i) {
        if (std::strcmp(id, kDescs[i]->id) != 0)
            continue;
        auto* s = new Plugin();
        s->host = host;
        s->kind = static_cast<Kind>(i);
        double lo, hi, def;
        const char* name;
        paramRange(s->kind, lo, hi, def, name);
        s->param = def;
        s->plugin = {kDescs[i], s, init, destroy, activate, deactivate, startProcessing, stopProcessing,
                     reset, process, getExtension, onMainThread};
        g_stats.instances++;
        return &s->plugin;
    }
    return nullptr;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDesc, factoryCreate};

bool entryInit(const char* path) {
    g_file = path ? path : "";
    if (g_file.find("scan-crash-open") != std::string::npos)
        std::abort();
    while (g_file.find("scan-hang-open") != std::string::npos)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    return true;
}
void entryDeinit() {}
const void* entryFactory(const char* id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

} // namespace

extern "C" {
extern CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};

// Lifecycle counters for the host tests (not part of CLAP).
CLAP_EXPORT long cue_test_stat(const char* name) {
    if (!std::strcmp(name, "instances")) return g_stats.instances;
    if (!std::strcmp(name, "resets")) return g_stats.resets;
    if (!std::strcmp(name, "starts")) return g_stats.starts;
    if (!std::strcmp(name, "stops")) return g_stats.stops;
    if (!std::strcmp(name, "process_while_stopped")) return g_stats.processWhileStopped;
    if (!std::strcmp(name, "process_calls")) return g_stats.processCalls;
    if (!std::strcmp(name, "midi_events")) return g_stats.midiEvents;
    if (!std::strcmp(name, "last_cc")) return g_stats.lastCc;
    if (!std::strcmp(name, "last_cc_value")) return g_stats.lastCcValue;
    if (!std::strcmp(name, "timer_calls")) return g_stats.timerCalls;
    if (!std::strcmp(name, "editor_draws")) return g_stats.editorDraws;
    if (!std::strcmp(name, "editor_fd_calls")) return g_stats.editorFdCalls;
    if (!std::strcmp(name, "editor_open")) return g_stats.editorOpen;
    return -1;
}
}
