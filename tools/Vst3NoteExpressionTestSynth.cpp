// A minimal polyphonic VST3 sine synth, written directly against the VST3 SDK's interfaces
// (pluginterfaces only: no JUCE, no public.sdk classes). It's only used by the developer harness to
// check how MPE Roll plays VST3 synths that aren't built with JUCE.
//
// It behaves like many such synths. Per-note pitch comes only from VST3 note expressions (tuning, a
// custom "Pressure" one and volume), and through IMidiMapping it maps pitch bend on MIDI channel 1
// only, to a global "channel1 pitch bend" parameter, the way Steinberg's own IMidiMapping example does. JUCE's
// VST3 host turns pitch bend, channel pressure and CCs into parameter changes through IMidiMapping
// and drops them on channels the synth maps nothing for, so MPE's per-note bends on channels 2-16
// never reach it as MIDI: a note that glides in a render proves MPE Roll sent its MPE as note
// expressions.
//
// One object is the component, the audio processor and the edit controller (a "single component"
// plug-in): getControllerClassId() names no separate controller, so a host gets the edit controller
// by asking the component for IEditController.
//
// Interfaces are matched against the inline TUIDs the SDK headers declare (IComponent_iid, ...), so
// no FUID::iid definitions and none of the SDK's .cpp files are needed.

#if defined (_MSC_VER)
 #pragma warning (push, 0)
#elif defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wpedantic"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wcast-align"
 #pragma GCC diagnostic ignored "-Wfloat-equal"
 #pragma GCC diagnostic ignored "-Wredundant-decls"
 #pragma GCC diagnostic ignored "-Woverloaded-virtual"
 #pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/vst/vsttypes.h>

#if defined (_MSC_VER)
 #pragma warning (pop)
#elif defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

using namespace Steinberg;
using FUnknownPrivate::iidEqual;

// "MPENOTEXPTESTSY1"
constexpr TUID synthClassId = INLINE_UID (0x4D50454E, 0x4F544558, 0x50544553, 0x54535931);

constexpr const char* synthName = "MPE Roll Note Expression Test Synth";
constexpr const char* vendorName = "Glowing Pancake";

constexpr Vst::ParamID pitchBendId = 1;
constexpr Vst::NoteExpressionTypeID pressureTypeId = Vst::kCustomStart;     // 100000

constexpr double twoPi = 6.283185307179586;
constexpr double bendRangeSemitones = 2.0;
constexpr double attackSeconds = 0.003, releaseSeconds = 0.01;
constexpr int32 stateVersion = 1, stateSize = 4;
constexpr int16 numChannels = 16;

/** The global pitch bend parameter ("channel1 pitch bend"): 0.5 is the centre, 0 and 1 are two
    semitones down and up. */
double bendToSemitones (double normalised) noexcept   { return (normalised - 0.5) * 2.0 * bendRangeSemitones; }
double semitonesToBend (double semitones) noexcept    { return std::clamp (semitones / (2.0 * bendRangeSemitones) + 0.5, 0.0, 1.0); }

/** VST3's tuning note expression: 0.5 is none, 0 and 1 are ten octaves down and up. */
double tuningToSemitones (double normalised) noexcept { return (normalised - 0.5) * 240.0; }

void copyString (Vst::String128 target, const char* text) noexcept
{
    int i = 0;

    for (; i < 127 && text[i] != 0; ++i)
        target[i] = static_cast<Vst::TChar> (static_cast<unsigned char> (text[i]));

    target[i] = 0;
}

/** The number a UTF-16 string starts with (ASCII only). False if it doesn't start with one. */
bool parseNumber (const Vst::TChar* text, double& result) noexcept
{
    if (text == nullptr)
        return false;

    char ascii[128] {};

    for (int i = 0; i < 127 && text[i] != 0; ++i)
        ascii[i] = text[i] < 128 ? static_cast<char> (text[i]) : '?';

    char* end = nullptr;
    result = std::strtod (ascii, &end);
    return end != ascii;
}

