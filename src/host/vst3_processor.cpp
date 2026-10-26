#include "vst3_processor.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>

using namespace Steinberg;
using namespace Steinberg::Vst;

// The editor interfaces' ids: the SDK leaves these to whoever uses them, and
// the VST3 files Cue builds define only the audio ones.
namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
namespace Linux {
DEF_CLASS_IID(IEventHandler)
DEF_CLASS_IID(ITimerHandler)
DEF_CLASS_IID(IRunLoop)
} // namespace Linux
} // namespace Steinberg

namespace cue {

namespace {

// One host context for all plug-ins, as the SDK's PlugProvider expects.
void ensureHostContext() {
    static std::once_flag once;
    static IPtr<HostApplication> app;
    std::call_once(once, [] {
        app = owned(new HostApplication());
        PluginContextFactory::instance().setPluginContext(app);
    });
}

// Modules are shared per path, like CLAP libraries.
VST3::Hosting::Module::Ptr openModule(const std::string& path, std::string* error) {
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<VST3::Hosting::Module>> open;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto m = open[path].lock())
        return m;
    std::string err;
    auto m = VST3::Hosting::Module::create(path, err);
    if (!m) {
        if (error)
            *error = "cannot load: " + err;
        return nullptr;
    }
    open[path] = m;
    return m;
}

class ComponentHandler : public U::Implements<U::Directly<IComponentHandler>> {
public:
    std::atomic<int32> restartFlags{0};
    std::function<void(ParamID, ParamValue)> onEdit;

    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        if (onEdit)
            onEdit(id, value);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restartFlags.fetch_or(flags);
        return kResultOk;
    }
};

uint32_t vstTail(uint32 t) { return t == kInfiniteTail ? cue::kInfiniteTail : t; }

// The frame an editor sits in: what the plug-in talks to about its size, and
// the run loop it needs on Linux, where there is no run loop of its own.
// Everything here happens on the main thread, with the editor's window.
class PlugFrame : public U::Implements<U::Directly<IPlugFrame, Linux::IRunLoop>> {
public:
    struct Handler {
        IPtr<Linux::IEventHandler> handler;
        int fd = -1;
    };
    struct Timer {
        IPtr<Linux::ITimerHandler> handler;
        std::chrono::milliseconds period{16};
        std::chrono::steady_clock::time_point due;
    };

    std::vector<Handler> handlers;
    std::vector<Timer> timers;
    bool resizeAsked = false;
    cue::EditorSize asked;

    tresult PLUGIN_API resizeView(IPlugView*, ViewRect* size) override {
        if (!size)
            return kInvalidArgument;
        // Cue resizes the window at its next turn; the plug-in is told the
        // new size when the window reports it.
        asked = {static_cast<uint32_t>(size->getWidth()), static_cast<uint32_t>(size->getHeight()), true};
        resizeAsked = true;
        return kResultTrue;
    }

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h, Linux::FileDescriptor fd) override {
        if (!h || fd < 0)
            return kInvalidArgument;
        handlers.push_back({IPtr<Linux::IEventHandler>(h), fd});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override {
        const auto it = std::remove_if(handlers.begin(), handlers.end(), [&](const Handler& e) { return e.handler.get() == h; });
        if (it == handlers.end())
            return kResultFalse;
        handlers.erase(it, handlers.end());
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval ms) override {
        if (!h)
            return kInvalidArgument;
        const auto period = std::chrono::milliseconds(std::max<Linux::TimerInterval>(ms, 1));
        timers.push_back({IPtr<Linux::ITimerHandler>(h), period, std::chrono::steady_clock::now() + period});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override {
        const auto it = std::remove_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.handler.get() == h; });
        if (it == timers.end())
            return kResultFalse;
        timers.erase(it, timers.end());
        return kResultTrue;
    }

    // Called from the engine's loop: what is due, and what is readable.
    void runTimers() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<IPtr<Linux::ITimerHandler>> due;
        for (Timer& t : timers)
            if (now >= t.due) {
                due.push_back(t.handler);
                t.due = std::max(t.due + t.period, now);
            }
        for (auto& h : due)
            h->onTimer();
    }
    void runFd(int fd) {
        // A copy: a handler may register or unregister while it runs.
        std::vector<IPtr<Linux::IEventHandler>> ready;
        for (const Handler& h : handlers)
            if (h.fd == fd)
                ready.push_back(h.handler);
        for (auto& h : ready)
            h->onFDIsSet(fd);
    }
};

} // namespace

