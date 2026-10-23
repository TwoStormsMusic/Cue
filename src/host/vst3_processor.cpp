#include "vst3_processor.h"

#include "pluginterfaces/base/funknownimpl.h"
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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace sh {

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

uint32_t vstTail(uint32 t) { return t == kInfiniteTail ? sh::kInfiniteTail : t; }

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

std::unique_ptr<Vst3Processor> Vst3Processor::load(const std::string& path, const std::string& name, std::string* error) {
    ensureHostContext();
    auto module = openModule(path, error);
    if (!module)
        return nullptr;
    const auto& factory = module->getFactory();
    std::unique_ptr<Vst3Processor> p(new Vst3Processor());
    Impl& d = *p->impl_;
    for (const auto& ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass || (!name.empty() && ci.name() != name))
            continue;
        d.provider = owned(new PlugProvider(factory, ci, true));
        if (!d.provider->initialize()) {
            if (error)
                *error = "could not initialise " + ci.name();
            return nullptr;
        }
        p->name_ = ci.name();
        break;
    }
    if (!d.provider) {
        if (error)
            *error = name.empty() ? "no audio effect in module" : "no audio effect named '" + name + "'";
        return nullptr;
    }
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
    latency_ = d.processor->getLatencySamples();
    tail_ = vstTail(d.processor->getTailSamples());
    return true;
}

void Vst3Processor::deactivate() {
    Impl& d = *impl_;
    if (!d.active)
        return;
    if (d.processing)
        stopProcessing();
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
}

bool Vst3Processor::setParam(uint32_t id, double normalized) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    const bool ok = impl_->audioQueue.push({id, normalized});
    impl_->controllerQueue.push({id, normalized});
    return ok;
}

void Vst3Processor::startProcessing() {
    Impl& d = *impl_;
    if (d.active && !d.processing) {
        d.processor->setProcessing(true);
        d.processing = true;
    }
}

void Vst3Processor::stopProcessing() {
    Impl& d = *impl_;
    if (d.processing) {
        d.processor->setProcessing(false);
        d.processing = false;
    }
}

void Vst3Processor::reset() {
    // Toggling processing is the only reset VST3 offers on the audio thread.
    // A scene coming out of idle is started right after this, which does
    // the setProcessing(true) half.
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

} // namespace sh
