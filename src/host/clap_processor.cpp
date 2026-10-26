#include "clap_processor.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace cue {

// ---------------------------------------------------------------- threads

namespace {
std::thread::id g_mainThread;
std::once_flag g_mainThreadOnce;
std::atomic<uint64_t> g_droppedAudioThreadLogs{0};

void noteMainThread() {
    std::call_once(g_mainThreadOnce, [] { g_mainThread = std::this_thread::get_id(); });
}
} // namespace

void markClapAudioThread() { markAudioThread(); }

// ---------------------------------------------------------------- library

// One loaded .clap file. Plug-ins from the same file share one instance of
// the library, so plug-ins that share memory through it keep working.
class ClapLibrary {
public:
    static std::shared_ptr<ClapLibrary> open(const std::string& path, std::string* error) {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<ClapLibrary>> open;
        std::lock_guard<std::mutex> lock(mutex);
        if (auto existing = open[path].lock())
            return existing;

        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            if (error)
                *error = std::string("cannot load: ") + dlerror();
            return nullptr;
        }
        auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
        if (!entry || !clap_version_is_compatible(entry->clap_version)) {
            if (error)
                *error = entry ? "incompatible CLAP version" : "no clap_entry symbol";
            dlclose(handle);
            return nullptr;
        }
        if (!entry->init(path.c_str())) {
            if (error)
                *error = "clap_entry.init failed";
            dlclose(handle);
            return nullptr;
        }
        auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory) {
            if (error)
                *error = "no plug-in factory";
            entry->deinit();
            dlclose(handle);
            return nullptr;
        }
        auto lib = std::shared_ptr<ClapLibrary>(new ClapLibrary(handle, entry, factory));
        open[path] = lib;
        return lib;
    }

    ~ClapLibrary() {
        entry_->deinit();
        dlclose(handle_);
    }

    const clap_plugin_factory_t* factory() const { return factory_; }

private:
    ClapLibrary(void* h, const clap_plugin_entry_t* e, const clap_plugin_factory_t* f) : handle_(h), entry_(e), factory_(f) {}
    void* handle_;
    const clap_plugin_entry_t* entry_;
    const clap_plugin_factory_t* factory_;
};

std::vector<ClapPluginInfo> scanClapFile(const std::string& path, std::string* error) {
    noteMainThread();
    std::vector<ClapPluginInfo> result;
    auto lib = ClapLibrary::open(path, error);
    if (!lib)
        return result;
    const auto* f = lib->factory();
    const uint32_t count = f->get_plugin_count(f);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
        if (!d)
            continue;
        result.push_back({path, d->id ? d->id : "", d->name ? d->name : "", d->vendor ? d->vendor : "",
                          d->version ? d->version : ""});
    }
    return result;
}

// ---------------------------------------------------------------- host callbacks

namespace {

ClapProcessor* owner(const clap_host_t* host) { return static_cast<ClapProcessor*>(host->host_data); }

bool hostIsMainThread(const clap_host_t*) { return std::this_thread::get_id() == g_mainThread; }
bool hostIsAudioThread(const clap_host_t*) { return isAudioThread(); }
const clap_host_thread_check_t kThreadCheck = {hostIsMainThread, hostIsAudioThread};

void hostLog(const clap_host_t* host, clap_log_severity severity, const char* msg) { owner(host)->onLog(severity, msg); }
const clap_host_log_t kLog = {hostLog};

void hostParamsRescan(const clap_host_t* host, clap_param_rescan_flags) { owner(host)->onParamsRescan(); }
void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
// The engine flushes idle plug-ins regularly and processes active ones
// every block, so an explicit flush request needs no action.
void hostParamsRequestFlush(const clap_host_t*) {}
const clap_host_params_t kParamsHost = {hostParamsRescan, hostParamsClear, hostParamsRequestFlush};

void hostLatencyChanged(const clap_host_t* host) { owner(host)->onLatencyChanged(); }
const clap_host_latency_t kLatencyHost = {hostLatencyChanged};

void hostTailChanged(const clap_host_t* host) { owner(host)->onTailChanged(); }
const clap_host_tail_t kTailHost = {hostTailChanged};

void hostStateMarkDirty(const clap_host_t*) {}
const clap_host_state_t kStateHost = {hostStateMarkDirty};

bool hostRegisterTimer(const clap_host_t* host, uint32_t periodMs, clap_id* id) { return owner(host)->onRegisterTimer(periodMs, id); }
bool hostUnregisterTimer(const clap_host_t* host, clap_id id) { return owner(host)->onUnregisterTimer(id); }
const clap_host_timer_support_t kTimerSupport = {hostRegisterTimer, hostUnregisterTimer};

const void* hostGetExtension(const clap_host_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kLog;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParamsHost;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatencyHost;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTailHost;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kStateHost;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
    return nullptr;
}
void hostRequestRestart(const clap_host_t* host) { owner(host)->onRequestRestart(); }
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t* host) { owner(host)->onRequestCallback(); }

