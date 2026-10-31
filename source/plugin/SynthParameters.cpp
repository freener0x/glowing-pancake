#include "SynthParameters.h"

#include <map>
#include <optional>

namespace mperoll
{

namespace
{
    juce::String parameterIdOf (juce::AudioProcessorParameter& p)
    {
        if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (&p))
            return hosted->getParameterID();

        return juce::String (p.getParameterIndex());
    }

    /** The message thread, or a lock on it. */
    struct MessageThreadAccess
    {
        MessageThreadAccess()
        {
            if (! juce::MessageManager::existsAndIsCurrentThread())
                lock.emplace();
        }

        bool granted() const    { return ! lock.has_value() || lock->lockWasGained(); }

        std::optional<juce::MessageManagerLock> lock;
    };

    // Which synth a set of slots belongs to. Leaves out the file path, so moving the plugin, or
    // opening the project on another computer, keeps the slots.
    juce::String identityOf (const juce::PluginDescription& d)
    {
        return d.pluginFormatName + ":" + juce::String (d.uniqueId != 0 ? d.uniqueId : d.deprecatedUid);
    }
}

//==============================================================================
SynthParameterProxy::SynthParameterProxy (SynthParameterBank& b, int s)
    : AudioProcessorParameterWithID (juce::ParameterID ("synth" + juce::String (s + 1), 1),
                                     "Synth " + juce::String (s + 1)),
      bank (b), slot (s)
{
}

float SynthParameterProxy::getValue() const         { return bank.getValue (slot); }
void SynthParameterProxy::setValue (float newValue) { bank.setValue (slot, newValue); }

float SynthParameterProxy::getDefaultValue() const
{
    float result = 0.0f;
    bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { result = p.getDefaultValue(); });
    return result;
}

juce::String SynthParameterProxy::getName (int maximumStringLength) const
{
    juce::String result;

    if (! bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p)
                                    { result = p.getName (maximumStringLength); }))
        result = "Synth " + juce::String (slot + 1) + " (unused)";

    return result.substring (0, maximumStringLength);
}

juce::String SynthParameterProxy::getLabel() const
{
    juce::String result;
    bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { result = p.getLabel(); });
    return result;
}

// Text conversion can need the message thread: JUCE's VST3 host locks it, and CLAP only allows
// it there. A host asking from another thread therefore locks the message thread first, so the
// synth can't be swapped meanwhile and nothing waits on us while we wait for it.
juce::String SynthParameterProxy::getText (float value, int maximumStringLength) const
{
    juce::String result = juce::String (value, 3);
    const MessageThreadAccess access;

    if (access.granted())
        bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p)
                                  { result = p.getText (value, maximumStringLength); });

    return result;
}

float SynthParameterProxy::getValueForText (const juce::String& text) const
{
    float result = text.getFloatValue();
    const MessageThreadAccess access;

    if (access.granted())
        bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p)
                                  { result = p.getValueForText (text); });

    return result;
}

int SynthParameterProxy::getNumSteps() const
{
    int result = juce::AudioProcessor::getDefaultNumParameterSteps();
    bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { result = p.getNumSteps(); });
    return result;
}

bool SynthParameterProxy::isDiscrete() const
{
    bool result = false;
    bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { result = p.isDiscrete(); });
    return result;
}

bool SynthParameterProxy::isBoolean() const
{
    bool result = false;
    bank.synth.withParameter (bank.getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { result = p.isBoolean(); });
    return result;
}

//==============================================================================
SynthParameterBank::SynthParameterBank (juce::AudioProcessor& o, HostedSynth& s)
    : owner (o), synth (s)
{
    for (int i = 0; i < numSlots; ++i)
    {
        slotToHosted[(size_t) i] = -1;
        cachedValues[(size_t) i] = 0.0f;

        auto proxy = std::make_unique<SynthParameterProxy> (*this, i);
        proxies[(size_t) i] = proxy.get();
        owner.addParameter (proxy.release());
    }
}

SynthParameterBank::~SynthParameterBank()
{
    detach();
}

int SynthParameterBank::getHostedIndex (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots) ? slotToHosted[(size_t) slot].load() : -1;
}

float SynthParameterBank::getValue (int slot) const
{
    auto value = cachedValues[(size_t) slot].load();

    if (synth.withParameter (getHostedIndex (slot), [&] (juce::AudioProcessorParameter& p) { value = p.getValue(); }))
        cachedValues[(size_t) slot] = value;

    return value;
}

void SynthParameterBank::setValue (int slot, float value)
{
    cachedValues[(size_t) slot] = value;
    synth.setParameterValue (getHostedIndex (slot), value);     // and the same on every copy of it
}

void SynthParameterBank::parameterValueChanged (int hostedIndex, float newValue)
{
    if (! juce::isPositiveAndBelow (hostedIndex, (int) hostedToSlot.size()))
        return;

    const auto slot = hostedToSlot[(size_t) hostedIndex];

    if (slot >= 0)
    {
        cachedValues[(size_t) slot] = newValue;

        // The synth following the notes it's sent (its wheels, a parameter its MIDI mapping moves
        // with each channel's pitch bend, or anything its first copy moves while it plays in copy
        // mode) isn't an edit: reported to the host, it would fill its undo history, record as
        // automation, and come back to move every copy with the first one's note.
        if (! synth.isFollowingItsNotes (hostedIndex))
            proxies[(size_t) slot]->sendValueChangedMessageToListeners (newValue);
    }
}