struct Vst3Processor::Impl {
    struct ParamChange {
        ParamID id;
        ParamValue value;
    };

    VST3::Hosting::Module::Ptr module;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    IPtr<ComponentHandler> handler;

    HostProcessData data;
    ParameterChanges inParams{64}, outParams{64};
    EventList inEvents{128}, outEvents{128};
    ProcessContext context{};

    CommandQueue<ParamChange, 512> audioQueue;
    CommandQueue<ParamChange, 512> controllerQueue;

    // The editor, main thread only.
    IPtr<IPlugView> view;
    IPtr<PlugFrame> frame;

    int32 inChannels = 0, outChannels = 0;
    bool active = false, processing = false;
    uint32_t maxFrames = 0;
    std::vector<float> scratch; // mono adaptation and aux buses
    float* monoIn = nullptr;
    float* monoOut = nullptr;
};

Vst3Processor::Vst3Processor() : impl_(std::make_unique<Impl>()) {}

std::vector<Vst3PluginInfo> scanVst3(const std::string& path, std::string* error) {
    ensureHostContext();
    std::vector<Vst3PluginInfo> out;
    auto m = openModule(path, error);
    if (!m)
        return out;
    for (const auto& ci : m->getFactory().classInfos())
        if (ci.category() == kVstAudioEffectClass)
            out.push_back({path, ci.name(), ci.vendor(), ci.version(), ci.subCategoriesString(), ci.ID().toString()});
    return out;
}

namespace {
// "{ABCD...}" or "abcd..." to upper-case hex without braces or dashes.
std::string normalUid(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (c != '{' && c != '}' && c != '-')
            return {};
    return out.size() == 32 ? out : std::string();
}
} // namespace

std::unique_ptr<Vst3Processor> Vst3Processor::load(const std::string& path, const std::string& which, std::string* error) {
    ensureHostContext();
    auto module = openModule(path, error);
    if (!module)
        return nullptr;
    const auto& factory = module->getFactory();
    std::unique_ptr<Vst3Processor> p(new Vst3Processor());
    Impl& d = *p->impl_;
    std::vector<VST3::Hosting::ClassInfo> effects;
    for (const auto& ci : factory.classInfos())
        if (ci.category() == kVstAudioEffectClass)
            effects.push_back(ci);
    const std::string uid = normalUid(which);
    const VST3::Hosting::ClassInfo* chosen = nullptr;
    for (const auto& ci : effects)
        if (which.empty() || ci.name() == which || (!uid.empty() && normalUid(ci.ID().toString()) == uid)) {
            chosen = &ci;
            break;
        }
    // REAPER's names can differ from the class name ("Parallax (mono)"); a
    // module with one effect has only one candidate.
    if (!chosen && effects.size() == 1)
        chosen = &effects[0];
    if (!chosen) {
        if (error)
            *error = effects.empty() ? "no audio effect in module" : "no audio effect named '" + which + "' in module";
        return nullptr;
    }
    d.provider = owned(new PlugProvider(factory, *chosen, true));
    if (!d.provider->initialize()) {
        if (error)
            *error = "could not initialise " + chosen->name();
        return nullptr;
    }
    p->name_ = chosen->name();
    d.module = module;
    d.component = d.provider->getComponentPtr();
    d.controller = d.provider->getControllerPtr();
    d.processor = U::cast<IAudioProcessor>(d.component);
    if (!d.component || !d.processor) {
        if (error)
            *error = p->name_ + " has no audio processor";
        return nullptr;
    }
    if (d.controller) {
        d.handler = owned(new ComponentHandler());
        Impl* impl = p->impl_.get();
        // Edits from the plug-in's own UI reach the audio side too.
        d.handler->onEdit = [impl](ParamID id, ParamValue v) { impl->audioQueue.push({id, v}); };
        d.controller->setComponentHandler(d.handler);
    }
    return p;
}

Vst3Processor::~Vst3Processor() {
    closeEditor();
    deactivate();
    if (impl_->controller)
        impl_->controller->setComponentHandler(nullptr);
    impl_->processor = nullptr;
    impl_->controller = nullptr;
    impl_->component = nullptr;
    impl_->provider = nullptr; // terminates and disconnects the plug-in
}

