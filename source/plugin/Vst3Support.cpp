#include "Vst3Support.h"

#include <algorithm>
#include <map>

#if JUCE_PLUGINHOST_VST3
 // Declarations only: JUCE's own VST3 hosting code defines the interface IDs.
 #define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
 #include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>
 #undef JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY

 JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4100 4458)
 JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wnon-virtual-dtor", "-Wshadow", "-Wsign-conversion", "-Wconversion",
                                      "-Wextra-semi", "-Wzero-as-null-pointer-constant", "-Wunused-parameter",
                                      "-Wpedantic", "-Wpragma-pack", "-Wnewline-eof", "-Wdeprecated")
 #include <pluginterfaces/vst/ivstnoteexpression.h>
 #include <pluginterfaces/vst/ivstphysicalui.h>
 JUCE_END_IGNORE_WARNINGS_GCC_LIKE
 JUCE_END_IGNORE_WARNINGS_MSVC
#endif

namespace mperoll
{

juce::String Vst3Support::describeBend() const
{
    if (! hasMidiMapping || channelsWithBend == 0)
        return "on no MIDI channel";

    if (ownBendOnMemberChannels)
        return "on every MIDI channel, each its own";

    if (channelsWithBend == 1)
        return "on one MIDI channel only";

    if (channelsWithOwnBend == 0)
        return "as one setting for every MIDI channel";

    return "on " + juce::String (channelsWithOwnBend) + " of 16 MIDI channels";
}

juce::String Vst3Support::describeNoteExpressions() const
{
    static constexpr const char* names[numExpressions] { "volume", "pan", "tuning", "vibrato", "expression", "brightness", "pressure" };

    // Tuning first: it's the one that decides.
    juce::StringArray taken;

    for (auto i : { 2, 6, 5, 0, 1, 3, 4 })
        if (typeIds[(size_t) i] != noType)
            taken.add (names[i]);

    if (taken.isEmpty())
        return {};

    if (taken.size() == 1)
        return taken[0];

    const auto last = taken[taken.size() - 1];
    taken.remove (taken.size() - 1);
    return taken.joinIntoString (", ") + " and " + last;
}

juce::String Vst3Support::describe() const
{
    if (! isVst3)
        return "not a VST3";

    if (! hasEditController)
        return "VST3 with no edit controller to ask";

    juce::String s;
    s << "VST3: pitch bend " << describeBend()
      << " (" << channelsWithBend << " mapped, " << channelsWithOwnBend << " of their own), channel pressure on "
      << channelsWithPressure << ", CC74 on " << channelsWithTimbre << " channels; note expressions: "
      << (hasNoteExpressions ? (describeNoteExpressions().isNotEmpty() ? describeNoteExpressions() : juce::String ("none usable"))
                             : juce::String ("none"));
    return s;
}

#if JUCE_PLUGINHOST_VST3
namespace
{
    using Mapped = std::array<Steinberg::Vst::ParamID, 16>;

    // The parameter a controller is mapped to on each MIDI channel, or kNoParamId.
    Mapped mappingsOf (Steinberg::Vst::IMidiMapping& mapping, Steinberg::Vst::CtrlNumber controller)
    {
        Mapped ids;

        for (Steinberg::int16 channel = 0; channel < 16; ++channel)
        {
            Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;

            if (mapping.getMidiControllerAssignment (0, channel, controller, id) != Steinberg::kResultTrue)
                id = Steinberg::Vst::kNoParamId;

            ids[(size_t) channel] = id;
        }

        return ids;
    }

    int countMapped (const Mapped& ids)
    {
        return (int) std::count_if (ids.begin(), ids.end(), [] (auto id) { return id != Steinberg::Vst::kNoParamId; });
    }

