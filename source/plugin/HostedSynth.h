#pragma once

#include "core/CopyRouter.h"
#include "core/MpeSettings.h"
#include "CopyWorkers.h"
#include "LoadDiagnosis.h"
#include "PluginScanner.h"
#include "SynthProfiles.h"
#include "Vst3NoteExpressions.h"
#include "Vst3Support.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace mperoll
{

class ClapSynthControls;

/** The synth MPE Roll plays: a VST3, VST2 or CLAP (or AU on macOS) instrument hosted inside the plugin.

    Loading, unloading and the editor window happen on the message thread. process() is called
    on the audio thread and never blocks: while a plugin is being swapped it outputs silence.

    One copy per note (OutputSettings::Mode::copies): beside the synth (the first copy, whose
    window the user sees and whose parameters the host automates) more copies of the same plugin
    are loaded, one at a time, each given the first copy's state. CopyRouter shares the notes out
    over them, one note per copy, as Plain MIDI. Parameter changes of the first copy reach the
    others before their next block; other changes to its sound (a preset picked in its window) are
    copied over as its whole state, when the synth says it changed, when its window closes, every
    few seconds while its window is open, or when the user asks. Copies that have gone quiet stop
    being processed until a note comes for them, the rest are processed on several CPU cores, a
    copy that crashes (where that can be caught: Windows) is switched off rather than taking FL
    with it, copies stop loading when memory runs low, and if FL closes while copies are loading,
    they aren't loaded again for that synth until the user asks.
*/
class HostedSynth : private juce::AudioProcessorListener
{
public:
    HostedSynth();
    ~HostedSynth() override;

    //==============================================================================
    using LoadCallback = std::function<void (const juce::String& error)>;

    /** Loads a synth. If it fails, the error says why as far as can be found out: for a VST2 or VST3,
        Windows' own reason, or what the scanner found retracing the load step by step (see
        LoadDiagnosis), which takes a moment longer (isFindingOutWhy()).
    */
    void load (const juce::PluginDescription& description, LoadCallback onDone = {});

    /** Loads the first instrument in a plugin file, once the scanner has looked at it. onChecked is
        called when the check has passed, right before the synth is loaded: loading can take a while
        and holds up the message thread, so a status line should change there.
    */
    void loadFromFile (const juce::File& file, LoadCallback onDone = {}, std::function<void()> onChecked = {});
    void unload();

    /** File patterns for the plugin formats this build can host, for a file chooser. */
    static juce::String getPluginFilePatterns();

    /** The plugin formats this build can host, for showing to the user ("VST3, VST2 or CLAP"). */
    static juce::String describeHostedFormats();

    /** Whether two descriptions are the same synth: same format and plugin ID, wherever installed. */
    static bool isSameSynth (const juce::PluginDescription&, const juce::PluginDescription&);

    /** Whether two descriptions are the same product (name and maker), whatever the format or file:
        two installs of one plugin, or its VST2 and VST3. */
    static bool isSameProduct (const juce::PluginDescription&, const juce::PluginDescription&);

    /** The descriptions with names that tell them apart in a menu: one whose name another shares gets
        its format added, and one whose name and format another shares (a second install) its folder
        too. The order is kept, so an index into the result indexes the originals. */
    static juce::Array<juce::PluginDescription> withDistinctNames (const juce::Array<juce::PluginDescription>&);

    /** Whether the file is one of FL Studio's own plugins (Harmor, Sytrus...), which come in
        Image-Line's own format: only FL can load them, so loadFromFile finds nothing in them.
    */
    static bool isFlStudioNativePlugin (const juce::File&);

    /** Opens the window for scanning plugins into the known list, or brings it to the front if it's
        already open. The synth owns the window: it goes when the synth does, so a scan can't carry on
        into a list and settings that have been deleted. Message thread.
    */
    void showScanWindow (juce::Colour background = juce::Colour (0xff1e1f22));

    bool isLoaded() const;
    bool isLoading() const noexcept                     { return loading; }
    bool isFindingOutWhy() const noexcept               { return findingOutWhy; }   // a load failed; still loading
    juce::String getName() const;

    /** The name of a synth saved in the project that couldn't be loaded (e.g. not installed here). */
    juce::String getMissingSynthName() const;

    /** Why the project's synth wasn't loaded when the project was opened, if it wasn't: FL Studio or
        MPE Roll closed while it was loading last time, so it's left for the user to load by name
        (Load synth), which forgets that. Empty otherwise. Message thread. */
    juce::String getLoadProblem() const                 { return loadProblem; }
    juce::AudioPluginInstance* getInstance() const      { return instance.get(); }

    /** What the loaded synth is, or nullptr when none is loaded. Message thread. */
    const juce::PluginDescription* getDescription() const   { return instance != nullptr ? description.get() : nullptr; }

    /** Opens the synth's window on the display `near` is on (MPE Roll's window), with its title bar
        on screen; an open one comes to the front. */
    void showEditor (const juce::Component* near = nullptr);

    /** Puts the synth's window back on the display `near` is on, centred and in front (opening it if
        it's closed): for a window that has gone off screen. */
    void bringEditorOnScreen (const juce::Component* near = nullptr);

    void closeEditor();
    bool isEditorShowing() const noexcept               { return editorWindow != nullptr; }

    //==============================================================================
    void prepare (double sampleRate, int maximumBlockSize);
    void release();

    /** Renders the synth into the buffer. The MIDI buffer may be modified by the plugin. A block
        longer than prepare() was told is played in pieces.
    */
    void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead);

    /** Audio thread, after process(): with one copy per note, the furthest a copy's pitch bend went
        past the Zone bend range since the last call (CopyRouter::takeBendOverflow()); otherwise empty. */
    MpeEncoder::BendOverflow takeCopyBendOverflow() noexcept     { return router.takeBendOverflow(); }

    /** Audio thread, before process(): the MPE layout the MIDI is in. A CLAP synth uses it to turn
        the MPE into CLAP note expressions, and a VST3 synth that needs them into VST3 ones.
    */
    void setOutputSettings (const OutputSettings& settings) noexcept   { outputSettings = settings; }

    /** How the synth is played with these settings, for showing to the user. With Plain MIDI or one
        copy per note, also where the synth's own pitch-bend range is set, if MPE Roll knows, and a
        glide that stopped short at it lately (MpeRollProcessor::getRecentBendOverflow()). Message thread. */
    juce::String describeConnection (const OutputSettings&, const MpeEncoder::BendOverflow& recent = {}) const;

    /** The part of describeConnection() about the synth's own pitch-bend range, which Plain MIDI and
        one copy per note bend within: what to set it to, where it is if MPE Roll knows the synth, and
        a glide that stopped short at it lately. Message thread. */
    juce::String describeBendRange (const OutputSettings&, const MpeEncoder::BendOverflow& recent) const;

    /** Whether these settings play the synth with CLAP or VST3 note expressions rather than MIDI. Message thread. */
    bool playsNoteExpressions (const OutputSettings&) const;

    /** What MPE Roll knows about the loaded synth's MPE, if it's one it knows. Message thread. */
    std::optional<SynthProfile> getProfile() const;

    /** What the loaded VST3 synth takes of MPE (isVst3 is false for any other synth). Message thread. */
    const Vst3Support& getVst3Support() const noexcept  { return vst3Support; }

    /** Asks the loaded VST3 synth again what it takes of MPE, as its own MPE switch may have changed
        it, and plays it accordingly. Done every few seconds while one is loaded. Message thread.
    */
    void refreshVst3Support();

    /** The loaded synth's channels, buses and MIDI input, in a line, for the plugin log. Message thread. */
    juce::String describeSetup() const;

    /** True once per synth loaded (or given a project's state) if, after a few seconds' grace for
        it to load its sounds, it has been played over MPE for three seconds of notes in all without
        making a sound: most likely a synth that isn't in MPE mode and so only listens on channel 1.
        A sound before the warning is taken withdraws it. Any thread.
    */
    bool takeSilenceWarning() noexcept                  { return silenceWarning.exchange (false); }

    /** True from that warning until the synth is heard (or loaded, or given a project's state,
        afresh): for a standing reminder where the box may have been missed. Any thread.
    */
    bool isSilentOverMpe() const noexcept               { return silentOverMpe.load(); }

    int getLatencySamples() const;
    double getTailLengthSeconds() const;

    /** Tells the synth whether it's being rendered offline, where it can take its time. Message thread. */
    void setNonRealtime (bool isNonRealtime);

    /** This one is only ever rendered offline (an audio export), beside the one playing live: its
        copies are played on threads that don't take the cores from the live ones, and they keep
        the sound they're loaded with. Message thread, before setCopySettings(). */
    void setRenderedOfflineOnly()                       { offlineOnly = true; }

    //==============================================================================
    std::unique_ptr<juce::XmlElement> saveState() const;
    void restoreState (const juce::XmlElement& xml, LoadCallback onDone = {});

    //==============================================================================
    juce::AudioPluginFormatManager& getFormatManager() noexcept     { return formatManager; }
    juce::KnownPluginList& getKnownPlugins() noexcept               { return knownPlugins; }
    juce::PropertiesFile* getUserSettings()                         { return shared->getUserSettings(); }
    juce::File getDeadMansPedalFile();

    /** For tests: from now on every MPE Roll in this process keeps its settings (and the plugin
        list) in this file, starting from what's saved there.
    */
    void useSettingsFile (const juce::File&);

    /** Instrument types from the scanned plugin list. */
    juce::Array<juce::PluginDescription> getInstrumentTypes() const;

    //==============================================================================
    // One copy per note.

    /** The project's output settings, on the message thread: with mode copies, copies of the synth
        are loaded (or let go) until there are settings.copies of them, and the rest of the copy
        settings take effect. The audio thread plays them as setOutputSettings() says.
    */
    void setCopySettings (const OutputSettings& settings);

    struct CopyStatus
    {
        bool on = false;            // copy mode, with a synth loaded
        int wanted = 0;             // copies asked for
        int loaded = 0;             // copies loaded, the first one included
        int inStep = 0;             // of those, the ones with the first copy's sound
        int playing = 0;            // copies holding a note
        int asleep = 0;             // copies not being processed right now
        int crashed = 0;            // copies switched off after a crash
        int workers = 0;            // threads besides the audio thread processing copies
        bool loading = false;
        bool copyingSound = false;
        juce::String problem;       // why fewer copies are loaded than were asked for
    };

    /** Where copy mode is up to, for showing to the user. Message thread. */
    CopyStatus getCopyStatus() const;

    /** Gives every copy the first copy's sound now (its whole state). Message thread. */
    void copySoundToAllCopies();

    /** Loads the copies again after loading stopped (a crash while loading them last time, low
        memory, a copy that wouldn't load or that crashed). Message thread. */
    void reloadCopies();

    /** Calls back on the message thread once every copy asked for is loaded, or loading has
        stopped short; at once if nothing is to be loaded. For the audio export. */
    void whenCopiesLoaded (std::function<void()> callback);

    /** The copy at this index (0 is the synth itself), or nullptr. Message thread, for tests. */
    juce::AudioPluginInstance* getCopy (int index) const;

    /** Called on the message thread when the copies change: loaded, given the first copy's sound,
        switched off after a crash, or stopped loading. */
    std::function<void()> onCopiesChanged;

    /** Audio thread: the MPE setup is being sent, so each copy gets its pitch-bend range again. */
    void requestConfiguration() noexcept                { configurationRequested = true; }

    /** Sets the synth's parameter at this index, and the same parameter of every copy: what host
        automation does. Any thread; never blocks. Returns false as withParameter() does.
    */
    bool setParameterValue (int index, float value);

    /** Whether a change the synth reports now, at this index, is the first copy following the notes
        it's sent rather than an edit: one reported while it plays its block in copy mode, or of its
        pitch-wheel, mod-wheel or aftertouch parameter. That's its own, not the other copies', so it
        isn't passed on to the host either, which could send it back as automation for every copy.
        Any thread, on the synth's parameter callback. */
    bool isFollowingItsNotes (int index) const noexcept;

    /** Calls fn with the synth's parameter at this index, without ever blocking (safe on any thread,
        and never in the audio thread's way). Returns false if there's no such parameter, or the
        synth is being swapped, prepared or released right now.
    */
    template <typename Fn>
    bool withParameter (int index, Fn&& fn) const
    {
        const ScopedUse use (guard);

        if (! use.entered || instance == nullptr)
            return false;

        const auto& params = instance->getParameters();

        if (! juce::isPositiveAndBelow (index, params.size()))
            return false;

        fn (*params.getUnchecked (index));
        return true;
    }

    /** Called on the message thread whenever a plugin is loaded or unloaded, or a project gives the
        loaded one new state.
    */
    std::function<void()> onPluginChanged;

    /** Called on the message thread when the loaded synth is to be played another way (MPE over MIDI
        or VST3 note expressions) after it said its MPE has changed, so everything is sent again. */
    std::function<void()> onPlayingChanged;

    /** Called on the message thread just before a plugin instance is removed and deleted. */
    std::function<void (juce::AudioPluginInstance&)> onPluginRemoving;

    /** Called on the message thread when the synth reports a change the host should hear about:
        new latency, renamed parameters, or state that needs saving.
    */
    std::function<void (const juce::AudioProcessorListener::ChangeDetails&)> onSynthReportedChange;

private:
    class EditorWindow;

    /** Lets any number of threads use the synth at once (the audio thread, and whichever threads
        the host reads and automates parameters on) and holds them all off while the synth is
        swapped, prepared or released. Users never wait; they're turned away during those moments.
    */
    class InstanceGuard
    {
    public:
        bool tryEnter() const noexcept
        {
            users.fetch_add (1);

            if (exclusive.load())
            {
                users.fetch_sub (1);
                return false;
            }

            return true;
        }

        void exit() const noexcept      { users.fetch_sub (1); }

        /** Waits for current users to finish. Never call it while using the synth yourself. */
        void enterExclusive() noexcept
        {
            while (exclusive.exchange (true))
                std::this_thread::yield();

            while (users.load() != 0)
                std::this_thread::yield();
        }

        void exitExclusive() noexcept   { exclusive = false; }

    private:
        mutable std::atomic<int> users { 0 };
        std::atomic<bool> exclusive { false };
    };

    struct ScopedUse
    {
        explicit ScopedUse (const InstanceGuard& g) noexcept : guard (g), entered (g.tryEnter()) {}
        ~ScopedUse()                                         { if (entered) guard.exit(); }

        const InstanceGuard& guard;
        const bool entered;
    };

    struct ScopedExclusive
    {
        explicit ScopedExclusive (InstanceGuard& g) noexcept : guard (g)   { guard.enterExclusive(); }
        ~ScopedExclusive()                                                  { guard.exitExclusive(); }

        InstanceGuard& guard;
    };

    /** Locks the message thread, unless this thread is it (or already holds it). Taken before the
        guard by anything that holds the guard while calling into the synth from another thread:
        JUCE's VST3 host locks the message thread inside prepareToPlay and its state and text calls,
        while install() holds the guard on the message thread, so the other order could deadlock.
    */
    struct MessageThreadLock
    {
        MessageThreadLock()
        {
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm != nullptr && ! mm->currentThreadHasLockedMessageManager())
                lock.emplace();
        }

        std::optional<juce::MessageManagerLock> lock;
    };

    /** The host's playhead, moved on to where a piece of a long block starts. Audio thread. */
    struct PieceHead final : public juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override;

        juce::AudioPlayHead* source = nullptr;
        int offset = 0;
        double sampleRate = 44100.0;
    };

    static constexpr int maxCopies = CopyRouter::maxCopies;

    /** A copy of the synth beside the first one (`instance`), in copy mode. */
    struct Follower
    {
        enum class State { empty, ready, busy };        // only the message thread changes it

        std::unique_ptr<juce::AudioPluginInstance> plugin;
        ClapSynthControls* clap = nullptr;
        InstanceGuard guard;                            // the audio thread's, while it plays the copy
        std::atomic<State> state { State::empty };      // only a ready copy is played
        std::atomic<bool> crashed { false };            // set by whichever thread caught it: never called again
        std::atomic<bool> fresh { false };              // given new state: the audio thread starts it afresh
        size_t stateHash = 0;                           // message thread: the state it was last given

        // How far it has caught up with the first copy's parameter changes. Owned by whoever holds
        // the copy: the thread playing it, or the message thread while it's busy.
        std::vector<uint32_t> seenParameter, seenChunk;

        // The first copy's parameter changes it has played a block with, counted as the mirror's
        // getChanges() counts them. Set by the thread playing it.
        std::atomic<uint32_t> playedChanges { 0 };
    };

    /** The first copy's parameter changes, as they happen, for the others to catch up with right
        before their next block: on the thread that plays them, so never while they're playing.
        Each parameter, and each chunk of 64, has a version that goes up with every change; a copy
        remembers the versions it has seen, so catching up skips the chunks nothing changed in.
    */
    class ParameterMirror
    {
    public:
        static constexpr int chunkSize = 64;

        /** Message thread, while nothing else uses it. Only the parameters a host may automate are
            copied: a synth's hidden ones (JUCE's stand-ins for MIDI controllers and pitch bend,
            say) follow the MIDI each copy is sent, which is its own. */
        void setUp (const juce::Array<juce::AudioProcessorParameter*>&, const juce::AudioProcessorParameter* bypass);
        int size() const noexcept                           { return numParameters; }

        /** Any thread, never blocks. */
        void set (int index, float value) noexcept;

        /** A parameter that stands for a performance controller (Serum 1's "Pitch Bend" and "Mod
            Wheel"), or one the synth maps a MIDI controller or the pitch bend to (setMidiMapped):
            it follows the notes the synth is sent, so it's not handed on when the synth reports it
            moved, to the other copies or to the host. Host automation of it still reaches every copy.
            Any thread. */
        bool isPerformanceControl (int index) const noexcept
        {
            return juce::isPositiveAndBelow (index, numParameters)
                && (performance[(size_t) index] || midiMapped[(size_t) index].load (std::memory_order_relaxed));
        }

        /** Which parameters a VST3 synth maps MIDI controllers or the pitch bend to, by their VST3
            IDs (Vst3Support::midiMappedParameters). Message thread; may change while the synth plays,
            as its own MPE switch can change the mapping. */
        void setMidiMapped (const juce::AudioPluginInstance&, const std::vector<uint32_t>& parameterIds);

        /** Goes up with every change: for telling whether any parameter moved. */
        uint32_t getChanges() const noexcept                { return changes.load (std::memory_order_acquire); }

        struct Snapshot
        {
            std::vector<uint32_t> versions;             // every parameter's
            uint32_t changes = 0;                       // getChanges() then
            uint32_t leadPlayed = 0;                    // the changes the first copy had played a block with
        };

        /** Every parameter's version now: taken just before the first copy's state is. Message thread. */
        Snapshot snapshot() const;

        /** The copy was given state taken right after `snapshot`: changes up to it are in that
            state, later ones are still to be caught up with. So are changes the first copy hadn't
            played a block with yet, which a synth may keep out of its state until it has (a VST3
            or CLAP synth's automation, while it sleeps). Message thread, while the copy isn't played. */
        void markSeen (Follower&, const Snapshot&) const;

        /** The first copy has played a block with the changes made up to getChanges() == changesBefore.
            The thread playing it. */
        void leadPlayed (uint32_t changesBefore) noexcept;

        /** Sets on the copy every parameter that changed since it last caught up. Not noexcept: it
            calls into the synth, inside CopySafety::call(), which a C++ exception may have to reach. */
        void catchUp (Follower&, juce::AudioPluginInstance&) const;

    private:
        std::unique_ptr<std::atomic<float>[]> values;
        std::unique_ptr<std::atomic<uint32_t>[]> versions, chunkVersions;
        std::unique_ptr<std::atomic<uint32_t>[]> stamps;    // each parameter's last change, counted in changes
        std::vector<bool> copied;                       // which parameters are copied at all
        std::vector<bool> performance;                  // see isPerformanceControl()
        std::unique_ptr<std::atomic<bool>[]> midiMapped;    // ...and setMidiMapped(): read on the audio thread
        std::atomic<uint32_t> changes { 0 }, lastLeadPlayed { 0 };
        int numParameters = 0, numChunks = 0;
    };

    /** What the audio thread keeps for each copy, the first included (its MIDI is in copyMidi). */
    struct CopyRender
    {
        juce::AudioBuffer<float> audio;                 // sized on the message thread while the copy isn't played
        int outputs = 0;                                // the copy's output channels, this block
        float level = 0.0f;                             // its loudest sample, this block
        double quietSeconds = 0.0;                      // of quiet output with no note held
        bool quiet = true;                              // its last note's sound has died away
        bool asleep = false;                            // not processed until a note comes for it
    };

    /** What every MPE Roll in the process shares: the computer's settings file ("MPE Roll.settings")
        and the list of scanned plugins kept in it. There's one for all instances, so a scan in one
        is seen by the others and none of them saves an older list over it. The list has its own
        lock, so one instance may scan while another reads it.
    */
    struct SharedSettings : private juce::ChangeListener
    {
        SharedSettings();
        ~SharedSettings() override;

        juce::PropertiesFile* getUserSettings() const noexcept      { return settings.get(); }
        void useSettingsFile (const juce::File&);

        juce::KnownPluginList knownPlugins;

    private:
        std::unique_ptr<juce::PropertiesFile> settings;

        void open (const juce::File&, const juce::PropertiesFile::Options&);
        void changeListenerCallback (juce::ChangeBroadcaster*) override;
    };

    juce::SharedResourcePointer<SharedSettings> shared;     // first, so it outlives everything below
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList& knownPlugins;                    // the shared list

    InstanceGuard guard;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    ClapSynthControls* clapControls = nullptr;    // the instance, when it's a CLAP synth
    OutputSettings outputSettings;                      // audio thread
    std::unique_ptr<juce::PluginDescription> description;
    std::unique_ptr<EditorWindow> editorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;

    // loadFromFile checks the file in the scanner on this thread, so a plugin that hangs or shows a
    // dialog there doesn't hold up the message thread, and a failed load is retraced there too. The
    // flag stops either early.
    std::thread checkThread;
    std::shared_ptr<std::atomic<bool>> checkCancelled;

    /** Audio thread: whether the synth has made a sound while notes were held over MPE. */
    struct SilenceWatch
    {
        int heldNotes = 0;
        double graceSeconds = 0.0;                      // of audio since it got its state: it may still be loading
        double silentSeconds = 0.0;                     // of notes held, added up since it was last heard or given its state
        bool finished = false;                          // it was heard, or the warning was given
    };

    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midiPiece;                         // audio thread: a long block's MIDI, one piece at a time
    juce::MidiBuffer noMidi;                            // audio thread: always empty, for ending notes
    PieceHead pieceHead;                                // audio thread: and its transport position
    SilenceWatch silenceWatch;
    std::atomic<bool> silenceWarning { false };
    std::atomic<bool> silentOverMpe { false };         // the warning stands: nothing heard since
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool prepared = false;
    bool loading = false;
    bool findingOutWhy = false;                         // retracing a failed load in the scanner
    int loadGeneration = 0;
    std::shared_ptr<int> lifetime = std::make_shared<int> (0);  // lets async callbacks detect that we're gone

    // A synth from the project that hasn't loaded (yet). Saving keeps it, so a project opened on a
    // machine without that synth - or saved while it's still loading - doesn't forget it.
    std::unique_ptr<juce::XmlElement> unresolvedState;

    // A VST3 synth MPE over MIDI can't reach note by note, played with note expressions instead.
    Vst3NoteExpressions vst3Expressions;               // audio thread, but for what the synth takes
    Vst3Support vst3Support;                            // message thread

    // One copy per note, message thread.
    OutputSettings copySettings;
    std::array<Follower, maxCopies> followers;          // [0] unused: the first copy is `instance`
    ParameterMirror mirror;                             // sized under the guard, with the first copy
    std::shared_ptr<CopyWorkers> workers;               // the process's; swapped under the guard
    int copiesGeneration = 0;                           // a copy loaded for an earlier first copy is let go
    bool loadingCopy = false;
    juce::String copiesProblem;                         // why loading stopped short
    juce::int64 lastCopyMegabytes = 0;                  // memory the last copy took when loaded
    juce::String loadingMarker;                         // this instance's key for "loading copies now"
    juce::String synthLoadingMarker;                    // ...and for "loading the synth now"
    bool retryingLoad = false;                          // load() is the one more try after the scanner found the plugin works
    juce::String loadProblem;                           // why the project's synth wasn't loaded, for the user
    std::vector<std::function<void()>> copiesLoadedCallbacks;

    struct SoundCopy
    {
        juce::MemoryBlock state;                        // the first copy's, being copied to the others
        size_t hash = 0;
        ParameterMirror::Snapshot versions;             // its parameters' versions when it was taken
        CopyRouter::CopySet copies;                     // the copies it's for (each only while it lacks it)
        double lookingSince = 0.0;                      // ms: since choosing the next copy began (0: not yet)
        std::array<double, maxCopies> freeSince {};     // ms: since each was first seen holding no note (0: holding)
        int due = 0;                                    // the copy closed to new notes, given it next (0: none)
        bool closedQuiet = false;                       // it was quiet then: a note given it just before mustn't be cut
        uint32_t blocksAtClose = 0;                     // copyBlocks once it was closed
        uint32_t blocksSeen = 0;                        // copyBlocks at the last look
        double blockSeenAt = 0.0;                       // ms: when that changed
        bool running = false;
        int copied = 0, alreadyHad = 0;
        juce::String reason;                            // for the log
    } soundCopy;

    bool soundChangeReported = false;                   // the first copy said its sound changed
    double reportedAt = 0.0;                            // ms: when it first said so, since the last look
    bool windowJustClosed = false;
    double checkAskedAt = 0.0;                          // when a check was first asked for since the last one ran
    uint32_t soundGeneration = 0;                       // moves on whenever the first copy's sound may have changed
    bool stateChangesByItself = false;                  // its state changes with nothing done to it
    int unexplainedChanges = 0;                         // checks in a row that found it so
    size_t hashAtLastCheck = 0;
    uint32_t changesAtLastCheck = 0;
    std::atomic<uint32_t> wheelMoves { 0 };             // the first copy's own wheel parameters, reported moving (not copied)
    uint32_t wheelMovesAtLastCheck = 0;
    double stateMilliseconds = 0.0;                     // how long its state took to get, last time
    bool nonRealtime = false;
    bool offlineOnly = false;                           // an export's
    juce::String crashNote;                             // which copies crashed, for the user

    // One copy per note, audio thread.
    CopyRouter router;
    std::array<CopyRender, maxCopies> renders;
    std::array<juce::MidiBuffer, maxCopies> copyMidi;   // each copy's MIDI, this block
    OutputSettings copyPlay;                            // what each copy is played with
    CopyRouter::CopySet playedCopies, enteredCopies;
    std::array<int, maxCopies> jobs {};
    int numJobs = 0, jobSamples = 0, jobOffset = 0;     // jobs from jobOffset on go to the workers
    juce::AudioPlayHead* jobHead = nullptr;
    OutputSettings::Mode lastMode = OutputSettings::Mode::mpe;
    std::atomic<bool> leadFresh { true };               // the first copy is new, or was given state
    std::atomic<bool> introduceAll { true };            // every copy starts afresh (after prepare)
    std::atomic<bool> configurationRequested { false };
    std::atomic<bool> windowOpen { false };             // the first copy's window: it doesn't sleep
    std::atomic<int> copiesPlaying { 0 }, copiesAsleep { 0 };
    CopyRouter::CopySet openCopies;                     // audio thread: the played copies new notes may go to

    // A bit per copy each. The message thread closes a copy to new notes before giving it the
    // first copy's sound (copiesDueSound); each block says, as it ends, which copies hold a note
    // (held or sustained: the low half of copiesSeen) and which of them it was told were closed
    // (the high half), and which still sound (copiesSounding).
    std::atomic<uint32_t> copiesDueSound { 0 };
    std::atomic<uint64_t> copiesSeen { 0 };
    std::atomic<uint32_t> copiesSounding { 0 };
    std::atomic<uint32_t> copyBlocksStarted { 0 }, copyBlocks { 0 };   // blocks in copy mode begun, and ended
    static_assert (maxCopies <= 32);
    std::array<std::atomic<unsigned int>, maxCopies> crashCodes {};   // set by the thread that caught a copy's crash
    std::atomic<bool> anyCopyCrashed { false };
    int lastPlaying = 0, lastAsleep = 0;               // message thread: what the status last showed

    /** A transport for copies when the host gives none: some synths crash without one. */
    struct FallbackHead final : public juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override;
    } fallbackHead;

    juce::TimedCallback soundCheck { [this] { checkSound(); } };        // a change to copy over, maybe
    juce::TimedCallback soundCopyStep { [this] { copySoundToNext(); } };
    juce::TimedCallback copiesWatch { [this] { watchCopies(); } };
    juce::TimedCallback vst3Recheck { [this] { refreshVst3Support(); } };   // last: goes first

    void install (std::unique_ptr<juce::AudioPluginInstance> newInstance, const juce::PluginDescription& desc);
    std::unique_ptr<juce::AudioPluginInstance> takeInstance();

    bool copiesWanted() const noexcept;
    int getWantedCopies() const noexcept;
    void updateCopies();
    void loadCopy (int index);
    void finishLoadingCopy (int index, std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error,
                            const juce::MemoryBlock& state, const ParameterMirror::Snapshot& versions,
                            uint32_t soundGenerationThen, juce::int64 megabytesBefore, double started);
    void removeCopy (int index);
    void removeAllCopies();
    void updateWorkers();
    bool copiesSettled() const;
    void settleCopies();
    void notifyCopiesChanged();
    void copyCrashed (int index, unsigned int exceptionCode, const juce::String& doing);
    void soundMayHaveChanged();
    void askForSoundCheck (int delayMs);
    void keepLookingWhileOpen();
    void forgetSoundChecks();
    void startCopyingSound (juce::MemoryBlock state, ParameterMirror::Snapshot versions, CopyRouter::CopySet copies,
                            bool evenIfInStep, const juce::String& reason);
    void checkSound();
    void copySoundToNext();
    void giveDueCopyTheSound();
    void leaveSoundCopy (int copy);
    void endSoundCopy();
    void watchCopies();
    bool copiesBlockedAfterCrash();
    void setLoadingMarker (bool loading);
    juce::String getCopyIdentity() const;

    void processCopies (juce::AudioBuffer<float>&, juce::MidiBuffer&, juce::AudioPlayHead*, bool justStarted,
                        const juce::MidiBuffer* leadEndings);
    void processCopiesPiece (juce::AudioBuffer<float>&, const juce::MidiBuffer&, juce::AudioPlayHead*);
    void renderCopy (int copy);
    static void renderJob (void* context, int index) noexcept;
    void finishLoadFromFile (const juce::File&, PluginScanner::Outcome, juce::OwnedArray<juce::PluginDescription>& found,
                             LoadCallback, const std::function<void()>& onChecked);
    void explainLoadFailure (const juce::PluginDescription&, const juce::String& error, const LoadDiagnosis::PreFlight&,
                             int generation, bool retry, LoadCallback);
    void setSynthLoadingMarker (const juce::PluginDescription* loading);    // nullptr: done loading
    bool synthLoadBlockedAfterCrash (const juce::PluginDescription&);
    void forgetSynthCrash (const juce::PluginDescription&);
    void stopCheck();
    void configure (juce::AudioPluginInstance& plugin);
    void countHeldNotes (const juce::MidiBuffer&) noexcept;
    void listenForSound (const juce::AudioBuffer<float>&) noexcept;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedSynth)
};

} // namespace mperoll