bool outTryPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

} // namespace

struct ClapProcessor::InputEvents {
    clap_input_events_t list;
    ClapProcessor* self;
    uint32_t count = 0;

    static uint32_t size(const clap_input_events_t* l) { return static_cast<const InputEvents*>(l->ctx)->count; }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
        auto* e = static_cast<const InputEvents*>(l->ctx);
        return i < e->count ? &e->self->events_[i].header : nullptr;
    }
};

void ClapProcessor::onLog(int severity, const char* msg) const {
    // Printing can block; logs from the audio thread are counted, not printed.
    if (isAudioThread()) {
        g_droppedAudioThreadLogs++;
        return;
    }
    std::fprintf(stderr, "[%s] log(%d): %s\n", name_.c_str(), severity, msg ? msg : "");
}

bool ClapProcessor::onRegisterTimer(uint32_t periodMs, clap_id* id) {
    if (!id || std::this_thread::get_id() != g_mainThread)
        return false;
    // Serviced every few milliseconds; faster timers run at that rate.
    const auto period = std::chrono::milliseconds(std::max<uint32_t>(periodMs, 1));
    *id = nextTimerId_++;
    timers_.push_back({*id, period, std::chrono::steady_clock::now() + period});
    return true;
}

bool ClapProcessor::onUnregisterTimer(clap_id id) {
    const auto it = std::find_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.id == id; });
    if (it == timers_.end())
        return false;
    timers_.erase(it);
    return true;
}

// ---------------------------------------------------------------- lifecycle

std::unique_ptr<ClapProcessor> ClapProcessor::load(const std::string& path, const std::string& pluginId, std::string* error) {
    noteMainThread();
    auto lib = ClapLibrary::open(path, error);
    if (!lib)
        return nullptr;
    const auto* f = lib->factory();

    std::string id = pluginId;
    const clap_plugin_descriptor_t* desc = nullptr;
    for (uint32_t i = 0, n = f->get_plugin_count(f); i < n; ++i) {
        const auto* d = f->get_plugin_descriptor(f, i);
        if (d && (id.empty() || id == d->id)) {
            desc = d;
            break;
        }
    }
    if (!desc) {
        if (error)
            *error = "plug-in id not found: " + (id.empty() ? std::string("(none in file)") : id);
        return nullptr;
    }

    std::unique_ptr<ClapProcessor> p(new ClapProcessor());
    p->library_ = lib;
    p->id_ = desc->id;
    p->name_ = desc->name ? desc->name : desc->id;
    p->host_ = {CLAP_VERSION_INIT, p.get(), "Cue", "TwoStorms", "", CUE_VERSION,
                hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback};
    p->plugin_ = f->create_plugin(f, &p->host_, desc->id);
    if (!p->plugin_) {
        if (error)
            *error = "create_plugin failed for " + p->id_;
        return nullptr;
    }
    if (!p->plugin_->init(p->plugin_)) {
        if (error)
            *error = "init failed for " + p->id_;
        p->plugin_->destroy(p->plugin_);
        p->plugin_ = nullptr;
        return nullptr;
    }
    auto ext = [&](const char* name) { return p->plugin_->get_extension(p->plugin_, name); };
    p->ports_ = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
    p->paramsExt_ = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
    p->state_ = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
    p->latency_ = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
    p->tail_ = static_cast<const clap_plugin_tail_t*>(ext(CLAP_EXT_TAIL));
    p->timerExt_ = static_cast<const clap_plugin_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
    if (const auto* notes = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS))) {
        for (uint32_t i = 0, n = notes->count(p->plugin_, true); i < n; ++i) {
            clap_note_port_info_t info{};
            if (notes->get(p->plugin_, i, true, &info) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)) {
                p->midiPort_ = static_cast<int>(i);
                break;
            }
        }
    }

    p->inEvents_ = std::make_unique<InputEvents>();
    p->inEvents_->list = {p->inEvents_.get(), InputEvents::size, InputEvents::get};
    p->inEvents_->self = p.get();
    p->outEvents_ = {nullptr, outTryPush};
    p->events_.resize(1024);
    return p;
}