//==============================================================================
struct ExpressionType
{
    Vst::NoteExpressionTypeID id;
    const char* title;
    const char* shortTitle;
    const char* units;
    double defaultValue;
    int32 flags;
};

constexpr std::array<ExpressionType, 3> expressionTypes
{ {
    { Vst::kTuningTypeID, "Tuning",   "Tune", "st", 0.5,
      Vst::NoteExpressionTypeInfo::kIsBipolar | Vst::NoteExpressionTypeInfo::kIsAbsolute },
    { pressureTypeId,     "Pressure", "Pres", "%",  0.0,  Vst::NoteExpressionTypeInfo::kIsAbsolute },
    { Vst::kVolumeTypeID, "Volume",   "Vol",  "dB", 0.25, Vst::NoteExpressionTypeInfo::kIsAbsolute },
} };

//==============================================================================
struct Voice
{
    enum class Stage { idle, playing, releasing };

    Stage stage = Stage::idle;
    int32 noteId = -1;
    int16 channel = 0, pitch = 0;
    uint64 started = 0;             // which note-on this was, to steal the oldest voice
    double velocity = 0.0;
    double noteTuning = 0.0;        // semitones, from the note-on's own tuning (in cents)
    double expressionTuning = 0.0;  // semitones, from kTuningTypeID note expressions
    double pressure = 1.0;          // amplitude scale, from "Pressure" note expressions
    double volume = 1.0;            // gain, from kVolumeTypeID note expressions
    double envelope = 0.0;          // 0 to 1: a linear attack and release
    double phase = 0.0;

    /** A note-off with a note ID finds the note-on with that ID; without one, the same channel and key. */
    bool plays (int32 id, int16 ch, int16 key) const noexcept
    {
        if (id != -1 && noteId != -1)
            return noteId == id;

        return channel == ch && pitch == key;
    }
};