void SynthParameterBank::parameterGestureChanged (int hostedIndex, bool gestureIsStarting)
{
    if (! juce::isPositiveAndBelow (hostedIndex, (int) hostedToSlot.size()))
        return;

    const auto slot = hostedToSlot[(size_t) hostedIndex];

    if (slot < 0)
        return;

    if (gestureIsStarting)
        proxies[(size_t) slot]->beginChangeGesture();
    else
        proxies[(size_t) slot]->endChangeGesture();
}

//==============================================================================
void SynthParameterBank::detach()
{
    if (listenedTo == nullptr)
        return;

    // removeListener waits for any callback in flight, so nothing reaches us after this.
    for (auto* p : listenedTo->getParameters())
        p->removeListener (this);

    listenedTo = nullptr;
}

void SynthParameterBank::synthRemoving (juce::AudioPluginInstance& outgoing)
{
    if (&outgoing == listenedTo)
    {
        detach();

        for (auto& s : slotToHosted)
            s = -1;

        numMapped = 0;
    }
}

void SynthParameterBank::synthChanged()
{
    detach();

    for (auto& s : slotToHosted)
        s = -1;

    hostedToSlot.clear();
    int mapped = 0;

    auto* instance = synth.getInstance();
    const auto* description = synth.getDescription();

    if (instance != nullptr && description != nullptr)
    {
        const auto identity = identityOf (*description);

        // The slots were made for another synth: start afresh.
        if (slotSynth.isNotEmpty() && slotSynth != identity)
            slotIds.clear();

        slotSynth = identity;

        while (slotIds.size() < numSlots)
            slotIds.add ({});

        const auto& params = instance->getParameters();
        hostedToSlot.assign ((size_t) params.size(), -1);

        auto assign = [&] (int slot, int hosted)
        {
            slotToHosted[(size_t) slot] = hosted;
            hostedToSlot[(size_t) hosted] = slot;
            cachedValues[(size_t) slot] = params.getUnchecked (hosted)->getValue();
            ++mapped;
        };

        // Slots that already stand for a parameter follow its ID, wherever the synth now lists it.
        // A plugin may (wrongly) repeat an ID, so each ID lists every parameter that has it.
        std::map<juce::String, std::vector<int>> byId;

        for (int i = 0; i < params.size(); ++i)
            byId[parameterIdOf (*params.getUnchecked (i))].push_back (i);

        for (int slot = 0; slot < numSlots; ++slot)
        {
            if (slotIds[slot].isEmpty())
                continue;

            const auto found = byId.find (slotIds[slot]);

            if (found == byId.end())
                continue;   // the parameter has gone: keep the slot for it, unused

            for (auto hosted : found->second)
            {
                if (hostedToSlot[(size_t) hosted] < 0 && params.getUnchecked (hosted) != instance->getBypassParameter())
                {
                    assign (slot, hosted);
                    break;
                }
            }
        }

        // New parameters go into the free slots, in order. Only parameters a host could automate:
        // JUCE-built synths, for instance, also list a bypass and thousands of hidden MIDI CC
        // parameters that would otherwise crowd out the real ones.
        int nextSlot = 0;

        for (int hosted = 0; hosted < params.size(); ++hosted)
        {
            auto* p = params.getUnchecked (hosted);

            if (hostedToSlot[(size_t) hosted] >= 0 || ! p->isAutomatable() || p == instance->getBypassParameter())
                continue;

            while (nextSlot < numSlots && slotIds[nextSlot].isNotEmpty())
                ++nextSlot;

            if (nextSlot >= numSlots)
                break;

            slotIds.set (nextSlot, parameterIdOf (*p));
            assign (nextSlot, hosted);
        }

        listenedTo = instance;

        for (auto* p : params)
            p->addListener (this);
    }

    numMapped = mapped;
    owner.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withParameterInfoChanged (true));
}

//==============================================================================
std::unique_ptr<juce::XmlElement> SynthParameterBank::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("SynthParameters");

    if (slotSynth.isNotEmpty())
        xml->setAttribute ("synth", slotSynth);

    for (int slot = 0; slot < slotIds.size(); ++slot)
    {
        if (slotIds[slot].isEmpty())
            continue;

        auto* child = xml->createNewChildElement ("Slot");
        child->setAttribute ("index", slot);
        child->setAttribute ("id", slotIds[slot]);
    }

    return xml;
}

void SynthParameterBank::fromXml (const juce::XmlElement* xml)
{
    slotIds.clear();
    slotSynth.clear();

    if (xml == nullptr)
        return;

    slotSynth = xml->getStringAttribute ("synth");

    for (auto* child : xml->getChildWithTagNameIterator ("Slot"))
    {
        const auto slot = child->getIntAttribute ("index", -1);

        if (! juce::isPositiveAndBelow (slot, numSlots))
            continue;

        while (slotIds.size() <= slot)
            slotIds.add ({});

        slotIds.set (slot, child->getStringAttribute ("id"));
    }
}

} // namespace mperoll
