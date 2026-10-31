#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace mperoll
{

/** What a hosted VST3 synth can take of MPE, as its edit controller tells it.

    JUCE's VST3 host passes MIDI pitch bend, channel pressure and controllers to a synth only as
    the parameters its IMidiMapping assigns to each MIDI channel, and drops them on a channel it
    assigns nothing to. So MPE over MIDI reaches a VST3 synth's notes one by one only if every
    member channel has a pitch bend of its own, as JUCE-built synths have. A synth that maps pitch
    bend on MIDI channel 1 only, or one parameter for every channel, may take VST3 note expressions
    instead: tuning, plus pressure and brightness through its physical UI mapping.
*/
struct Vst3Support
{
    static constexpr uint32_t noType = 0xffffffffu;

    // Indexed like CLAP's note expressions (and VST3's predefined types, which come in the same
    // order): volume, pan, tuning, vibrato, expression, brightness, pressure.
    static constexpr int numExpressions = 7;
    static constexpr int tuning = 2, brightness = 5, pressure = 6;

    bool isVst3 = false;
    bool hasEditController = false, hasMidiMapping = false, hasNoteExpressions = false;

    int channelsWithBend = 0;               // MIDI channels (of 16) whose pitch bend reaches a parameter
    int channelsWithOwnBend = 0;            // ... one no other channel shares
    bool ownBendOnMemberChannels = false;   // channels 2-16 each have their own: MPE over MIDI reaches notes one by one
    int channelsWithPressure = 0, channelsWithTimbre = 0;

    // The parameters (VST3 IDs, sorted) a MIDI controller, the pitch bend or aftertouch is mapped to
    // on any channel: JUCE moves them with the MIDI MPE Roll sends, so they follow the notes
    // (Kontakt's and Analog Lab's "channel1 pitch bend"... "channel16 pitch bend").
    std::vector<uint32_t> midiMappedParameters;

    /** Whether a parameter, by its VST3 ID, is one a MIDI controller or the pitch bend is mapped to. */
    bool isMidiMapped (uint32_t parameterId) const noexcept
    {
        return std::binary_search (midiMappedParameters.begin(), midiMappedParameters.end(), parameterId);
    }

    std::array<uint32_t, numExpressions> typeIds;   // the VST3 note expression type for each, or noType
    std::array<bool, numExpressions> scaled {};      // a custom type from the physical UI mapping: 0..1 goes to minimum..maximum
    std::array<double, numExpressions> minimum {}, maximum {};

    Vst3Support()
    {
        typeIds.fill (noType);
        maximum.fill (1.0);
    }

    bool takesTuning() const noexcept               { return typeIds[(size_t) tuning] != noType; }

    /** MPE over MIDI can't reach this synth's notes one by one. */
    bool midiMissesNotes() const noexcept           { return isVst3 && hasEditController && ! ownBendOnMemberChannels; }

    /** Notes would have their own pitch only as VST3 note expressions. */
    bool wantsNoteExpressions() const noexcept      { return midiMissesNotes() && takesTuning(); }

    /** Which channels its pitch bend reaches, in words: "on MIDI channel 1 only", "one setting for every channel"... */
    juce::String describeBend() const;

    /** The note expressions it takes, in words: "tuning, pressure and brightness". */
    juce::String describeNoteExpressions() const;

    /** Everything, in a line, for the plugin log. */
    juce::String describe() const;

    // Exact on purpose: these comparisons detect changes, not closeness.
    JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
    bool operator== (const Vst3Support&) const = default;
    JUCE_END_IGNORE_WARNINGS_GCC_LIKE

    /** Asks the plugin. For anything but a VST3 (or in a build without VST3 hosting), isVst3 is
        false. Message thread, as VST3 edit controllers expect.
    */
    static Vst3Support probe (juce::AudioPluginInstance&);
};

} // namespace mperoll