    bool isOwn (const Mapped& ids, size_t channel)
    {
        return ids[channel] != Steinberg::Vst::kNoParamId && std::count (ids.begin(), ids.end(), ids[channel]) == 1;
    }
}

Vst3Support Vst3Support::probe (juce::AudioPluginInstance& plugin)
{
    using namespace Steinberg;
    Vst3Support s;
    auto* client = plugin.getVST3Client();

    if (client == nullptr)
        return s;

    s.isVst3 = true;
    auto* controller = client->getIEditControllerPtr();

    if (controller == nullptr)
        return s;

    s.hasEditController = true;

    if (FUnknownPtr<Vst::IMidiMapping> mapping (controller); mapping != nullptr)
    {
        s.hasMidiMapping = true;
        const auto bend = mappingsOf (*mapping, Vst::kPitchBend);
        s.channelsWithBend = countMapped (bend);
        s.channelsWithPressure = countMapped (mappingsOf (*mapping, Vst::kAfterTouch));
        s.channelsWithTimbre = countMapped (mappingsOf (*mapping, (Vst::CtrlNumber) 74));
        s.ownBendOnMemberChannels = true;

        for (size_t channel = 0; channel < 16; ++channel)
        {
            const auto own = isOwn (bend, channel);
            s.channelsWithOwnBend += own ? 1 : 0;

            if (channel > 0 && ! own)
                s.ownBendOnMemberChannels = false;
        }

        // Every parameter any controller is mapped to, on any channel: they follow the MIDI the synth
        // is sent, so a change in one isn't an edit to tell the host about.
        for (Vst::CtrlNumber number = 0; number < Vst::kCountCtrlNumber; ++number)
            for (const auto id : mappingsOf (*mapping, number))
                if (id != Vst::kNoParamId)
                    s.midiMappedParameters.push_back ((uint32_t) id);

        std::sort (s.midiMappedParameters.begin(), s.midiMappedParameters.end());
        s.midiMappedParameters.erase (std::unique (s.midiMappedParameters.begin(), s.midiMappedParameters.end()),
                                      s.midiMappedParameters.end());
    }

    FUnknownPtr<Vst::INoteExpressionController> expressions (controller);

    if (expressions == nullptr)
        return s;

    // The types it lists, with their ranges.
    std::map<uint32_t, std::pair<double, double>> listed;
    const auto count = expressions->getNoteExpressionCount (0, 0);

    for (int32 i = 0; i < juce::jmin (count, (int32) 256); ++i)
    {
        Vst::NoteExpressionTypeInfo info {};

        if (expressions->getNoteExpressionInfo (0, 0, i, info) == kResultTrue)
            listed[info.typeId] = { juce::jlimit (0.0, 1.0, info.valueDesc.minimum),
                                    juce::jlimit (0.0, 1.0, info.valueDesc.maximum) };
    }

    s.hasNoteExpressions = ! listed.empty();

    const auto use = [&s, &listed] (int expression, uint32_t typeId, bool scaled)
    {
        const auto found = listed.find (typeId);

        if (typeId == Vst::kInvalidTypeID || found == listed.end())
            return false;

        const auto [low, high] = found->second;
        s.typeIds[(size_t) expression] = typeId;
        s.scaled[(size_t) expression] = scaled;
        s.minimum[(size_t) expression] = juce::jmin (low, high);
        s.maximum[(size_t) expression] = high > low ? high : 1.0;
        return true;
    };

    // The predefined types carry their own meaning, so their values go as they are.
    use (0, Vst::kVolumeTypeID, false);
    use (1, Vst::kPanTypeID, false);
    use (tuning, Vst::kTuningTypeID, false);
    use (3, Vst::kVibratoTypeID, false);
    use (4, Vst::kExpressionTypeID, false);

    // Where a synth takes timbre and pressure is what its physical UI mapping says (MPE's Y and
    // pressure), as for Steinberg's own MPE support; brightness is the fallback for timbre.
    Vst::PhysicalUIMap map[2] { { Vst::kPUIYMovement, Vst::kInvalidTypeID }, { Vst::kPUIPressure, Vst::kInvalidTypeID } };

    if (FUnknownPtr<Vst::INoteExpressionPhysicalUIMapping> physical (controller); physical != nullptr)
    {
        Vst::PhysicalUIMapList list { 2, map };

        if (physical->getPhysicalUIMapping (0, 0, list) != kResultTrue)
            map[0].noteExpressionTypeID = map[1].noteExpressionTypeID = Vst::kInvalidTypeID;
    }

    if (! use (brightness, map[0].noteExpressionTypeID, map[0].noteExpressionTypeID != Vst::kBrightnessTypeID))
        use (brightness, Vst::kBrightnessTypeID, false);

    use (pressure, map[1].noteExpressionTypeID, true);
    return s;
}
#else
Vst3Support Vst3Support::probe (juce::AudioPluginInstance&)
{
    return {};
}
#endif

} // namespace mperoll
