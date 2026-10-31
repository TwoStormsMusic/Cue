// A test VST3 plug-in for the host's own tests, written against the VST3
// interfaces only: a stereo feedback delay (250 ms, feedback 0.5) with one
// parameter, "Mix" (normalised, default 0.5).
//
// Like a well-behaved plug-in it clears its delay line when processing is
// switched on, and it records how the host drives it in counters exported
// as cue_vst3_test_stat().

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

std::atomic<long> g_setProcessingOn{0}, g_setProcessingOff{0}, g_processWhileOff{0}, g_instances{0};

const TUID kClassId = INLINE_UID(0x5a7b1c2d, 0x3e4f5061, 0x72839405, 0xa6b7c8d9);
constexpr double kDelayMs = 250.0;
constexpr float kFeedback = 0.5f;

class TestDelay : public IComponent, public IAudioProcessor, public IEditController {
public:
    TestDelay() { g_instances++; }
    virtual ~TestDelay() { g_instances--; }

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
            FUnknownPrivate::iidEqual(iid, IComponent::iid)) {
            *obj = static_cast<IComponent*>(this);
        } else if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid)) {
            *obj = static_cast<IAudioProcessor*>(this);
        } else if (FUnknownPrivate::iidEqual(iid, IEditController::iid)) {
            *obj = static_cast<IEditController*>(this);
        } else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0)
            delete this;
        return r;
    }

    // IPluginBase (shared by component and controller)
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0)
            return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = 2;
        const char16_t name[] = u"Main";
        std::memcpy(bus.name, name, sizeof name);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            const size_t len = static_cast<size_t>(kDelayMs * sampleRate_ / 1000.0);
            for (auto& l : line_)
                l.assign(len, 0.0f);
            pos_ = 0;
        }
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override {
        double v = 0;
        int32 read = 0;
        if (!state || state->read(&v, sizeof v, &read) != kResultOk || read != sizeof v)
            return kResultFalse;
        mix_ = v;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) override {
        int32 written = 0;
        return state && state->write(&mix_, sizeof mix_, &written) == kResultOk ? kResultOk : kResultFalse;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 numIns, SpeakerArrangement* out, int32 numOuts) override {
        return numIns == 1 && numOuts == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0)
            return kInvalidArgument;
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        processing_ = state;
        if (state) {
            g_setProcessingOn++;
            for (auto& l : line_)
                std::fill(l.begin(), l.end(), 0.0f);
            pos_ = 0;
        } else {
            g_setProcessingOff++;
        }
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (!processing_)
            g_processWhileOff++;
        if (IParameterChanges* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* q = changes->getParameterData(i);
                if (q && q->getParameterId() == 0 && q->getPointCount() > 0) {
                    int32 offset = 0;
                    ParamValue v = 0;
                    if (q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk)
                        mix_ = v;
                }
            }
        }
        if (data.numSamples == 0 || data.numInputs < 1 || data.numOutputs < 1 || line_[0].empty())
            return kResultOk;
        float** in = data.inputs[0].channelBuffers32;
        float** out = data.outputs[0].channelBuffers32;
        const size_t len = line_[0].size();
        for (int32 n = 0; n < data.numSamples; ++n) {
            for (int c = 0; c < 2; ++c) {
                const float echo = line_[c][pos_];
                line_[c][pos_] = in[c][n] + echo * kFeedback;
                out[c][n] = in[c][n] + echo * static_cast<float>(mix_);
            }
            if (++pos_ == len)
                pos_ = 0;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return static_cast<uint32>(16 * kDelayMs * sampleRate_ / 1000.0); }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0)
            return kInvalidArgument;
        std::memset(&info, 0, sizeof info);
        info.id = 0;
        const char16_t name[] = u"Mix";
        std::memcpy(info.title, name, sizeof name);
        info.defaultNormalizedValue = 0.5;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 out) override {
        if (id != 0)
            return kInvalidArgument;
        char text[32];
        std::snprintf(text, sizeof text, "%.0f %%", v * 100);
        int i = 0;
        for (; text[i] && i < 127; ++i)
            out[i] = static_cast<char16_t>(text[i]);
        out[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id == 0 ? controllerMix_ : 0.0; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
        if (id != 0)
            return kInvalidArgument;
        controllerMix_ = v;
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    std::atomic<uint32> refs_{1};
    double sampleRate_ = 48000.0;
    bool processing_ = false;
    double mix_ = 0.5, controllerMix_ = 0.5;
    std::vector<float> line_[2];
    size_t pos_ = 0;
};

class Factory : public IPluginFactory {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        std::memset(info, 0, sizeof *info);
        std::strncpy(info->vendor, "TwoStorms test", PFactoryInfo::kNameSize - 1);
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (index != 0)
            return kInvalidArgument;
        std::memset(info, 0, sizeof *info);
        std::memcpy(info->cid, kClassId, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
        std::strncpy(info->name, "Cue Test Delay", PClassInfo::kNameSize - 1);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (!FUnknownPrivate::iidEqual(cid, kClassId)) {
            *obj = nullptr;
            return kNoInterface;
        }
        auto* p = new TestDelay();
        const tresult r = p->queryInterface(iid, obj);
        p->release();
        return r;
    }
};

Factory g_factory;

} // namespace

extern "C" {
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() { return &g_factory; }

SMTG_EXPORT_SYMBOL long cue_vst3_test_stat(const char* name) {
    if (!std::strcmp(name, "processing_on")) return g_setProcessingOn;
    if (!std::strcmp(name, "processing_off")) return g_setProcessingOff;
    if (!std::strcmp(name, "process_while_off")) return g_processWhileOff;
    if (!std::strcmp(name, "instances")) return g_instances;
    return -1;
}
}