bool Vst3Processor::activate(double sampleRate, uint32_t maxFrames) {
    Impl& d = *impl_;
    if (d.active)
        return true;
    d.maxFrames = maxFrames;
    ProcessSetup setup{kRealtime, kSample32, static_cast<int32>(maxFrames), sampleRate};
    if (d.processor->setupProcessing(setup) != kResultOk) {
        std::fprintf(stderr, "[%s] setupProcessing refused\n", name_.c_str());
        return false;
    }

    // Ask for stereo in and out on the main buses; accept mono if refused.
    const int32 nIn = d.component->getBusCount(kAudio, kInput);
    const int32 nOut = d.component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> inArr(nIn), outArr(nOut);
    for (int32 i = 0; i < nIn; ++i)
        d.processor->getBusArrangement(kInput, i, inArr[i]);
    for (int32 i = 0; i < nOut; ++i)
        d.processor->getBusArrangement(kOutput, i, outArr[i]);
    for (SpeakerArrangement in : {SpeakerArr::kStereo, SpeakerArr::kMono}) {
        if (nIn > 0)
            inArr[0] = in;
        if (nOut > 0)
            outArr[0] = SpeakerArr::kStereo;
        if (d.processor->setBusArrangements(inArr.data(), nIn, outArr.data(), nOut) == kResultTrue)
            break;
    }
    for (int32 i = 0; i < nIn; ++i)
        d.component->activateBus(kAudio, kInput, i, i == 0);
    for (int32 i = 0; i < nOut; ++i)
        d.component->activateBus(kAudio, kOutput, i, i == 0);

    // Buffer containers only; every channel points at our own memory.
    d.data.prepare(*d.component, 0, kSample32);
    size_t channels = 2;
    for (int32 b = 0; b < d.data.numInputs; ++b) channels += d.data.inputs[b].numChannels;
    for (int32 b = 0; b < d.data.numOutputs; ++b) channels += d.data.outputs[b].numChannels;
    d.scratch.assign(channels * maxFrames, 0.0f);
    float* next = d.scratch.data();
    d.monoIn = next; next += maxFrames;
    d.monoOut = next; next += maxFrames;
    for (int32 b = 0; b < d.data.numInputs; ++b)
        for (int32 c = 0; c < d.data.inputs[b].numChannels; ++c, next += maxFrames)
            d.data.inputs[b].channelBuffers32[c] = next;
    for (int32 b = 0; b < d.data.numOutputs; ++b)
        for (int32 c = 0; c < d.data.outputs[b].numChannels; ++c, next += maxFrames)
            d.data.outputs[b].channelBuffers32[c] = next;
    d.inChannels = d.data.numInputs > 0 ? d.data.inputs[0].numChannels : 0;
    d.outChannels = d.data.numOutputs > 0 ? d.data.outputs[0].numChannels : 0;

    d.data.inputParameterChanges = &d.inParams;
    d.data.outputParameterChanges = &d.outParams;
    d.data.inputEvents = &d.inEvents;
    d.data.outputEvents = &d.outEvents;
    d.data.processContext = &d.context;
    d.data.processMode = kRealtime;
    d.data.symbolicSampleSize = kSample32;
    if (d.controller)
        d.inParams.setMaxParameters(std::max<int32>(64, d.controller->getParameterCount()));
    d.context.sampleRate = sampleRate;

    if (d.component->setActive(true) != kResultOk) {
        std::fprintf(stderr, "[%s] setActive refused\n", name_.c_str());
        return false;
    }
    d.active = true;
    // Processing stays on from here until deactivate(): with yabridge every
    // setProcessing() is a round trip to the Wine process, far too slow for
    // the audio thread at a chain switch.
    d.processor->setProcessing(true);
    d.processing = true;
    latency_ = d.processor->getLatencySamples();
    tail_ = vstTail(d.processor->getTailSamples());
    return true;
}

void Vst3Processor::deactivate() {
    Impl& d = *impl_;
    if (!d.active)
        return;
    if (d.processing) {
        d.processor->setProcessing(false);
        d.processing = false;
    }
    d.component->setActive(false);
    d.data.unprepare();
    d.active = false;
}