ClapProcessor::~ClapProcessor() {
    if (!plugin_)
        return;
    deactivate();
    plugin_->destroy(plugin_);
}

void ClapProcessor::queryPorts() {
    inPortChannels_.clear();
    outPortChannels_.clear();
    if (!ports_) {
        // No audio-ports extension: assume one stereo port each way.
        inPortChannels_ = {2};
        outPortChannels_ = {2};
        return;
    }
    for (int dir = 0; dir < 2; ++dir) {
        const bool isInput = dir == 0;
        auto& list = isInput ? inPortChannels_ : outPortChannels_;
        const uint32_t n = ports_->count(plugin_, isInput);
        for (uint32_t i = 0; i < n; ++i) {
            clap_audio_port_info_t info{};
            if (!ports_->get(plugin_, i, isInput, &info))
                continue;
            // The main port goes first.
            if (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
                list.insert(list.begin(), info.channel_count);
            else
                list.push_back(info.channel_count);
        }
    }
}

bool ClapProcessor::activate(double sampleRate, uint32_t maxFrames) {
    if (activated_)
        return true;
    queryPorts();
    maxFrames_ = maxFrames;

    // Scratch: one buffer per channel of every port, plus a mono pair.
    uint32_t channels = 2;
    for (uint32_t c : inPortChannels_) channels += c;
    for (uint32_t c : outPortChannels_) channels += c;
    scratch_.assign(static_cast<size_t>(channels) * maxFrames, 0.0f);
    float* next = scratch_.data();
    auto take = [&] {
        float* p = next;
        next += maxFrames;
        return p;
    };

    auto build = [&](const std::vector<uint32_t>& ports, std::vector<clap_audio_buffer_t>& bufs, std::vector<float*>& ptrs) {
        size_t total = 0;
        for (uint32_t c : ports) total += c;
        ptrs.assign(total, nullptr);
        bufs.assign(ports.size(), clap_audio_buffer_t{});
        size_t offset = 0;
        for (size_t p = 0; p < ports.size(); ++p) {
            for (uint32_t c = 0; c < ports[p]; ++c)
                ptrs[offset + c] = take();
            bufs[p].data32 = ptrs.data() + offset;
            bufs[p].channel_count = ports[p];
            offset += ports[p];
        }
    };
    build(inPortChannels_, inBuffers_, inPtrs_);
    build(outPortChannels_, outBuffers_, outPtrs_);
    take(); // mono adaptation buffer lives at the end
    take();

    if (!plugin_->activate(plugin_, sampleRate, 1, maxFrames))
        return false;
    activated_ = true;
    cachedLatency_ = latency_ ? latency_->get(plugin_) : 0;
    cachedTail_ = tail_ ? tail_->get(plugin_) : 0;
    return true;
}

void ClapProcessor::deactivate() {
    if (!activated_)
        return;
    plugin_->deactivate(plugin_);
    activated_ = false;
}

void ClapProcessor::pumpMainThread() {
    if (callbackRequested_.exchange(false))
        plugin_->on_main_thread(plugin_);
    if (timerExt_ && !timers_.empty()) {
        // A timer callback may register or unregister timers: collect first.
        const auto now = std::chrono::steady_clock::now();
        std::vector<clap_id> due;
        for (auto& t : timers_)
            if (now >= t.due) {
                due.push_back(t.id);
                t.due = std::max(t.due + t.period, now);
            }
        for (clap_id id : due)
            timerExt_->on_timer(plugin_, id);
    }
    if (activated_ && latencyDirty_.exchange(false))
        cachedLatency_ = latency_ ? latency_->get(plugin_) : 0;
    if (tailDirty_.exchange(false))
        cachedTail_ = tail_ ? tail_->get(plugin_) : 0;
    if (restartRequested_.exchange(false))
        std::fprintf(stderr, "[%s] asked for a restart; not supported while loaded yet\n", name_.c_str());
    rescanRequested_ = false;
}

std::vector<ParamInfo> ClapProcessor::params() const {
    std::vector<ParamInfo> out;
    if (!paramsExt_)
        return out;
    for (uint32_t i = 0, n = paramsExt_->count(plugin_); i < n; ++i) {
        clap_param_info_t info{};
        if (!paramsExt_->get_info(plugin_, i, &info))
            continue;
        const bool stepped = info.flags & CLAP_PARAM_IS_STEPPED;
        out.push_back({info.id, info.name, info.module, info.min_value, info.max_value, info.default_value,
                       stepped ? static_cast<int>(info.max_value - info.min_value) : 0});
    }
    return out;
}

bool ClapProcessor::paramValue(uint32_t id, double& value) const { return paramsExt_ && paramsExt_->get_value(plugin_, id, &value); }

std::string ClapProcessor::paramText(uint32_t id, double value) const {
    char buf[256] = {};
    if (!paramsExt_ || !paramsExt_->value_to_text(plugin_, id, value, buf, sizeof buf))
        return {};
    buf[sizeof buf - 1] = 0;
    return buf;
}

bool ClapProcessor::saveState(std::vector<uint8_t>& out) const {
    if (!state_)
        return false;
    out.clear();
    clap_ostream_t stream{&out, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                              auto* v = static_cast<std::vector<uint8_t>*>(s->ctx);
                              const auto* b = static_cast<const uint8_t*>(buf);
                              v->insert(v->end(), b, b + size);
                              return static_cast<int64_t>(size);
                          }};
    return state_->save(plugin_, &stream);
}