//==============================================================================
class NoteExpressionTestSynth final : public Vst::IComponent,
                                      public Vst::IAudioProcessor,
                                      public Vst::IEditController,
                                      public Vst::IMidiMapping,
                                      public Vst::INoteExpressionController,
                                      public Vst::INoteExpressionPhysicalUIMapping
{
public:
    NoteExpressionTestSynth() = default;

    //==============================================================================
    // FUnknown
    tresult PLUGIN_API queryInterface (const TUID queried, void** object) override
    {
        if (object == nullptr)
            return kInvalidArgument;

        *object = nullptr;

        if (iidEqual (queried, FUnknown_iid) || iidEqual (queried, IPluginBase_iid) || iidEqual (queried, Vst::IComponent_iid))
            *object = static_cast<Vst::IComponent*> (this);
        else if (iidEqual (queried, Vst::IAudioProcessor_iid))
            *object = static_cast<Vst::IAudioProcessor*> (this);
        else if (iidEqual (queried, Vst::IEditController_iid))
            *object = static_cast<Vst::IEditController*> (this);
        else if (iidEqual (queried, Vst::IMidiMapping_iid))
            *object = static_cast<Vst::IMidiMapping*> (this);
        else if (iidEqual (queried, Vst::INoteExpressionController_iid))
            *object = static_cast<Vst::INoteExpressionController*> (this);
        else if (iidEqual (queried, Vst::INoteExpressionPhysicalUIMapping_iid))
            *object = static_cast<Vst::INoteExpressionPhysicalUIMapping*> (this);
        else
            return kNoInterface;

        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override
    {
        return ++refCount;
    }

    uint32 PLUGIN_API release() override
    {
        const auto remaining = --refCount;

        if (remaining == 0)
            delete this;

        return remaining;
    }

    //==============================================================================
    // IPluginBase, for IComponent and IEditController both
    tresult PLUGIN_API initialize (FUnknown*) override   { return kResultOk; }
    tresult PLUGIN_API terminate() override              { return kResultOk; }

    //==============================================================================
    // IComponent
    tresult PLUGIN_API getControllerClassId (TUID) override   { return kResultFalse; }  // this object is the controller
    tresult PLUGIN_API setIoMode (Vst::IoMode) override       { return kResultOk; }

    int32 PLUGIN_API getBusCount (Vst::MediaType type, Vst::BusDirection direction) override
    {
        return isOurBus (type, direction, 0) ? 1 : 0;
    }

    tresult PLUGIN_API getBusInfo (Vst::MediaType type, Vst::BusDirection direction, int32 index, Vst::BusInfo& bus) override
    {
        if (! isOurBus (type, direction, index))
            return kInvalidArgument;

        bus = {};
        bus.mediaType = type;
        bus.direction = direction;
        bus.channelCount = type == Vst::kAudio ? 2 : numChannels;
        copyString (bus.name, type == Vst::kAudio ? "Output" : "Notes");
        bus.busType = Vst::kMain;
        bus.flags = Vst::BusInfo::kDefaultActive;
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo (Vst::RoutingInfo&, Vst::RoutingInfo&) override
    {
        return kNotImplemented;
    }

    tresult PLUGIN_API activateBus (Vst::MediaType type, Vst::BusDirection direction, int32 index, TBool) override
    {
        return isOurBus (type, direction, index) ? kResultOk : kInvalidArgument;
    }

    tresult PLUGIN_API setActive (TBool) override
    {
        voices.fill (Voice {});     // not processing now, so not racing process()
        return kResultOk;
    }

    // IComponent's and IEditController's both: there's nothing to keep but a version number.
    tresult PLUGIN_API setState (IBStream* stream) override
    {
        if (stream == nullptr)
            return kInvalidArgument;

        int32 version = 0;
        stream->read (&version, stateSize);
        return kResultOk;
    }

    tresult PLUGIN_API getState (IBStream* stream) override
    {
        if (stream == nullptr)
            return kInvalidArgument;

        int32 version = stateVersion, written = 0;
        return stream->write (&version, stateSize, &written) == kResultOk && written == stateSize ? kResultOk : kResultFalse;
    }

    //==============================================================================
    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements (Vst::SpeakerArrangement*, int32 numIns,
                                           Vst::SpeakerArrangement* outputs, int32 numOuts) override
    {
        return numIns == 0 && numOuts == 1 && outputs != nullptr && outputs[0] == Vst::SpeakerArr::kStereo
                   ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement (Vst::BusDirection direction, int32 index, Vst::SpeakerArrangement& arrangement) override
    {
        if (direction != Vst::kOutput || index != 0)
            return kInvalidArgument;

        arrangement = Vst::SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 size) override
    {
        return size == Vst::kSample32 ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples() override   { return 0; }

    tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& setup) override
    {
        if (setup.symbolicSampleSize != Vst::kSample32 || setup.sampleRate <= 0.0)
            return kResultFalse;

        sampleRate = setup.sampleRate;
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing (TBool) override   { return kResultOk; }

    uint32 PLUGIN_API getTailSamples() override
    {
        return static_cast<uint32> (std::ceil (releaseSeconds * sampleRate));
    }

    tresult PLUGIN_API process (Vst::ProcessData& data) override
    {
        if (data.symbolicSampleSize != Vst::kSample32)
            return kResultFalse;

        readParameterChanges (data.inputParameterChanges);

        const auto numSamples = std::max (0, data.numSamples);
        float* left = nullptr;
        float* right = nullptr;

        // No output buffers (a flush, say): events and parameters are still taken.
        if (data.numOutputs > 0 && data.outputs != nullptr && data.outputs[0].channelBuffers32 != nullptr)
        {
            auto& bus = data.outputs[0];

            for (int32 ch = 0; ch < bus.numChannels; ++ch)
                if (bus.channelBuffers32[ch] != nullptr)
                    std::fill_n (bus.channelBuffers32[ch], numSamples, 0.0f);

            bus.silenceFlags = 0;

            if (bus.numChannels >= 2)
            {
                left = bus.channelBuffers32[0];
                right = bus.channelBuffers32[1];
            }
        }

        // Render up to each event, then apply it.
        int32 rendered = 0;

        if (auto* events = data.inputEvents)
        {
            const auto count = events->getEventCount();

            for (int32 i = 0; i < count; ++i)
            {
                Vst::Event event {};

                if (events->getEvent (i, event) != kResultOk || event.busIndex != 0)
                    continue;

                const auto offset = std::clamp (event.sampleOffset, rendered, numSamples);
                render (left, right, rendered, offset);
                rendered = offset;
                handleEvent (event);
            }
        }

        render (left, right, rendered, numSamples);
        return kResultOk;
    }

    //==============================================================================
    // IEditController
    tresult PLUGIN_API setComponentState (IBStream*) override   { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override               { return 1; }

    tresult PLUGIN_API getParameterInfo (int32 index, Vst::ParameterInfo& info) override
    {
        if (index != 0)
            return kInvalidArgument;

        info = {};
        info.id = pitchBendId;
        // Named as Analog Lab and Kontakt name theirs, which MPE Roll can't tell by the name alone.
        copyString (info.title, "channel1 pitch bend");
        copyString (info.shortTitle, "Bend");
        copyString (info.units, "st");
        info.stepCount = 0;
        info.defaultNormalizedValue = 0.5;
        info.unitId = Vst::kRootUnitId;
        info.flags = Vst::ParameterInfo::kCanAutomate;
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue (Vst::ParamID id, Vst::ParamValue value, Vst::String128 text) override
    {
        if (id != pitchBendId || text == nullptr)
            return kInvalidArgument;

        char ascii[32] {};
        std::snprintf (ascii, sizeof (ascii), "%+.2f", bendToSemitones (value));
        copyString (text, ascii);
        return kResultOk;
    }

    tresult PLUGIN_API getParamValueByString (Vst::ParamID id, Vst::TChar* text, Vst::ParamValue& value) override
    {
        double semitones = 0.0;

        if (id != pitchBendId || ! parseNumber (text, semitones))
            return kResultFalse;

        value = semitonesToBend (semitones);
        return kResultOk;
    }

    Vst::ParamValue PLUGIN_API normalizedParamToPlain (Vst::ParamID id, Vst::ParamValue value) override
    {
        return id == pitchBendId ? bendToSemitones (value) : value;
    }

    Vst::ParamValue PLUGIN_API plainParamToNormalized (Vst::ParamID id, Vst::ParamValue plain) override
    {
        return id == pitchBendId ? semitonesToBend (plain) : plain;
    }

    Vst::ParamValue PLUGIN_API getParamNormalized (Vst::ParamID id) override
    {
        return id == pitchBendId ? controllerBend.load() : 0.0;
    }

    tresult PLUGIN_API setParamNormalized (Vst::ParamID id, Vst::ParamValue value) override
    {
        if (id != pitchBendId)
            return kInvalidArgument;

        controllerBend = std::clamp (value, 0.0, 1.0);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentHandler (Vst::IComponentHandler*) override   { return kResultOk; }   // never used
    IPlugView* PLUGIN_API createView (FIDString) override                      { return nullptr; }

    //==============================================================================
    // IMidiMapping: pitch bend on the first channel only, to the global bend. See the top.
    tresult PLUGIN_API getMidiControllerAssignment (int32 busIndex, int16 channel, Vst::CtrlNumber controller,
                                                    Vst::ParamID& id) override
    {
        if (busIndex != 0 || channel != 0 || controller != Vst::kPitchBend)
            return kResultFalse;

        id = pitchBendId;
        return kResultTrue;
    }

    //==============================================================================
    // INoteExpressionController
    int32 PLUGIN_API getNoteExpressionCount (int32 busIndex, int16 channel) override
    {
        return busIndex == 0 && channel >= 0 && channel < numChannels ? static_cast<int32> (expressionTypes.size()) : 0;
    }

    tresult PLUGIN_API getNoteExpressionInfo (int32 busIndex, int16 channel, int32 index,
                                              Vst::NoteExpressionTypeInfo& info) override
    {
        if (index < 0 || index >= getNoteExpressionCount (busIndex, channel))
            return kInvalidArgument;

        const auto& type = expressionTypes[static_cast<size_t> (index)];

        info = {};
        info.typeId = type.id;
        copyString (info.title, type.title);
        copyString (info.shortTitle, type.shortTitle);
        copyString (info.units, type.units);
        info.unitId = -1;
        info.valueDesc.defaultValue = type.defaultValue;
        info.valueDesc.minimum = 0.0;
        info.valueDesc.maximum = 1.0;
        info.valueDesc.stepCount = 0;
        info.associatedParameterId = Vst::kNoParamId;
        info.flags = type.flags;
        return kResultOk;
    }

    tresult PLUGIN_API getNoteExpressionStringByValue (int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id,
                                                       Vst::NoteExpressionValue value, Vst::String128 text) override
    {
        if (getNoteExpressionCount (busIndex, channel) == 0 || text == nullptr)
            return kInvalidArgument;

        char ascii[32] {};

        switch (id)
        {
            case Vst::kTuningTypeID:
                std::snprintf (ascii, sizeof (ascii), "%+.2f", tuningToSemitones (value));
                break;

            case pressureTypeId:
                std::snprintf (ascii, sizeof (ascii), "%.0f", value * 100.0);
                break;

            case Vst::kVolumeTypeID:
                if (value > 0.0)
                    std::snprintf (ascii, sizeof (ascii), "%+.1f", 20.0 * std::log10 (4.0 * value));
                else
                    std::snprintf (ascii, sizeof (ascii), "-inf");
                break;

            default:
                return kResultFalse;
        }

        copyString (text, ascii);
        return kResultOk;
    }

    tresult PLUGIN_API getNoteExpressionValueByString (int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id,
                                                       const Vst::TChar* text, Vst::NoteExpressionValue& value) override
    {
        double number = 0.0;

        if (getNoteExpressionCount (busIndex, channel) == 0 || ! parseNumber (text, number))
            return kResultFalse;

        switch (id)
        {
            case Vst::kTuningTypeID:    value = number / 240.0 + 0.5; break;
            case pressureTypeId:        value = number / 100.0; break;
            case Vst::kVolumeTypeID:    value = std::pow (10.0, number / 20.0) / 4.0; break;
            default:                    return kResultFalse;
        }

        value = std::clamp (value, 0.0, 1.0);
        return kResultOk;
    }

    //==============================================================================
    // INoteExpressionPhysicalUIMapping: MPE's pitch is X, its pressure is pressure, and Y is left alone.
    tresult PLUGIN_API getPhysicalUIMapping (int32 busIndex, int16 channel, Vst::PhysicalUIMapList& list) override
    {
        if (getNoteExpressionCount (busIndex, channel) == 0 || (list.count > 0 && list.map == nullptr))
            return kInvalidArgument;

        for (uint32 i = 0; i < list.count; ++i)
        {
            auto& entry = list.map[i];

            switch (entry.physicalUITypeID)
            {
                case Vst::kPUIXMovement:    entry.noteExpressionTypeID = Vst::kTuningTypeID; break;
                case Vst::kPUIPressure:     entry.noteExpressionTypeID = pressureTypeId; break;
                default:                    entry.noteExpressionTypeID = Vst::kInvalidTypeID; break;
            }
        }

        return kResultTrue;
    }

private:
    ~NoteExpressionTestSynth() = default;   // release() deletes it

    static bool isOurBus (Vst::MediaType type, Vst::BusDirection direction, int32 index) noexcept
    {
        return index == 0 && ((type == Vst::kAudio && direction == Vst::kOutput)
                               || (type == Vst::kEvent && direction == Vst::kInput));
    }

    /** The last value each block brings: sample accuracy doesn't matter here. */
    void readParameterChanges (Vst::IParameterChanges* changes) noexcept
    {
        if (changes == nullptr)
            return;

        const auto count = changes->getParameterCount();

        for (int32 i = 0; i < count; ++i)
        {
            auto* queue = changes->getParameterData (i);

            if (queue == nullptr || queue->getParameterId() != pitchBendId)
                continue;

            const auto points = queue->getPointCount();
            int32 offset = 0;
            Vst::ParamValue value = 0.5;

            if (points > 0 && queue->getPoint (points - 1, offset, value) == kResultOk)
                processorBend = std::clamp (value, 0.0, 1.0);
        }
    }

    void handleEvent (const Vst::Event& event) noexcept
    {
        switch (event.type)
        {
            case Vst::Event::kNoteOnEvent:              startNote (event.noteOn); break;
            case Vst::Event::kNoteOffEvent:             stopNote (event.noteOff); break;
            case Vst::Event::kNoteExpressionValueEvent: applyExpression (event.noteExpressionValue); break;
            default:                                    break;
        }
    }

    void startNote (const Vst::NoteOnEvent& note) noexcept
    {
        // A free voice, or else the one playing the oldest note.
        auto* voice = &voices[0];

        for (auto& v : voices)
        {
            if (v.stage == Voice::Stage::idle)
            {
                voice = &v;
                break;
            }

            if (v.started < voice->started)
                voice = &v;
        }

        *voice = Voice {};
        voice->stage = Voice::Stage::playing;
        voice->noteId = note.noteId;
        voice->channel = note.channel;
        voice->pitch = note.pitch;
        voice->started = ++notesStarted;
        voice->velocity = std::clamp (static_cast<double> (note.velocity), 0.0, 1.0);
        voice->noteTuning = note.tuning / 100.0;
    }

    void stopNote (const Vst::NoteOffEvent& note) noexcept
    {
        Voice* oldest = nullptr;

        for (auto& v : voices)
            if (v.stage == Voice::Stage::playing && v.plays (note.noteId, note.channel, note.pitch)
                  && (oldest == nullptr || v.started < oldest->started))
                oldest = &v;

        if (oldest != nullptr)
            oldest->stage = Voice::Stage::releasing;
    }

    void applyExpression (const Vst::NoteExpressionValueEvent& expression) noexcept
    {
        if (expression.noteId == -1)
            return;

        const auto value = std::clamp (expression.value, 0.0, 1.0);

        for (auto& v : voices)
        {
            if (v.stage == Voice::Stage::idle || v.noteId != expression.noteId)
                continue;

            switch (expression.typeId)
            {
                case Vst::kTuningTypeID:    v.expressionTuning = tuningToSemitones (value); break;
                case pressureTypeId:        v.pressure = 0.25 + 0.75 * value; break;
                case Vst::kVolumeTypeID:    v.volume = 4.0 * value; break;
                default:                    break;
            }
        }
    }

    void render (float* left, float* right, int32 from, int32 to) noexcept
    {
        if (left == nullptr || right == nullptr || to <= from)
            return;

        const auto bend = bendToSemitones (processorBend);
        const auto attackStep = 1.0 / (attackSeconds * sampleRate);
        const auto releaseStep = 1.0 / (releaseSeconds * sampleRate);

        for (auto& v : voices)
        {
            if (v.stage == Voice::Stage::idle)
                continue;

            const auto semitones = v.pitch - 69 + v.noteTuning + v.expressionTuning + bend;
            const auto increment = twoPi * 440.0 * std::pow (2.0, semitones / 12.0) / sampleRate;
            const auto level = 0.2 * v.velocity * v.pressure * v.volume;

            for (auto i = from; i < to; ++i)
            {
                if (v.stage == Voice::Stage::releasing)
                {
                    v.envelope -= releaseStep;

                    if (v.envelope <= 0.0)
                    {
                        v = Voice {};
                        break;
                    }
                }
                else
                {
                    v.envelope = std::min (1.0, v.envelope + attackStep);
                }

                const auto sample = static_cast<float> (std::sin (v.phase) * level * v.envelope);
                left[i] += sample;
                right[i] += sample;

                v.phase += increment;

                if (v.phase >= twoPi)
                    v.phase = std::fmod (v.phase, twoPi);
            }
        }
    }

    std::atomic<uint32> refCount { 1 };
    std::atomic<double> controllerBend { 0.5 };     // the edit controller's value of Pitch Bend
    double processorBend = 0.5;                     // the value process() last received
    double sampleRate = 44100.0;
    uint64 notesStarted = 0;
    std::array<Voice, 16> voices {};
};

//==============================================================================
/** The module's one factory. It lives as long as the module, so it isn't reference counted. */
class Factory final : public IPluginFactory2
{
public:
    tresult PLUGIN_API queryInterface (const TUID queried, void** object) override
    {
        if (object == nullptr)
            return kInvalidArgument;

        if (iidEqual (queried, FUnknown_iid) || iidEqual (queried, IPluginFactory_iid) || iidEqual (queried, IPluginFactory2_iid))
        {
            *object = static_cast<IPluginFactory2*> (this);
            return kResultOk;
        }

        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override    { return 1; }
    uint32 PLUGIN_API release() override   { return 1; }

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (info == nullptr)
            return kInvalidArgument;

        *info = PFactoryInfo (vendorName, "https://github.com/freener0x/MPE-Roll", "", PFactoryInfo::kUnicode);
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override   { return 1; }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (index != 0 || info == nullptr)
            return kInvalidArgument;

        *info = PClassInfo (synthClassId, PClassInfo::kManyInstances, kVstAudioEffectClass, synthName);
        return kResultOk;
    }

    tresult PLUGIN_API getClassInfo2 (int32 index, PClassInfo2* info) override
    {
        if (index != 0 || info == nullptr)
            return kInvalidArgument;

        *info = PClassInfo2 (synthClassId, PClassInfo::kManyInstances, kVstAudioEffectClass, synthName, 0,
                             Vst::PlugType::kInstrumentSynth, vendorName, "0.1.0", kVstVersionString);
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString classId, FIDString interfaceId, void** object) override
    {
        if (object == nullptr)
            return kInvalidArgument;

        *object = nullptr;

        if (classId == nullptr || interfaceId == nullptr || ! iidEqual (classId, synthClassId))
            return kNoInterface;

        auto* synth = new NoteExpressionTestSynth();
        const auto result = synth->queryInterface (interfaceId, object);
        synth->release();       // what queryInterface() handed out holds the only reference now
        return result;
    }
};

Factory factory;

} // namespace

//==============================================================================
// Entry points: the factory, and the module entry and exit each platform's hosts call.
extern "C"
{

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory()
{
    return &factory;
}

#if SMTG_OS_WINDOWS
SMTG_EXPORT_SYMBOL bool InitDll();
SMTG_EXPORT_SYMBOL bool ExitDll();

SMTG_EXPORT_SYMBOL bool InitDll()   { return true; }
SMTG_EXPORT_SYMBOL bool ExitDll()   { return true; }
#elif SMTG_OS_MACOS
typedef struct __CFBundle* CFBundleRef;     // as CoreFoundation has it, without including it

SMTG_EXPORT_SYMBOL bool bundleEntry (CFBundleRef);
SMTG_EXPORT_SYMBOL bool bundleExit();

SMTG_EXPORT_SYMBOL bool bundleEntry (CFBundleRef)   { return true; }
SMTG_EXPORT_SYMBOL bool bundleExit()                { return true; }
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry (void*);
SMTG_EXPORT_SYMBOL bool ModuleExit();

SMTG_EXPORT_SYMBOL bool ModuleEntry (void*)   { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit()          { return true; }
#endif

} // extern "C"