bool Vst3Processor::loadState(const std::vector<uint8_t>& component, const std::vector<uint8_t>& controller) {
    Impl& d = *impl_;
    if (component.empty())
        return false;
    auto stream = owned(new MemoryStream(const_cast<uint8_t*>(component.data()), static_cast<TSize>(component.size())));
    if (d.component->setState(stream) != kResultOk)
        return false;
    if (d.controller) {
        int64 pos = 0;
        stream->seek(0, IBStream::kIBSeekSet, &pos);
        d.controller->setComponentState(stream);
        if (!controller.empty()) {
            auto cs = owned(new MemoryStream(const_cast<uint8_t*>(controller.data()), static_cast<TSize>(controller.size())));
            d.controller->setState(cs);
        }
    }
    return true;
}

bool Vst3Processor::saveState(std::vector<uint8_t>& component, std::vector<uint8_t>& controller) const {
    Impl& d = *impl_;
    auto copyOut = [](MemoryStream* s, std::vector<uint8_t>& out) {
        out.assign(s->getData(), s->getData() + s->getSize());
    };
    auto cs = owned(new MemoryStream());
    if (d.component->getState(cs) != kResultOk)
        return false;
    copyOut(cs, component);
    controller.clear();
    if (d.controller) {
        auto ks = owned(new MemoryStream());
        if (d.controller->getState(ks) == kResultOk)
            copyOut(ks, controller);
    }
    return true;
}

// ---------------------------------------------------------------- the editor

bool Vst3Processor::hasEditor() const { return impl_->controller != nullptr; }

bool Vst3Processor::editorOpen() const { return impl_->view != nullptr; }

bool Vst3Processor::openEditor(unsigned long window, EditorSize* size, std::string* error) {
    Impl& d = *impl_;
    if (d.view)
        return true;
    if (!d.controller) {
        *error = name_ + " has no editor of its own";
        return false;
    }
    IPtr<IPlugView> view = owned(d.controller->createView(ViewType::kEditor));
    if (!view) {
        *error = name_ + " has no editor of its own";
        return false;
    }
    if (view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        *error = name_ + " cannot put its editor in an X11 window (a Windows plug-in needs yabridge)";
        return false;
    }
    if (!d.frame)
        d.frame = owned(new PlugFrame());
    d.frame->resizeAsked = false;
    view->setFrame(d.frame);
    // Attached first: a bridged plug-in only knows its size once its window
    // is there.
    if (view->attached(reinterpret_cast<void*>(static_cast<uintptr_t>(window)), kPlatformTypeX11EmbedWindowID) != kResultOk) {
        view->setFrame(nullptr);
        *error = name_ + " would not put its editor in Cue's window";
        return false;
    }
    ViewRect rect{};
    if (view->getSize(&rect) != kResultOk)
        rect = ViewRect(0, 0, 800, 600);
    d.view = view;
    *size = {static_cast<uint32_t>(rect.getWidth()), static_cast<uint32_t>(rect.getHeight()), view->canResize() == kResultTrue};
    return true;
}

void Vst3Processor::closeEditor() {
    Impl& d = *impl_;
    if (!d.view)
        return;
    d.view->removed();
    d.view->setFrame(nullptr);
    d.view = nullptr;
    // A plug-in that forgot to give them back does not keep the loop busy.
    if (d.frame) {
        d.frame->handlers.clear();
        d.frame->timers.clear();
        d.frame->resizeAsked = false;
    }
}

bool Vst3Processor::editorResized(uint32_t width, uint32_t height) {
    Impl& d = *impl_;
    if (!d.view || !width || !height)
        return false;
    ViewRect rect(0, 0, static_cast<int32>(width), static_cast<int32>(height));
    return d.view->onSize(&rect) == kResultOk;
}

bool Vst3Processor::takeEditorResize(EditorSize* size) {
    Impl& d = *impl_;
    if (!d.frame || !d.frame->resizeAsked)
        return false;
    d.frame->resizeAsked = false;
    *size = d.frame->asked;
    return true;
}

void Vst3Processor::editorFds(std::vector<int>& out) const {
    if (!impl_->frame)
        return;
    for (const PlugFrame::Handler& h : impl_->frame->handlers)
        if (std::find(out.begin(), out.end(), h.fd) == out.end())
            out.push_back(h.fd);
}

void Vst3Processor::onEditorFd(int fd) {
    if (impl_->frame && impl_->view)
        impl_->frame->runFd(fd);
}