bool ClapProcessor::loadState(const std::vector<uint8_t>& data) {
    if (!state_)
        return false;
    struct Reader {
        const std::vector<uint8_t>* data;
        size_t pos = 0;
    } reader{&data};
    clap_istream_t stream{&reader, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                              auto* r = static_cast<Reader*>(s->ctx);
                              const size_t n = std::min<size_t>(size, r->data->size() - r->pos);
                              std::memcpy(buf, r->data->data() + r->pos, n);
                              r->pos += n;
                              return static_cast<int64_t>(n);
                          }};
    return state_->load(plugin_, &stream);
}

bool ClapProcessor::setParam(uint32_t id, double value) {
    if (!paramQueue_.push({id, value}))
        return false;
    paramsPending_.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------- audio thread

void ClapProcessor::drainParams(uint32_t maxEvents) {
    uint32_t n = 0;
    paramsPending_.store(false, std::memory_order_relaxed);
    while (n < maxEvents) {
        auto change = paramQueue_.pop();
        if (!change)
            break;
        clap_event_param_value_t& ev = events_[n++].param;
        ev = {};
        ev.header.size = sizeof ev;
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = change->id;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = change->value;
    }
    if (n == maxEvents)
        paramsPending_.store(true, std::memory_order_relaxed); // more next time
    inEvents_->count = n;
}

void ClapProcessor::startProcessing() {
    if (activated_ && !processing_)
        processing_ = plugin_->start_processing(plugin_);
}

void ClapProcessor::stopProcessing() {
    if (processing_) {
        plugin_->stop_processing(plugin_);
        processing_ = false;
    }
}

void ClapProcessor::reset() {
    if (activated_)
        plugin_->reset(plugin_);
}

void ClapProcessor::flushIdle() {
    if (!paramsPending_.load(std::memory_order_acquire) || processing_ || !paramsExt_ || !activated_)
        return;
    drainParams(static_cast<uint32_t>(events_.size()));
    if (inEvents_->count > 0)
        paramsExt_->flush(plugin_, &inEvents_->list, &outEvents_);
    inEvents_->count = 0;
}

void ClapProcessor::process(const float* const* in, float* const* out, uint32_t frames) {
    if (!processing_ || outBuffers_.empty()) {
        for (int c = 0; c < kChainChannels; ++c)
            std::memcpy(out[c], in[c], frames * sizeof(float));
        return;
    }
    if (paramsPending_.load(std::memory_order_acquire))
        drainParams(static_cast<uint32_t>(events_.size() / 2));
    else
        inEvents_->count = 0;
    if (midiPending_.load(std::memory_order_acquire))
        drainMidi();

    float* monoIn = scratch_.data() + scratch_.size() - 2 * static_cast<size_t>(maxFrames_);
    float* monoOut = monoIn + maxFrames_;

    // Main input port.
    if (!inBuffers_.empty()) {
        const uint32_t ch = inPortChannels_[0];
        if (ch == 1) {
            for (uint32_t n = 0; n < frames; ++n)
                monoIn[n] = 0.5f * (in[0][n] + in[1][n]);
            inBuffers_[0].data32[0] = monoIn;
        } else {
            for (uint32_t c = 0; c < ch; ++c)
                inBuffers_[0].data32[c] = const_cast<float*>(in[std::min<uint32_t>(c, kChainChannels - 1)]);
        }
        // The second port is the side-chain: the key signal if the chain
        // feeds one. Other spare inputs get silence.
        for (size_t p = 1; p < inBuffers_.size(); ++p)
            for (uint32_t c = 0; c < inPortChannels_[p]; ++c) {
                if (p == 1 && key_[0])
                    std::memcpy(inBuffers_[p].data32[c], key_[std::min<uint32_t>(c, kChainChannels - 1)], frames * sizeof(float));
                else
                    std::memset(inBuffers_[p].data32[c], 0, frames * sizeof(float));
            }
    }
    // Main output port.
    const uint32_t outCh = outPortChannels_[0];
    if (outCh == 1)
        outBuffers_[0].data32[0] = monoOut;
    else
        for (uint32_t c = 0; c < std::min<uint32_t>(outCh, kChainChannels); ++c)
            outBuffers_[0].data32[c] = out[c];

    clap_process_t proc{};
    proc.steady_time = steadyTime_;
    proc.frames_count = frames;
    proc.transport = nullptr;
    proc.audio_inputs = inBuffers_.data();
    proc.audio_outputs = outBuffers_.data();
    proc.audio_inputs_count = static_cast<uint32_t>(inBuffers_.size());
    proc.audio_outputs_count = static_cast<uint32_t>(outBuffers_.size());
    proc.in_events = &inEvents_->list;
    proc.out_events = &outEvents_;
    const clap_process_status status = plugin_->process(plugin_, &proc);
    steadyTime_ += frames;
    inEvents_->count = 0;

    if (status == CLAP_PROCESS_ERROR) {
        for (int c = 0; c < kChainChannels; ++c)
            std::memset(out[c], 0, frames * sizeof(float));
        return;
    }
    if (outCh == 1) {
        std::memcpy(out[0], monoOut, frames * sizeof(float));
        std::memcpy(out[1], monoOut, frames * sizeof(float));
    } else if (outCh == 0) {
        for (int c = 0; c < kChainChannels; ++c)
            std::memset(out[c], 0, frames * sizeof(float));
    }
}

void ClapProcessor::sendMidi(uint8_t status, uint8_t data1, uint8_t data2) {
    if (midiPort_ < 0 || status < 0x80)
        return;
    const uint8_t type = status & 0xF0, ch = status & 0x0F;
    if (type == 0xB0) {
        const size_t i = ch * 128u + (data1 & 0x7F);
        ccValue_[i].store(data2 & 0x7F, std::memory_order_relaxed);
        ccDirty_[i / 64].fetch_or(uint64_t{1} << (i % 64), std::memory_order_release);
    } else if (type == 0xE0) {
        bendValue_[ch].store(static_cast<uint16_t>((data2 & 0x7F) << 7 | (data1 & 0x7F)), std::memory_order_relaxed);
        bendDirty_.fetch_or(1u << ch, std::memory_order_release);
    } else if (!midiQueue_.push({{status, data1, data2}})) {
        return; // the plug-in's chain is idle and the queue is full: drop
    }
    midiPending_.store(true, std::memory_order_release);
}

void ClapProcessor::pushMidiEvent(uint8_t s, uint8_t d1, uint8_t d2) {
    clap_event_midi_t& ev = events_[inEvents_->count++].midi;
    ev = {};
    ev.header.size = sizeof ev;
    ev.header.time = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_MIDI;
    ev.header.flags = 0;
    ev.port_index = static_cast<uint16_t>(midiPort_);
    ev.data[0] = s;
    ev.data[1] = d1;
    ev.data[2] = d2;
}

void ClapProcessor::drainMidi() {
    midiPending_.store(false, std::memory_order_relaxed);
    const uint32_t capacity = static_cast<uint32_t>(events_.size());
    bool more = false;
    for (size_t w = 0; w < 16 * 128 / 64; ++w) {
        uint64_t bits = ccDirty_[w].exchange(0, std::memory_order_acquire);
        while (bits) {
            const int b = __builtin_ctzll(bits);
            bits &= bits - 1;
            const size_t i = w * 64 + b;
            if (inEvents_->count >= capacity) {
                ccDirty_[w].fetch_or(uint64_t{1} << b, std::memory_order_relaxed);
                more = true;
                continue;
            }
            pushMidiEvent(static_cast<uint8_t>(0xB0 | (i / 128)), static_cast<uint8_t>(i % 128),
                          ccValue_[i].load(std::memory_order_relaxed));
        }
    }
    uint32_t bends = bendDirty_.exchange(0, std::memory_order_acquire);
    while (bends) {
        const int ch = __builtin_ctz(bends);
        bends &= bends - 1;
        if (inEvents_->count >= capacity) {
            bendDirty_.fetch_or(1u << ch, std::memory_order_relaxed);
            more = true;
            continue;
        }
        const uint16_t v = bendValue_[ch].load(std::memory_order_relaxed);
        pushMidiEvent(static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(v & 0x7F), static_cast<uint8_t>(v >> 7));
    }
    while (inEvents_->count < capacity) {
        auto m = midiQueue_.pop();
        if (!m)
            break;
        pushMidiEvent(m->data[0], m->data[1], m->data[2]);
    }
    if (inEvents_->count >= capacity)
        more = true; // the queue may still hold messages
    if (more)
        midiPending_.store(true, std::memory_order_relaxed);
}

bool ClapProcessor::hasSidechain() const {
    if (!ports_ || ports_->count(plugin_, true) < 2)
        return false;
    // The port after the main one.
    for (uint32_t i = 0, n = ports_->count(plugin_, true); i < n; ++i) {
        clap_audio_port_info_t info{};
        if (ports_->get(plugin_, i, true, &info) && !(info.flags & CLAP_AUDIO_PORT_IS_MAIN) && info.channel_count > 0)
            return true;
    }
    return false;
}

void ClapProcessor::setSidechain(const float* const* key) {
    for (int c = 0; c < kChainChannels; ++c)
        key_[c] = key ? key[c] : nullptr;
}

uint32_t ClapProcessor::latencySamples() const { return cachedLatency_.load(std::memory_order_relaxed); }

uint32_t ClapProcessor::tailSamples() const {
    const uint32_t t = cachedTail_.load(std::memory_order_relaxed);
    return t >= static_cast<uint32_t>(INT32_MAX) ? kInfiniteTail : t;
}

} // namespace cue