void Vst3Processor::pumpMainThread() {
    Impl& d = *impl_;
    while (auto c = d.controllerQueue.pop())
        if (d.controller)
            d.controller->setParamNormalized(c->id, c->value);
    if (d.handler) {
        const int32 flags = d.handler->restartFlags.exchange(0);
        if (flags & kLatencyChanged)
            latency_ = d.processor->getLatencySamples();
        if (flags & kReloadComponent)
            std::fprintf(stderr, "[%s] asked to be reloaded; not supported while loaded yet\n", name_.c_str());
    }
    // The editor's timers: what a plug-in redraws itself with on Linux.
    if (d.view && d.frame)
        d.frame->runTimers();
}

bool Vst3Processor::setParam(uint32_t id, double normalized) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    const bool ok = impl_->audioQueue.push({id, normalized});
    impl_->controllerQueue.push({id, normalized});
    return ok;
}

std::vector<ParamInfo> Vst3Processor::params() const {
    std::vector<ParamInfo> out;
    const Impl& d = *impl_;
    if (!d.controller)
        return out;
    for (int32 i = 0, n = d.controller->getParameterCount(); i < n; ++i) {
        ParameterInfo info{};
        if (d.controller->getParameterInfo(i, info) != kResultOk || (info.flags & ParameterInfo::kIsReadOnly))
            continue;
        out.push_back({info.id, Steinberg::Vst::StringConvert::convert(info.title), "", 0.0, 1.0, info.defaultNormalizedValue, info.stepCount});
    }
    return out;
}

bool Vst3Processor::paramValue(uint32_t id, double& normalized) const {
    if (!impl_->controller)
        return false;
    normalized = impl_->controller->getParamNormalized(id);
    return true;
}

std::string Vst3Processor::paramText(uint32_t id, double normalized) const {
    String128 text{};
    if (!impl_->controller || impl_->controller->getParamStringByValue(id, normalized, text) != kResultOk)
        return {};
    return Steinberg::Vst::StringConvert::convert(text);
}

// Processing is on for as long as the plug-in is active (see activate()), so
// starting and stopping a chain makes no calls into it.
void Vst3Processor::startProcessing() {}

void Vst3Processor::stopProcessing() {}

void Vst3Processor::reset() {
    // Toggling processing is the only reset VST3 offers. The engine calls
    // this on its helper thread while the chain is idle, never at a switch.
    Impl& d = *impl_;
    if (d.processing) {
        d.processor->setProcessing(false);
        d.processor->setProcessing(true);
    }
}

void Vst3Processor::process(const float* const* in, float* const* out, uint32_t frames) {
    Impl& d = *impl_;
    if (!d.processing || d.outChannels == 0) {
        for (int c = 0; c < kChainChannels; ++c)
            std::memcpy(out[c], in[c], frames * sizeof(float));
        return;
    }

    d.inParams.clearQueue();
    while (auto c = d.audioQueue.pop()) {
        int32 index = 0;
        if (IParamValueQueue* q = d.inParams.addParameterData(c->id, index)) {
            int32 point = 0;
            q->addPoint(0, c->value, point);
        }
    }
    d.outParams.clearQueue();
    d.outEvents.clear();

    if (d.inChannels == 1) {
        for (uint32_t n = 0; n < frames; ++n)
            d.monoIn[n] = 0.5f * (in[0][n] + in[1][n]);
        d.data.inputs[0].channelBuffers32[0] = d.monoIn;
    } else if (d.inChannels >= 2) {
        d.data.inputs[0].channelBuffers32[0] = const_cast<float*>(in[0]);
        d.data.inputs[0].channelBuffers32[1] = const_cast<float*>(in[1]);
    }
    if (d.outChannels == 1) {
        d.data.outputs[0].channelBuffers32[0] = d.monoOut;
    } else {
        d.data.outputs[0].channelBuffers32[0] = out[0];
        d.data.outputs[0].channelBuffers32[1] = out[1];
    }
    for (int32 b = 0; b < d.data.numInputs; ++b)
        d.data.inputs[b].silenceFlags = 0;
    d.data.numSamples = static_cast<int32>(frames);

    d.processor->process(d.data);
    d.context.projectTimeSamples += frames;
    d.context.continousTimeSamples += frames;

    if (d.outChannels == 1) {
        std::memcpy(out[0], d.monoOut, frames * sizeof(float));
        std::memcpy(out[1], d.monoOut, frames * sizeof(float));
    }
}

} // namespace cue
