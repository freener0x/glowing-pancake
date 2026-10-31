#include "HostedSynth.h"
#include "CopySafety.h"
#include "CrashReport.h"
#include "PluginLog.h"
#include "PluginScanner.h"
#include "ui/WindowPlacement.h"

#include <algorithm>
#include <string_view>
#include <utility>

#if MPEROLL_HOST_CLAP
 #include "clap/ClapHosting.h"
#endif

namespace mperoll
{

namespace
{
    // For crash reports: which synth, in which format, from which file.
    juce::String describe (const juce::PluginDescription& d)
    {
        return d.name + " (" + d.pluginFormatName + ", " + d.fileOrIdentifier + ")";
    }

    // One copy per note.
    constexpr float quietLevel = 1.0e-4f;           // -80 dB: a copy this quiet with no note held has died away
    constexpr double quietBeforeSleep = 1.0;        // seconds of it before the copy stops being processed
    constexpr juce::int64 minimumFreeMegabytes = 1024;
    constexpr int checkAfterReportMs = 400;         // a synth picking presets reports several changes in a row
    constexpr int longestCheckWaitMs = 1000;        // ...but one that never stops reporting is still looked at
    constexpr double longestReportWaitMs = 2000.0;  // ...as is one reported while knobs keep moving
    constexpr int checkWhileOpenMs = 1500;          // while the first copy's window is open...
    constexpr double cheapStateMs = 30.0;           // ...if its state is quick to get
    constexpr int soundStepMs = 20;                 // between copies being given a sound
    constexpr int heldCopyRetryMs = 100;            // a copy holding a note is given it once the note ends
    constexpr double catchUpWaitMs = 300.0;         // for a copy to play the knob moves its sound is compared after
    constexpr double longestTailWaitMs = 10000.0;   // for a free copy whose sound has died away, before one still sounding is given it
    constexpr double hostIdleMs = 500.0;            // with no block played for this long, nothing is routed meanwhile
    constexpr const char* markerPrefix = "copiesLoading.";
    constexpr const char* crashedWhileLoadingKey = "copiesCrashedWhileLoading";
    constexpr const char* synthMarkerPrefix = "synthLoading.";
    constexpr const char* synthCrashedKey = "synthCrashedWhileLoading";

    // Set on the thread playing the first copy's block, which may be a worker.
    thread_local bool playingFirstCopy = false;

    size_t hashOf (const juce::MemoryBlock& block)
    {
        return std::hash<std::string_view> {} (std::string_view (static_cast<const char*> (block.getData()), block.getSize()));
    }

    /** Every copy but the first. */
    CopyRouter::CopySet everyFollower()
    {
        CopyRouter::CopySet copies;
        copies.set();
        copies.reset (0);
        return copies;
    }

    /** Different every time FL (or the standalone app) starts: a loading marker with another one
        was left by a run that closed while loading copies. */
    const juce::String& thisRun()
    {
        static const juce::String token = juce::Uuid().toString();
        return token;
    }

    /** All Notes Off and the sustain pedal up on every channel: for a synth that may hold notes the
        new way of playing it won't end. */
    void addEndOfAllNotes (juce::MidiBuffer& out, int samplePos)
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            out.addEvent (juce::MidiMessage::controllerEvent (channel, 64, 0), samplePos);
            out.addEvent (juce::MidiMessage::allNotesOff (channel), samplePos);
        }
    }

    // Calls into a copy that CopySafety::call() can abandon part way, so nothing in them needs destroying.
    struct ProcessCall
    {
        juce::AudioPluginInstance* plugin;
        juce::AudioBuffer<float>* audio;
        juce::MidiBuffer* midi;
        void (*first) (void*) = nullptr;    // what else calls into the synth first (its parameters catching up)
        void* firstContext = nullptr;
    };

    void callProcess (void* context)
    {
        auto& call = *static_cast<ProcessCall*> (context);

        if (call.first != nullptr)
            call.first (call.firstContext);

        call.plugin->processBlock (*call.audio, *call.midi);
    }

    struct SetStateCall
    {
        juce::AudioPluginInstance* plugin;
        const juce::MemoryBlock* state;
    };

    void callSetState (void* context)
    {
        auto& call = *static_cast<SetStateCall*> (context);
        call.plugin->setStateInformation (call.state->getData(), (int) call.state->getSize());
    }

    struct GetStateCall
    {
        juce::AudioPluginInstance* plugin;
        juce::MemoryBlock* state;
    };

    void callGetState (void* context)
    {
        auto& call = *static_cast<GetStateCall*> (context);
        call.plugin->getStateInformation (*call.state);
    }

    /** A copy that crashed is never called again: the host around it stops calling it on its own
        too (JUCE's timers for a VST3's parameters and a VST2's idling, a CLAP's callbacks). Its
        own code isn't called, only the host's. Any thread: the one that caught the crash, at once,
        and the message thread when the crash is reported. */
    void stopCalling (juce::AudioPluginInstance& plugin) noexcept
    {
        if (auto* vst3 = plugin.getVST3Client())
            vst3->stopCallingPlugin();

        if (auto* vst2 = plugin.getVSTClient())
            vst2->stopCallingPlugin();

       #if MPEROLL_HOST_CLAP
        if (auto* clap = dynamic_cast<ClapSynthControls*> (&plugin))
            clap->quarantine();
       #endif
    }

    juce::String describeFlStudioNativePlugin (const juce::File& file)
    {
        return file.getFileName() + " is one of FL Studio's own plugins. Those come in Image-Line's own format, "
               "which only FL Studio can load, so MPE Roll can't host it. To play it with what you write in MPE Roll, "
               "use Send to FL piano roll instead, which turns the clip into notes for FL's piano roll.";
    }

    /** The scan window's list. Files dropped on it are scanned the way Options > Scan does: on the
        scan's own thread, with its progress window and Cancel button. JUCE's list looks at them right
        there on the message thread, which would hold up FL's window while the scanner waits on a plugin.
    */
    class ScanList final : public juce::PluginListComponent
    {
    public:
        ScanList (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& knownList, const juce::File& deadMansPedal,
                  juce::PropertiesFile* settings)
            : PluginListComponent (fm, knownList, deadMansPedal, settings, true), formats (fm)
        {
        }

    private:
        void filesDropped (const juce::StringArray& files, int, int) override
        {
            if (isScanning())
                return;

            // A scan is of one format at a time, so a drop of mixed formats scans the first one's files.
            for (auto* format : formats.getFormats())
            {
                juce::StringArray matching;

                for (const auto& file : files)
                    if (format->fileMightContainThisPluginType (file))
                        matching.add (file);

                if (! matching.isEmpty())
                {
                    scanFor (*format, matching);
                    return;
                }
            }
        }

        juce::AudioPluginFormatManager& formats;
    };

    /** Whether a synth's parameter is named after a performance controller, the pitch wheel, mod
        wheel or channel pressure, as Serum 1's "Pitch Bend" and "Mod Wheel" are. Only names that
        are nothing else count: "Bend Range" or "Pressure" are a synth's own settings. */
    bool namesPerformanceControl (const juce::String& name)
    {
        auto key = name.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz");

        if (key.startsWith ("midi"))
            key = key.substring (4);

        for (const auto* control : { "pitchbend", "pitchbender", "pitchwheel", "modwheel", "modulationwheel",
                                     "aftertouch", "channelaftertouch", "channelpressure" })
            if (key == control)
                return true;

        return false;
    }
}

class HostedSynth::EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow (juce::AudioPluginInstance& p, const juce::Component* near, std::function<void()> onClose)
        : DocumentWindow (p.getName(), juce::Colour (0xff1e1f22),
                          DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          plugin (p),
          closed (std::move (onClose))
    {
        setUsingNativeTitleBar (true);

        std::unique_ptr<juce::AudioProcessorEditor> editor;

        if (plugin.hasEditor())
            editor.reset (plugin.createEditorAndMakeActive());

        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);

        setResizable (editor->isResizable(), false);
        setContentOwned (editor.release(), true);

        // Plugin windows aren't owned by the host's window, so keep the synth from hiding behind it.
        setAlwaysOnTop (true);

        // On the display MPE Roll is on, with the title bar inside its work area. JUCE would centre
        // it on the main display whatever its size, and a synth taller than the screen then opens
        // with its title bar above the top edge, where it can't be dragged or closed. Placed once
        // with a typical frame, then again with the real one, once the window is on screen.
        setBounds (WindowPlacement::place (getBounds(), WindowPlacement::frameOf (*this),
                                           WindowPlacement::workAreaNear (near), isResizable(), true));
        setVisible (true);
        WindowPlacement::keepOnScreen (*this, isResizable());
    }

    /** Back on the display MPE Roll is on, centred, and in front. */
    void bringOnScreen (const juce::Component* near)
    {
        if (isMinimised())
            setMinimised (false);

        WindowPlacement::showOnDisplayOf (*this, near, isResizable());
        toFront (true);
    }

    void childBoundsChanged (juce::Component* child) override
    {
        DocumentWindow::childBoundsChanged (child);

        // The synth changed its own size (some do with a preset, or a zoom setting): keep the title
        // bar where it can be reached. Not shrunk, as the synth chose that size.
        if (isShowing())
            WindowPlacement::keepOnScreen (*this, false);
    }

    ~EditorWindow() override
    {
        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            plugin.editorBeingDeleted (editor);

        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (closed)
            closed();
    }

private:
    juce::AudioPluginInstance& plugin;
    std::function<void()> closed;
};

//==============================================================================
HostedSynth::SharedSettings::SharedSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "MPE Roll";
    options.folderName = "MPE Roll";
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    open (options.getDefaultFile(), options);

    // Scans look at each plugin file in the scanner process, so one that crashes can't take FL with it.
    knownPlugins.setCustomScanner (std::make_unique<PluginScanner::OutOfProcessScanner>());
    knownPlugins.addChangeListener (this);
}

HostedSynth::SharedSettings::~SharedSettings()
{
    knownPlugins.removeChangeListener (this);

    // The list only announces a change with a message, which never arrives when FL deletes the last
    // MPE Roll in the middle of a scan: write what it holds now, for the settings file to save as it
    // goes. Nothing is written if the list hasn't changed.
    if (auto xml = knownPlugins.createXml())
        settings->setValue ("knownPlugins", xml.get());
}

void HostedSynth::SharedSettings::useSettingsFile (const juce::File& file)
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    open (file, options);
}

void HostedSynth::SharedSettings::open (const juce::File& file, const juce::PropertiesFile::Options& options)
{
    if (settings != nullptr)
        settings->saveIfNeeded();

    settings = std::make_unique<juce::PropertiesFile> (file, options);

    if (auto xml = settings->getXmlValue ("knownPlugins"))
        knownPlugins.recreateFromXml (*xml);
    else
        knownPlugins.clear();
}

void HostedSynth::SharedSettings::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = knownPlugins.createXml())
    {
        settings->setValue ("knownPlugins", xml.get());
        settings->saveIfNeeded();
    }
}

//==============================================================================
HostedSynth::HostedSynth()
    : knownPlugins (shared->knownPlugins)
{
    juce::addDefaultFormatsToManager (formatManager);

   #if MPEROLL_HOST_CLAP
    formatManager.addFormat (std::make_unique<ClapPluginFormat>());
   #endif

    // The router writes to every copy's MIDI, loaded or not: room for a block's worth up front.
    for (auto& m : copyMidi)
        m.ensureSize (1 << 14);

    loadingMarker = markerPrefix + juce::Uuid().toString();
    synthLoadingMarker = synthMarkerPrefix + juce::Uuid().toString();
}

HostedSynth::~HostedSynth()
{
    ++loadGeneration;
    ++copiesGeneration;
    copiesLoadedCallbacks.clear();

    // First of all: the scan window shows this list, saves into these settings and scans with this
    // scanner, so it mustn't outlive them (FL deletes MPE Roll with the window open when the channel
    // goes or another project is opened). Deleting it ends a scan that's running.
    scanWindow.deleteAndZero();
    stopCheck();
    closeEditor();

    if (loadingCopy)
        setLoadingMarker (false);   // let go in an orderly way: no crash to remember

    if (loading)
        setSynthLoadingMarker (nullptr);

    removeAllCopies();
    workers.reset();

    if (instance != nullptr)
        CrashReport::setActivity ("closing the synth " + getName());

    takeInstance().reset();
}

juce::File HostedSynth::getDeadMansPedalFile()
{
    if (auto* settings = getUserSettings())
        return settings->getFile().getSiblingFile ("RecentlyCrashedPluginsList");

    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MPE Roll crashed plugins");
}

void HostedSynth::useSettingsFile (const juce::File& file)
{
    shared->useSettingsFile (file);
}

juce::Array<juce::PluginDescription> HostedSynth::getInstrumentTypes() const
{
    juce::Array<juce::PluginDescription> instruments;

    for (const auto& type : knownPlugins.getTypes())
        if (type.isInstrument)
            instruments.add (type);

    return instruments.isEmpty() ? knownPlugins.getTypes() : instruments;
}

//==============================================================================
void HostedSynth::load (const juce::PluginDescription& desc, LoadCallback onDone)
{
    stopCheck();
    const auto retry = std::exchange (retryingLoad, false);
    loadProblem.clear();

    // Another install of the synth that's loaded (the same plugin from another file, or the same
    // product in its other format): let the current one go first, so two copies of one plugin's code
    // never meet in this program. They can trip over each other (a library both load by name, say): a
    // user's second install of Analog Lab failed to load beside the first, and its VST3 took FL down.
    if (! retry && instance != nullptr && description != nullptr
        && description->fileOrIdentifier != desc.fileOrIdentifier && isSameProduct (*description, desc))
    {
        PluginLog::add ("Unloading " + PluginLog::describe (*description) + " first: another install of it is about to be loaded");
        unload();
    }

    const auto generation = ++loadGeneration;
    loading = true;
    findingOutWhy = false;
    CrashReport::setActivity ("loading the synth " + describe (desc));
    PluginLog::add ((retry ? "Loading again " : "Loading ") + PluginLog::describe (desc) + " at " + juce::String (sampleRate)
                    + " Hz, " + juce::String (blockSize) + " samples");
    const auto started = juce::Time::getMillisecondCounterHiRes();

    // Asked for by name, so a crash recorded against it (FL closed while it was loading last time) no
    // longer keeps a project from loading it. A marker says it's loading now, so that if FL closes
    // meanwhile the next run knows (synthLoadBlockedAfterCrash).
    forgetSynthCrash (desc);
    setSynthLoadingMarker (&desc);

    // The plugin's binaries, loaded here just before JUCE loads them, so that if JUCE's load fails
    // the system's reason is known (JUCE keeps it to itself). Held until JUCE has answered.
    auto preFlight = std::make_shared<LoadDiagnosis::PreFlight> (desc);

    formatManager.createPluginInstanceAsync (desc, sampleRate, blockSize,
        [this, generation, desc, onDone, started, preFlight, retry, alive = std::weak_ptr<int> (lifetime)]
        (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& error)
        {
            // Another load began meanwhile (or MPE Roll closed), so this answer is of no use; the
            // marker is the newer load's by now.
            if (alive.expired() || generation != loadGeneration)
            {
                PluginLog::add ("  " + desc.name + (plugin != nullptr ? " loaded" : " failed to load")
                                + " after another load had begun, or MPE Roll had closed: not used");
                return;
            }

            loading = false;
            const auto took = juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)) + " ms";

            if (plugin == nullptr)
            {
                setSynthLoadingMarker (nullptr);
                CrashReport::setActivity ("failed to load the synth " + describe (desc));
                const auto message = error.isNotEmpty() ? error : juce::String ("The plugin couldn't be loaded.");
                PluginLog::add ("  failed after " + took + ": " + message);
                explainLoadFailure (desc, message, *preFlight, generation, retry, onDone);
                return;
            }

            CrashReport::setActivity ("setting up the synth " + describe (desc));
            install (std::move (plugin), desc);
            setSynthLoadingMarker (nullptr);    // set up and all: a crash in there counts as one while loading
            PluginLog::add ("  loaded in " + took + ": " + describeSetup());
            CrashReport::renew();
            CrashReport::setActivity ("playing the synth " + describe (desc));

            if (onDone)
                onDone ({});
        });
}

void HostedSynth::explainLoadFailure (const juce::PluginDescription& desc, const juce::String& error,
                                      const LoadDiagnosis::PreFlight& preFlight, int generation, bool retry, LoadCallback onDone)
{
    const auto finish = [onDone] (const juce::String& message)
    {
        if (onDone)
            onDone (message);
    };

    if (! (desc.pluginFormatName == "VST3" || desc.pluginFormatName == "VST"))
        return finish (error);

    PluginLog::add ("  " + LoadDiagnosis::describe (desc));

    for (const auto& line : LoadDiagnosis::inspectFiles (desc))
        PluginLog::add ("  " + line);

    for (const auto& line : preFlight.getLines())
        PluginLog::add ("  " + line);

    // The system's own reason: nothing more to find out.
    if (preFlight.getProblem().isNotEmpty())
        return finish (error + "\n\n" + preFlight.getProblem());

    if (PluginScanner::findExecutable() == juce::File())
        return finish (error);

    // Retraced step by step in the scanner process, where the plugin may crash or hang without
    // taking FL down, on the check thread so FL's window stays responsive meanwhile.
    PluginLog::add ("  Retracing the load in the scanner...");
    loading = findingOutWhy = true;
    stopCheck();    // load() stopped any check already; this only makes sure
    checkCancelled = std::make_shared<std::atomic<bool>> (false);

    checkThread = std::thread ([this, desc, error, generation, retry, onDone, cancelled = checkCancelled, alive = std::weak_ptr<int> (lifetime)]
    {
        auto diagnosis = std::make_shared<PluginScanner::Diagnosis> (
            PluginScanner::diagnose (desc, PluginScanner::diagnoseTimeoutMs, [&cancelled] { return cancelled->load(); }));

        juce::MessageManager::callAsync ([this, desc, error, generation, retry, onDone, diagnosis, alive]
        {
            if (alive.expired() || generation != loadGeneration)
                return;

            loading = findingOutWhy = false;

            for (const auto& line : diagnosis->lines)
                PluginLog::add ("    " + line);

            using Outcome = PluginScanner::Outcome;
            const auto outcome = diagnosis->outcome;
            auto why = LoadDiagnosis::summarise (diagnosis->lines, outcome == Outcome::crashed, outcome == Outcome::timedOut, desc.name);

            // The load had failed already; an outdated scanner only means there's no saying why.
            if (outcome == Outcome::outdated)
            {
                why = PluginScanner::describeOutdatedScanner ("MPE Roll couldn't find out why " + desc.name + " didn't load.");
                PluginLog::add ("  The scanner couldn't retrace it: " + why);
            }
            else if (outcome == Outcome::unavailable)
            {
                PluginLog::add ("  The scanner couldn't retrace it: it couldn't be started, or stopped before it began");
            }
            else if (LoadDiagnosis::worksElsewhere (diagnosis->lines))
            {
                // The plugin works on its own, so it may only have been waiting for something the first
                // time (a licence check, its maker's app): one more try here, before the user hears of it.
                // A user's Analog Lab failed once and loaded the second time.
                if (! retry)
                {
                    PluginLog::add ("  Loading it again, since it works in a separate process");
                    retryingLoad = true;
                    load (desc, onDone);
                    return;
                }

                why << " MPE Roll loaded it again, in case it had been waiting for something the first time (a licence check, "
                       "its maker's app), and it failed again, so what stops it is something in the program MPE Roll runs in "
                       "(FL Studio, say): another copy of a library it uses, loaded there already by that program, by another "
                       "plugin, or by a second install of this one. Restart the program and load it before other plugins, or "
                       "try its other format (VST2 or VST3) meanwhile, and please send the details.";
            }

            if (onDone)
                onDone (why.isNotEmpty() ? error + "\n\n" + why : error);
        });
    });
}

void HostedSynth::loadFromFile (const juce::File& file, LoadCallback onDone, std::function<void()> onChecked)
{
    stopCheck();
    const auto generation = ++loadGeneration;
    findingOutWhy = false;      // a retrace still running was stopped just now
    const auto path = file.getFullPathName();
    CrashReport::setActivity ("reading the plugin file " + path);

    juce::StringArray formatNames;

    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (path))
            formatNames.add (format->getName());

    // Without the scanner, the file is looked at here, as before.
    if (formatNames.isEmpty() || PluginScanner::findExecutable() == juce::File())
    {
        juce::OwnedArray<juce::PluginDescription> found;
        finishLoadFromFile (file, PluginScanner::Outcome::unavailable, found, std::move (onDone), onChecked);
        return;
    }

    // Look inside it in the scanner process first: a plugin that crashes there isn't loaded here.
    // On a thread of its own, since a plugin that hangs or shows a dialog there would otherwise
    // hold up FL's window for the length of the timeout. The outcome comes back to the message
    // thread, where the loading itself happens; a load started meanwhile makes it moot.
    loading = true;
    checkCancelled = std::make_shared<std::atomic<bool>> (false);

    checkThread = std::thread ([this, file, path, formatNames, generation, onDone, onChecked,
                                cancelled = checkCancelled, alive = std::weak_ptr<int> (lifetime)]
    {
        auto found = std::make_shared<juce::OwnedArray<juce::PluginDescription>>();
        auto outcome = PluginScanner::Outcome::found;

        for (const auto& formatName : formatNames)
        {
            outcome = PluginScanner::findTypes (formatName, path, *found, PluginScanner::defaultTimeoutMs,
                                                [&cancelled] { return cancelled->load(); });

            if (outcome != PluginScanner::Outcome::found)
                break;
        }

        juce::MessageManager::callAsync ([this, file, found, outcome, generation, onDone, onChecked, alive]
        {
            if (alive.expired() || generation != loadGeneration)
                return;

            finishLoadFromFile (file, outcome, *found, onDone, onChecked);
        });
    });
}

void HostedSynth::finishLoadFromFile (const juce::File& file, PluginScanner::Outcome outcome,
                                      juce::OwnedArray<juce::PluginDescription>& found, LoadCallback onDone,
                                      const std::function<void()>& onChecked)
{
    loading = false;

    if (outcome == PluginScanner::Outcome::unavailable)
    {
        // No scanner, or one that went down before it got to the file: look inside it here.
        for (auto* format : formatManager.getFormats())
            if (format->fileMightContainThisPluginType (file.getFullPathName()))
                format->findAllTypesForFile (found, file.getFullPathName());
    }
    else if (outcome != PluginScanner::Outcome::found)
    {
        CrashReport::setActivity ("refused to load " + file.getFullPathName());
        const auto message = PluginScanner::describeFailure (outcome, file.getFileName());
        PluginLog::add ("Checking " + file.getFullPathName() + " in the scanner: " + message);

        if (onDone)
            onDone (message);

        return;
    }

    if (found.isEmpty())
    {
        // FL's own plugins have no VST entry point, so a scan finds nothing in them. A .vst3 or
        // .clap with nothing in it is a plugin that wouldn't start (JUCE leaves out a VST3 whose
        // component won't initialise), typically one that isn't authorised yet.
        const auto message = isFlStudioNativePlugin (file) ? describeFlStudioNativePlugin (file)
                           : PluginScanner::mightRefuseToStart (file.getFullPathName())
                                 ? PluginScanner::describeRefusedToStart (juce::StringArray (file.getFileName()), false)
                                 : "No plugin was found in " + file.getFileName() + ".";
        PluginLog::add ("Checking " + file.getFullPathName() + ": " + message);

        if (onDone)
            onDone (message);

        return;
    }

    PluginLog::add ("Checked " + file.getFullPathName()
                    + (outcome == PluginScanner::Outcome::unavailable ? " here, without the scanner: " : " in the scanner: ")
                    + juce::String (found.size()) + (found.size() == 1 ? " plugin" : " plugins"));

    auto* chosen = found.getFirst();

    for (auto* d : found)
    {
        if (d->isInstrument)
        {
            chosen = d;
            break;
        }
    }

    // The file was read afresh, so what it holds now replaces what was listed for it: an update can
    // change a plugin's ID, leaving a listing that no longer loads beside the new one.
    for (const auto& known : knownPlugins.getTypes())
    {
        const auto fromThisFile = std::any_of (found.begin(), found.end(), [&known] (const auto* d)
        {
            return d->pluginFormatName == known.pluginFormatName && d->fileOrIdentifier == known.fileOrIdentifier;
        });

        const auto stillThere = std::any_of (found.begin(), found.end(), [&known] (const auto* d) { return d->isDuplicateOf (known); });

        if (fromThisFile && ! stillThere)
            knownPlugins.removeType (known);
    }

    // A file can hold several plugins (a CLAP bundle, a VST2 shell): list them all in the synth
    // menu, so the others can be picked from there.
    for (auto* d : found)
        knownPlugins.addType (*d);

    if (onChecked)
        onChecked();

    load (*chosen, std::move (onDone));
}

void HostedSynth::stopCheck()
{
    if (checkCancelled != nullptr)
        *checkCancelled = true;

    // Quick: the check notices the flag within moments and stops its scanner.
    if (checkThread.joinable())
        checkThread.join();

    checkCancelled.reset();
}

bool HostedSynth::isFlStudioNativePlugin (const juce::File& file)
{
    return PluginScanner::isFlStudioNativePluginFile (file.getFullPathName());
}

void HostedSynth::showScanWindow (juce::Colour background)
{
    if (scanWindow != nullptr)
    {
        scanWindow->toFront (true);
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScanList (formatManager, knownPlugins, getDeadMansPedalFile(), getUserSettings()));
    options.content->setSize (760, 520);
    options.dialogTitle = "MPE Roll: scan for plugins";
    options.dialogBackgroundColour = background;
    options.useNativeTitleBar = true;
    options.resizable = true;
    scanWindow = options.launchAsync();
}

juce::String HostedSynth::getPluginFilePatterns()
{
    juce::StringArray patterns { "*.vst3" };

   #if JUCE_PLUGINHOST_AU && JUCE_MAC
    patterns.add ("*.component");
   #endif

   #if JUCE_PLUGINHOST_VST
    #if JUCE_WINDOWS
     patterns.add ("*.dll");
    #elif JUCE_MAC
     patterns.add ("*.vst");
    #else
     patterns.add ("*.so");
    #endif
   #endif

   #if MPEROLL_HOST_CLAP
    patterns.add ("*.clap");
   #endif

    return patterns.joinIntoString (";");
}

bool HostedSynth::isSameSynth (const juce::PluginDescription& a, const juce::PluginDescription& b)
{
    if (a.pluginFormatName != b.pluginFormatName)
        return false;

    // The ID, not the file: the same synth installed somewhere else is still the same synth.
    if (a.uniqueId != 0 || b.uniqueId != 0)
        return a.uniqueId == b.uniqueId;

    if (a.deprecatedUid != 0 || b.deprecatedUid != 0)
        return a.deprecatedUid == b.deprecatedUid;

    return a.isDuplicateOf (b);
}

bool HostedSynth::isSameProduct (const juce::PluginDescription& a, const juce::PluginDescription& b)
{
    const auto same = [] (const juce::String& x, const juce::String& y) { return x.trim().equalsIgnoreCase (y.trim()); };
    return a.name.trim().isNotEmpty() && same (a.name, b.name) && same (a.manufacturerName, b.manufacturerName);
}

juce::Array<juce::PluginDescription> HostedSynth::withDistinctNames (const juce::Array<juce::PluginDescription>& types)
{
    auto shown = types;

    for (auto& d : shown)
    {
        int sameName = 0, sameNameAndFormat = 0;

        for (const auto& other : types)
        {
            if (other.name != d.name)
                continue;

            ++sameName;
            sameNameAndFormat += other.pluginFormatName == d.pluginFormatName ? 1 : 0;
        }

        if (sameName < 2)
            continue;

        d.name << " (" << d.pluginFormatName;

        if (sameNameAndFormat > 1)
            d.name << ", " << juce::File (d.fileOrIdentifier).getParentDirectory().getFullPathName();

        d.name << ")";
    }

    return shown;
}

juce::String HostedSynth::describeHostedFormats()
{
    juce::StringArray formats { "VST3" };

   #if JUCE_PLUGINHOST_VST
    formats.add ("VST2");
   #endif

   #if MPEROLL_HOST_CLAP
    formats.add ("CLAP");
   #endif

   #if JUCE_PLUGINHOST_AU && JUCE_MAC
    formats.add ("AU");
   #endif

    if (formats.size() == 1)
        return formats[0];

    const auto last = formats[formats.size() - 1];
    formats.remove (formats.size() - 1);
    return formats.joinIntoString (", ") + " or " + last;
}

void HostedSynth::configure (juce::AudioPluginInstance& plugin)
{
    plugin.disableNonMainBuses();

    auto layout = plugin.getBusesLayout();

    if (! layout.outputBuses.isEmpty() && layout.outputBuses[0] != juce::AudioChannelSet::stereo())
    {
        auto stereo = layout;
        stereo.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

        // A synth may refuse; it then plays in its own layout, its first outputs taken as left and right.
        if (! plugin.setBusesLayout (stereo))
            PluginLog::add ("  " + plugin.getName() + " kept its own output layout ("
                            + plugin.getBusesLayout().outputBuses[0].getDescription() + ") rather than stereo");
    }

    plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);

    if (prepared)
        plugin.prepareToPlay (sampleRate, blockSize);
}

void HostedSynth::install (std::unique_ptr<juce::AudioPluginInstance> newInstance, const juce::PluginDescription& desc)
{
    closeEditor();

    // The old synth's copies go first; the new one's are loaded once it has whatever state it's
    // to have (a project gives it right after this).
    removeAllCopies();
    ++copiesGeneration;
    copiesProblem.clear();
    crashNote.clear();
    lastCopyMegabytes = 0;      // another synth's copies may take more memory or less
    forgetSoundChecks();        // the old synth's, the one its window just closing asked for included

    configure (*newInstance);

    const auto channels = juce::jmax (2, newInstance->getTotalNumInputChannels(), newInstance->getTotalNumOutputChannels());
    auto old = takeInstance();

    {
        const ScopedExclusive exclusive (guard);
        scratch.setSize (juce::jmax (channels, scratch.getNumChannels()), juce::jmax (blockSize, scratch.getNumSamples()));
        renders[0].audio.setSize (juce::jmax (channels, 8), blockSize);
        instance = std::move (newInstance);
        silenceWatch = {};
        silenceWarning = false;
        silentOverMpe = false;
        vst3Expressions.reset();
        mirror.setUp (instance->getParameters(), instance->getBypassParameter());
        leadFresh = true;

       #if MPEROLL_HOST_CLAP
        clapControls = dynamic_cast<ClapSynthControls*> (instance.get());
       #endif
    }

    instance->addListener (this);
    const auto oldName = description != nullptr ? describe (*description) : juce::String();
    description = std::make_unique<juce::PluginDescription> (desc);
    unresolvedState.reset();

    // A VST3 synth is asked what it takes of MPE now, and again every few seconds: its own MPE
    // switch may change it, and JUCE doesn't pass that on.
    vst3Support = {};
    refreshVst3Support();

    if (vst3Support.isVst3)
        vst3Recheck.startTimer (3000);
    else
        vst3Recheck.stopTimer();

    if (old != nullptr)
    {
        CrashReport::setActivity ("closing the synth " + oldName + " to make way for " + describe (desc));
        old->releaseResources();
        old.reset();
        CrashReport::setActivity ("setting up the synth " + describe (desc));
    }

    if (onPluginChanged)
        onPluginChanged();

    juce::MessageManager::callAsync ([this, generation = copiesGeneration, alive = std::weak_ptr<int> (lifetime)]
    {
        if (! alive.expired() && generation == copiesGeneration)
            updateCopies();
    });
}

void HostedSynth::unload()
{
    stopCheck();
    ++loadGeneration;

    if (loading)
        setSynthLoadingMarker (nullptr);    // let go in an orderly way: no crash to remember

    loading = findingOutWhy = false;
    loadProblem.clear();
    closeEditor();
    removeAllCopies();
    ++copiesGeneration;
    copiesProblem.clear();
    crashNote.clear();
    lastCopyMegabytes = 0;
    forgetSoundChecks();

    auto old = takeInstance();

    if (description != nullptr)
        CrashReport::setActivity ("closing the synth " + describe (*description));

    description.reset();
    unresolvedState.reset();

    if (old != nullptr)
        old->releaseResources();

    old.reset();
    CrashReport::setActivity ("no synth loaded");

    if (onPluginChanged)
        onPluginChanged();

    updateCopies();     // nothing to load: stops the workers, and answers whoever waits for copies
}

std::unique_ptr<juce::AudioPluginInstance> HostedSynth::takeInstance()
{
    if (instance == nullptr)
        return {};

    if (onPluginRemoving)
        onPluginRemoving (*instance);

    instance->removeListener (this);
    vst3Recheck.stopTimer();
    vst3Support = {};
    vst3Expressions.setSupport (vst3Support);
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const ScopedExclusive exclusive (guard);
        old = std::move (instance);
        clapControls = nullptr;
    }

    return old;
}

void HostedSynth::refreshVst3Support()
{
    if (instance == nullptr)
        return;

    const auto support = Vst3Support::probe (*instance);

    if (support == vst3Support)
        return;

    // Only what decides how the synth is played (not, say, which channels it maps CC74 on) is worth
    // stopping and resending everything for, and only for a synth that was already playing.
    const auto playedTheSameWay = support.midiMissesNotes() == vst3Support.midiMissesNotes()
                               && support.takesTuning() == vst3Support.takesTuning()
                               && support.typeIds == vst3Support.typeIds
                               && support.scaled == vst3Support.scaled;
    const auto changedWhilePlaying = vst3Support.isVst3 && ! playedTheSameWay;
    vst3Support = support;
    vst3Expressions.setSupport (support);

    // The parameters its MIDI mapping names follow the MIDI it's sent (each channel's pitch bend, in
    // Analog Lab and Kontakt): a change in one isn't a knob tweak for the host's undo history or
    // automation, nor something the other copies are given.
    mirror.setMidiMapped (*instance, support.midiMappedParameters);

    if (support.isVst3)
        PluginLog::add ("  " + getName() + ", " + support.describe()
                        + (support.wantsNoteExpressions() && Vst3NoteExpressions::isAvailable()
                               ? "; with note expressions on, MPE goes to it as VST3 note expressions" : ""));

    // Played the other way now: what MPE Roll has sent the synth so far (zone values...) went the
    // old way, so it's sent again.
    if (changedWhilePlaying && onPlayingChanged)
        onPlayingChanged();
}

void HostedSynth::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details)
{
    // Plugins report these from any thread.
    juce::MessageManager::callAsync ([this, details, alive = std::weak_ptr<int> (lifetime)]
    {
        if (alive.expired())
            return;

        // A preset picked in its window, say: the copies are given its sound, once it settles.
        if (details.programChanged || details.nonParameterStateChanged)
            soundMayHaveChanged();

        if (onSynthReportedChange)
            onSynthReportedChange (details);
    });
}

bool HostedSynth::isLoaded() const
{
    return instance != nullptr;
}

juce::String HostedSynth::getName() const
{
    return instance != nullptr ? instance->getName() : juce::String();
}

juce::String HostedSynth::getMissingSynthName() const
{
    if (instance != nullptr || unresolvedState == nullptr)
        return {};

    if (const auto* descXml = unresolvedState->getChildByName ("PLUGIN"))
        return descXml->getStringAttribute ("name");

    return {};
}

void HostedSynth::showEditor (const juce::Component* near)
{
    if (instance == nullptr)
        return;

    if (editorWindow != nullptr)
    {
        editorWindow->toFront (true);
        WindowPlacement::keepOnScreen (*editorWindow, false);
        return;
    }

    CrashReport::setActivity ("opening the window of the synth " + getName());

    editorWindow = std::make_unique<EditorWindow> (*instance, near, [this, alive = std::weak_ptr<int> (lifetime)]
    {
        // Deleting the window from inside its own callback isn't safe, so do it right after.
        juce::MessageManager::callAsync ([this, alive]
        {
            if (! alive.expired())
                closeEditor();
        });
    });

    // The first copy keeps playing while its window is open (its own keyboard may be clicked), and
    // what's changed in it is copied to the others every few seconds.
    windowOpen = true;
    keepLookingWhileOpen();

    CrashReport::setActivity ("playing the synth " + getName() + ", with its window open");
}

void HostedSynth::bringEditorOnScreen (const juce::Component* near)
{
    if (editorWindow != nullptr)
        editorWindow->bringOnScreen (near);
    else
        showEditor (near);
}

void HostedSynth::closeEditor()
{
    if (editorWindow == nullptr)
        return;

    CrashReport::setActivity ("closing the window of the synth " + getName());
    editorWindow.reset();
    windowOpen = false;
    CrashReport::setActivity ("playing the synth " + getName());

    // Done editing the first copy: whatever changed in its window goes to the others now.
    if (copiesWanted() && copySettings.copiesFollow)
    {
        windowJustClosed = true;
        ++soundGeneration;      // a copy loading now took the sound from before
        askForSoundCheck (50);
    }
}

//==============================================================================
void HostedSynth::prepare (double newSampleRate, int maximumBlockSize)
{
    const MessageThreadLock messageThread;
    const ScopedExclusive exclusive (guard);

    const auto wasPrepared = std::exchange (prepared, true);
    sampleRate = newSampleRate;
    blockSize = juce::jmax (1, maximumBlockSize);
    silenceWatch.heldNotes = 0;     // MPE Roll stops its notes when it's prepared

    int channels = 2;

    if (instance != nullptr)
    {
        // A synth that's running is stopped before it hears the new rate or block size: JUCE's
        // VST3 host skips prepareToPlay for an active plugin whose rate and size look unchanged,
        // and VST2 synths may only be given them while suspended.
        if (wasPrepared && (! juce::approximatelyEqual (instance->getSampleRate(), sampleRate)
                              || instance->getBlockSize() != blockSize))
            instance->releaseResources();

        instance->prepareToPlay (sampleRate, blockSize);
        channels = juce::jmax (channels, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
        renders[0].audio.setSize (juce::jmax (channels, 8), blockSize);
    }

    // The copies the same way. The audio thread isn't playing any of them while the guard is held.
    for (int c = 1; c < maxCopies; ++c)
    {
        auto& f = followers[(size_t) c];

        if (f.plugin == nullptr || f.crashed.load())
            continue;

        if (wasPrepared && (! juce::approximatelyEqual (f.plugin->getSampleRate(), sampleRate)
                              || f.plugin->getBlockSize() != blockSize))
            f.plugin->releaseResources();

        f.plugin->prepareToPlay (sampleRate, blockSize);
        renders[(size_t) c].audio.setSize (juce::jmax (8, f.plugin->getTotalNumInputChannels(), f.plugin->getTotalNumOutputChannels()),
                                           blockSize);
    }

    scratch.setSize (juce::jmax (channels, 8), blockSize);
    midiPiece.ensureSize (1 << 14);
    vst3Expressions.prepare (blockSize);
    vst3Expressions.forgetNotes();      // MPE Roll stops its notes when it's prepared

    // Every copy starts afresh, its notes stopped like MPE Roll's.
    router.reset();
    playedCopies.reset();
    introduceAll = true;

    for (auto& r : renders)
    {
        r.quietSeconds = 0.0;
        r.quiet = true;
        r.asleep = false;
    }
}

void HostedSynth::release()
{
    const MessageThreadLock messageThread;
    const ScopedExclusive exclusive (guard);
    prepared = false;

    if (instance != nullptr)
        instance->releaseResources();

    for (int c = 1; c < maxCopies; ++c)
        if (auto& f = followers[(size_t) c]; f.plugin != nullptr && ! f.crashed.load())
            f.plugin->releaseResources();
}

void HostedSynth::process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead)
{
    const auto numSamples = buffer.getNumSamples();
    const ScopedUse use (guard);

    if (! use.entered || instance == nullptr || ! prepared)
    {
        buffer.clear();
        return;
    }

    // Into copy mode or out of it, the synth may hold notes the new way of playing it won't end
    // (Plain MIDI notes from its time as a copy, or MPE ones from before): all of them end.
    const auto copyMode = outputSettings.mode == OutputSettings::Mode::copies;
    const auto modeChanged = outputSettings.mode != lastMode
                          && (copyMode || lastMode == OutputSettings::Mode::copies);
    lastMode = outputSettings.mode;

    // Into copy mode from VST3 note expressions: the notes played that way end by their IDs, on
    // channel 1 where they were (the processor's note-offs are on the MPE channels).
    const juce::MidiBuffer* leadEndings = nullptr;

    if (modeChanged && copyMode)
        leadEndings = &vst3Expressions.process (noMidi, buffer.getNumSamples(), outputSettings);

    if (modeChanged)
    {
        silenceWatch.heldNotes = 0;
        vst3Expressions.forgetNotes();
    }

    if (copyMode)
    {
        processCopies (buffer, midi, playHead, modeChanged, leadEndings);
        return;
    }

    if (modeChanged)
    {
        midiPiece.clear();

        // Out of copy mode: the notes the synth played as the first copy end as they started, on
        // its channel (a VST3 synth only hears All Notes Off if it maps that controller).
        for (auto& m : copyMidi)
            m.clear();

        router.stopAll (copyMidi.data(), 0);
        midiPiece.addEvents (copyMidi[0], 0, -1, 0);

        for (auto& m : copyMidi)
            m.clear();

        addEndOfAllNotes (midiPiece, 0);
        midiPiece.addEvents (midi, 0, -1, 0);
        midi.swapWith (midiPiece);
    }

    auto& plugin = *instance;
    const auto outputs = plugin.getTotalNumOutputChannels();
    const auto needed = juce::jmax (plugin.getTotalNumInputChannels(), outputs);
    const auto pieceSize = scratch.getNumSamples();

    if (needed > scratch.getNumChannels() || pieceSize <= 0 || plugin.isSuspended())
    {
        buffer.clear();
        return;
    }

   #if MPEROLL_HOST_CLAP
    if (clapControls != nullptr)
        clapControls->setMpeOutputSettings (outputSettings);
   #endif

    countHeldNotes (midi);

    // Hosts may send a longer block than prepare() said. The synth gets it in pieces it was
    // prepared for, so the block (and its note-offs) isn't lost. Each piece's transport position
    // is moved on to where that piece starts.
    for (int start = 0; start < numSamples; start += pieceSize)
    {
        const auto length = juce::jmin (pieceSize, numSamples - start);
        auto* pieceMidi = &midi;

        if (length < numSamples)
        {
            midiPiece.clear();
            midiPiece.addEvents (midi, start, length, -start);
            pieceMidi = &midiPiece;
        }

        pieceHead.source = playHead;
        pieceHead.offset = start;
        pieceHead.sampleRate = sampleRate;
        plugin.setPlayHead (start == 0 || playHead == nullptr ? playHead : &pieceHead);

        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), needed, length);
        view.clear();
        plugin.processBlock (view, vst3Expressions.process (*pieceMidi, length, outputSettings));

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            if (outputs > 0)
                buffer.copyFrom (ch, start, view, juce::jmin (ch, outputs - 1), 0, length);
            else
                buffer.clear (ch, start, length);
        }
    }

    plugin.setPlayHead (playHead);
    listenForSound (buffer);
}

void HostedSynth::countHeldNotes (const juce::MidiBuffer& midi) noexcept
{
    // Counted even once the watch is finished: a project's state starts it again with the notes
    // held at that moment.
    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();

        if (msg.isNoteOn())
            ++silenceWatch.heldNotes;
        else if (msg.isNoteOff())
            silenceWatch.heldNotes = juce::jmax (0, silenceWatch.heldNotes - 1);
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            silenceWatch.heldNotes = 0;
    }
}

void HostedSynth::listenForSound (const juce::AudioBuffer<float>& output) noexcept
{
    constexpr float silent = 1.0e-3f;               // -60 dB: a noise floor or hum isn't the synth sounding
    constexpr double secondsBeforeWarning = 3.0;    // of notes held in all, so a slow attack isn't taken for silence
    constexpr double graceAfterLoad = 10.0;         // seconds after it got its state: a sample library may be loading

    auto& watch = silenceWatch;
    const auto seconds = output.getNumSamples() / sampleRate;

    // A sound settles it, and withdraws a warning that hasn't been shown yet: the user may have
    // turned on MPE in the synth's own window while MPE Roll's was closed.
    if (output.getMagnitude (0, output.getNumSamples()) > silent)
    {
        watch.finished = true;
        silenceWarning = false;
        silentOverMpe = false;
        return;
    }

    if (watch.finished)
        return;

    if (watch.graceSeconds < graceAfterLoad)
    {
        watch.graceSeconds += seconds;
        return;
    }

    // Only MPE over MIDI spreads notes over channels a synth may not be listening to. VST3 note
    // expressions put every note on channel 1.
    auto overMpe = outputSettings.mode == OutputSettings::Mode::mpe && ! vst3Expressions.isUsedWith (outputSettings);

   #if MPEROLL_HOST_CLAP
    if (clapControls != nullptr && clapControls->playsNoteExpressions (outputSettings))
        overMpe = false;
   #endif

    if (! overMpe)
    {
        watch.silentSeconds = 0.0;
        return;
    }

    // Silence with no note held says nothing. Short notes add up: a synth that hears none of them
    // is as silent through ten short notes as through one long one.
    if (watch.heldNotes == 0)
        return;

    watch.silentSeconds += seconds;

    if (watch.silentSeconds >= secondsBeforeWarning)
    {
        watch.finished = true;
        silenceWarning = true;
        silentOverMpe = true;
    }
}

juce::Optional<juce::AudioPlayHead::PositionInfo> HostedSynth::PieceHead::getPosition() const
{
    if (source == nullptr)
        return {};

    auto position = source->getPosition();

    if (! position.hasValue() || offset == 0 || sampleRate <= 0.0 || ! position->getIsPlaying())
        return position;

    const auto seconds = offset / sampleRate;

    if (const auto samples = position->getTimeInSamples())
        position->setTimeInSamples (*samples + offset);

    if (const auto time = position->getTimeInSeconds())
        position->setTimeInSeconds (*time + seconds);

    if (const auto ppq = position->getPpqPosition())
        if (const auto bpm = position->getBpm())
            position->setPpqPosition (*ppq + seconds * *bpm / 60.0);

    return position;
}

juce::String HostedSynth::describeBendRange (const OutputSettings& settings, const MpeEncoder::BendOverflow& recent) const
{
    // Plain MIDI and one copy per note bend within the synth's own pitch-bend range, which MPE Roll
    // sends (RPN 0) but most synths keep their own. Most start at 2 semitones.
    const auto name = getName();
    const auto range = juce::String (juce::roundToInt (settings.masterBendRange));
    juce::String s;

    s << "Glides bend within " << name << "'s own pitch-bend range: set it to " << range << " semitones, MPE Roll's Zone bend range, "
      << "or both to 12 or 24 for wider glides (a glide past the range stops at its edge).";

    if (const auto profile = getProfile(); profile.has_value() && profile->bendRange.isNotEmpty())
        s << " " << profile->bendRange;

    if (! recent.isEmpty() && std::abs (recent.range - settings.masterBendRange) < 0.01f)
        s << " A glide just reached " << recent.semitonesReached() << " semitones and stopped at " << range
          << ": set both to " << recent.suggestedRange() << ".";

    return s;
}

juce::String HostedSynth::describeConnection (const OutputSettings& settings, const MpeEncoder::BendOverflow& recent) const
{
    if (instance == nullptr)
        return loadProblem.isNotEmpty() ? loadProblem : juce::String ("No synth loaded.");

    // Each copy is played on one channel, whatever else the synth could take.
    if (settings.mode == OutputSettings::Mode::copies)
    {
        const auto status = getCopyStatus();
        const auto name = getName();
        juce::String s;

        s << "One copy of " << name << " per note: " << status.loaded << " of " << status.wanted << " copies loaded";

        if (status.loading)
            s << " (loading the rest)";

        if (status.crashed > 0)
            s << ", " << status.crashed << " switched off after crashing";

        s << "; " << status.playing << " playing, " << status.asleep << " asleep";

        if (status.workers > 0)
            s << ", spread over " << (status.workers + 1) << " CPU cores";

        s << ". Each copy plays its note as Plain MIDI on channel " << settings.singleChannel << ": pitch bend, "
          << (settings.pressureTarget == OutputSettings::PressureTarget::channelPressure ? juce::String ("channel pressure")
              : settings.pressureTarget == OutputSettings::PressureTarget::polyAftertouch ? juce::String ("poly aftertouch")
              : "CC" + juce::String (settings.pressureController))
          << " for pressure and CC" << settings.timbreController << " for timbre. " << describeBendRange (settings, recent);

        // A problem is under Copies now, with the button that deals with it.
        if (status.copyingSound)
            s << " Giving the copies " << name << "'s sound...";

        return s;
    }

    const auto profile = getProfile();

    if (playsNoteExpressions (settings))
    {
        if (clapControls != nullptr)
            return "CLAP notes, each with its own tuning, pressure, brightness, volume, pan, vibrato and expression.";

        return "VST3 notes on channel 1, each with its own " + vst3Support.describeNoteExpressions()
             + ". This VST3 synth takes pitch bend " + vst3Support.describeBend()
             + ", so MPE over MIDI couldn't reach its notes one by one."
             + (profile.has_value() && profile->name == "Serum 2"
                    ? juce::String (" Xfer's guide says Serum 2 takes note expressions with its own MPE switch off "
                                    "(MENU > MPE Enabled).")
                    : juce::String());
    }

    const auto via = clapControls != nullptr ? juce::String (" (the CLAP synth accepts MIDI)") : juce::String();

    if (settings.mode == OutputSettings::Mode::singleChannel)
        return "Plain MIDI on channel " + juce::String (settings.singleChannel) + via
             + ": the newest note's glides, pressure and timbre drive the synth. " + describeBendRange (settings, recent);

    // A synth that isn't in MPE mode often listens on channel 1 only, and hears nothing.
    const auto channels = settings.zones.describeMemberChannels();
    const auto mpe = "MPE over MIDI" + via + (channels.isNotEmpty() ? ", notes on channels " + channels : juce::String()) + ". ";

    if (profile.has_value() && profile->mpe == SynthProfile::Mpe::none)
        return mpe + profile->advice;

    // JUCE's VST3 host drops MIDI pitch bend and pressure on channels the synth maps nothing to.
    if (vst3Support.midiMissesNotes())
        return mpe + "But this VST3 synth takes pitch bend " + vst3Support.describeBend()
             + ", so each note's glides can't reach it one by one"
             + (vst3Support.takesTuning() && Vst3NoteExpressions::isAvailable()
                    ? juce::String (" (turn on Note expressions to play it with VST3 note expressions)") : juce::String())
             + ". Set Synth expects to Plain MIDI, or load its "
             + (Vst3NoteExpressions::isAvailable() ? "VST2 or CLAP" : "VST2") + " version if you have one.";

    if (profile.has_value())
        return mpe + profile->advice;

    return mpe + "Turn on MPE in the synth, or it may not hear them.";
}

std::optional<SynthProfile> HostedSynth::getProfile() const
{
    // What the loaded plugin says it is (its version, say), not what a scan or a project said.
    return instance != nullptr ? SynthProfile::find (instance->getPluginDescription()) : std::nullopt;
}

juce::String HostedSynth::describeSetup() const
{
    if (instance == nullptr)
        return {};

    // What the log needs to tell why a synth plays wrongly or not at all.
    juce::String s;
    s << instance->getTotalNumInputChannels() << " in / " << instance->getTotalNumOutputChannels() << " out channels in "
      << instance->getBusCount (false) << " output bus" << (instance->getBusCount (false) == 1 ? "" : "es")
      << (instance->acceptsMidi() ? ", takes MIDI" : ", takes no MIDI")
      << (instance->getPluginDescription().isInstrument ? ", an instrument" : ", not marked as an instrument")
      << ", latency " << instance->getLatencySamples() << " samples";
    return s;
}

bool HostedSynth::playsNoteExpressions (const OutputSettings& settings) const
{
    if (instance == nullptr)
        return false;

   #if MPEROLL_HOST_CLAP
    if (clapControls != nullptr)
        return clapControls->playsNoteExpressions (settings);
   #endif

    return vst3Expressions.isUsedWith (settings);
}

int HostedSynth::getLatencySamples() const
{
    const ScopedUse use (guard);
    return use.entered && instance != nullptr ? instance->getLatencySamples() : 0;
}

double HostedSynth::getTailLengthSeconds() const
{
    // Hosts ask for this from any thread.
    const ScopedUse use (guard);
    return use.entered && instance != nullptr ? instance->getTailLengthSeconds() : 0.0;
}

void HostedSynth::setNonRealtime (bool isNonRealtime)
{
    const ScopedExclusive exclusive (guard);
    nonRealtime = isNonRealtime;

    if (instance != nullptr)
        instance->setNonRealtime (isNonRealtime);

    for (int c = 1; c < maxCopies; ++c)
        if (auto& f = followers[(size_t) c]; f.plugin != nullptr && ! f.crashed.load())
            f.plugin->setNonRealtime (isNonRealtime);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> HostedSynth::saveState() const
{
    if (instance == nullptr || description == nullptr)
        return unresolvedState != nullptr ? std::make_unique<juce::XmlElement> (*unresolvedState) : nullptr;

    auto xml = std::make_unique<juce::XmlElement> ("HostedSynth");
    xml->addChildElement (description->createXml().release());

    juce::MemoryBlock state;
    instance->getStateInformation (state);
    xml->createNewChildElement ("State")->addTextElement (state.toBase64Encoding());
    return xml;
}

void HostedSynth::restoreState (const juce::XmlElement& xml, LoadCallback onDone)
{
    juce::PluginDescription desc;
    const auto* descXml = xml.getChildByName ("PLUGIN");

    if (descXml == nullptr || ! desc.loadFromXml (*descXml))
    {
        if (onDone)
            onDone ("The saved synth couldn't be read.");

        return;
    }

    juce::MemoryBlock state;

    if (const auto* stateXml = xml.getChildByName ("State"))
        state.fromBase64Encoding (stateXml->getAllSubText());

    // The same synth is already here (wherever it was installed when saved): just give it the state.
    if (instance != nullptr && description != nullptr && isSameSynth (*description, desc))
    {
        stopCheck();        // a Load plugin file... check still running would otherwise keep its scanner going
        ++loadGeneration;   // a load still in flight would otherwise replace it
        loading = findingOutWhy = false;
        unresolvedState.reset();

        if (state.getSize() > 0)
        {
            instance->setStateInformation (state.getData(), (int) state.getSize());

            {
                // New state can mean new sounds to load: listen afresh, with the grace a new synth gets.
                const ScopedExclusive exclusive (guard);
                silenceWatch.graceSeconds = 0.0;
                silenceWatch.silentSeconds = 0.0;
                silenceWatch.finished = false;
                silenceWarning = false;
                silentOverMpe = false;
            }

            refreshVst3Support();   // its own MPE switch may be in the state

            // The project's sound goes to every copy, and the first one is set up afresh as a copy.
            leadFresh = true;

            if (copiesWanted())
            {
                const auto versions = mirror.snapshot();
                juce::MemoryBlock leadState;
                instance->getStateInformation (leadState);
                startCopyingSound (std::move (leadState), versions, everyFollower(), true, "the project gave the synth its sound");
            }
        }

        if (onPluginChanged)
            onPluginChanged();

        if (onDone)
            onDone ({});

        return;
    }

    // A different synth: let the current one go now. If the new one fails to load, the project
    // then shows it as missing (and keeps it when saved) instead of quietly keeping the old synth.
    unload();
    unresolvedState = std::make_unique<juce::XmlElement> (xml);

    // A project from another computer may name a path that doesn't exist here: use the same synth
    // where it's installed on this one, if a scan found it.
    if (! formatManager.doesPluginStillExist (desc))
    {
        for (const auto& known : knownPlugins.getTypes())
        {
            if (isSameSynth (known, desc) && formatManager.doesPluginStillExist (known))
            {
                desc = known;
                break;
            }
        }
    }

    // A plugin that took FL Studio down while it was loading last time isn't loaded again on its own
    // by every project that names it: the user loads it by name (Load synth), which forgets the crash.
    if (synthLoadBlockedAfterCrash (desc))
    {
        loadProblem = desc.name + " wasn't loaded: FL Studio or MPE Roll closed while it was loading last time, so the project "
                      "opened without it. Load synth > " + desc.name + " loads it again.";
        PluginLog::add (loadProblem);

        if (onPluginChanged)
            onPluginChanged();

        if (onDone)
            onDone (loadProblem);

        return;
    }

    load (desc, [this, state, desc, onDone] (const juce::String& error)
    {
        if (error.isEmpty() && instance != nullptr && state.getSize() > 0)
        {
            instance->setStateInformation (state.getData(), (int) state.getSize());
            refreshVst3Support();   // its own MPE switch may be in the state, and decide how it's played
        }

        if (onDone)
            onDone (error.isEmpty() ? error : desc.name + ": " + error);
    });
}

//==============================================================================
// One copy per note: the first copy's parameter changes.

void HostedSynth::ParameterMirror::setUp (const juce::Array<juce::AudioProcessorParameter*>& parameters,
                                          const juce::AudioProcessorParameter* bypass)
{
    numParameters = parameters.size();
    numChunks = (numParameters + chunkSize - 1) / chunkSize;
    values = std::make_unique<std::atomic<float>[]> ((size_t) numParameters);
    versions = std::make_unique<std::atomic<uint32_t>[]> ((size_t) numParameters);
    chunkVersions = std::make_unique<std::atomic<uint32_t>[]> ((size_t) numChunks);
    stamps = std::make_unique<std::atomic<uint32_t>[]> ((size_t) numParameters);
    midiMapped = std::make_unique<std::atomic<bool>[]> ((size_t) numParameters);
    copied.assign ((size_t) numParameters, false);
    performance.assign ((size_t) numParameters, false);
    lastLeadPlayed = 0;

    for (int i = 0; i < numParameters; ++i)
    {
        auto* p = parameters.getUnchecked (i);
        copied[(size_t) i] = p->isAutomatable() && p != bypass;
        performance[(size_t) i] = namesPerformanceControl (p->getName (64));
        midiMapped[(size_t) i].store (false, std::memory_order_relaxed);
    }
}

void HostedSynth::ParameterMirror::setMidiMapped (const juce::AudioPluginInstance& plugin, const std::vector<uint32_t>& parameterIds)
{
    // JUCE's VST3 parameters give their VST3 ID as their parameter ID string.
    for (int i = 0; i < juce::jmin (numParameters, plugin.getParameters().size()); ++i)
    {
        const auto* hosted = plugin.getHostedParameter (i);
        const auto id = hosted != nullptr ? hosted->getParameterID() : juce::String();
        const auto mapped = ! parameterIds.empty() && id.isNotEmpty() && id.containsOnly ("0123456789")
                         && std::binary_search (parameterIds.begin(), parameterIds.end(), (uint32_t) id.getLargeIntValue());
        midiMapped[(size_t) i].store (mapped, std::memory_order_relaxed);
    }
}

void HostedSynth::ParameterMirror::set (int index, float value) noexcept
{
    if (! juce::isPositiveAndBelow (index, numParameters) || ! copied[(size_t) index])
        return;

    // The value, then the versions that say it's there: catchUp() reads them the other way round.
    // Read-modify-writes, so a copy that sees a version sees every value stored before it.
    values[(size_t) index].store (value, std::memory_order_release);
    versions[(size_t) index].fetch_add (1, std::memory_order_acq_rel);
    chunkVersions[(size_t) (index / chunkSize)].fetch_add (1, std::memory_order_acq_rel);

    // When it changed, counted in changes: never back, though two threads may change it at once.
    const auto stamp = changes.fetch_add (1, std::memory_order_acq_rel) + 1;
    auto& last = stamps[(size_t) index];

    for (auto seen = last.load (std::memory_order_relaxed);
         seen < stamp && ! last.compare_exchange_weak (seen, stamp, std::memory_order_release, std::memory_order_relaxed);)
    {
    }
}

void HostedSynth::ParameterMirror::leadPlayed (uint32_t changesBefore) noexcept
{
    for (auto seen = lastLeadPlayed.load (std::memory_order_relaxed);
         seen < changesBefore
         && ! lastLeadPlayed.compare_exchange_weak (seen, changesBefore, std::memory_order_release, std::memory_order_relaxed);)
    {
    }
}

HostedSynth::ParameterMirror::Snapshot HostedSynth::ParameterMirror::snapshot() const
{
    // What the first copy has played first: what it played may be in the state taken next, what
    // it hasn't, maybe not.
    Snapshot result;
    result.changes = changes.load (std::memory_order_acquire);
    result.leadPlayed = lastLeadPlayed.load (std::memory_order_acquire);
    result.versions.resize ((size_t) numParameters);

    for (int i = 0; i < numParameters; ++i)
        result.versions[(size_t) i] = versions[(size_t) i].load (std::memory_order_acquire);

    return result;
}

void HostedSynth::ParameterMirror::markSeen (Follower& f, const Snapshot& snapshot) const
{
    f.seenParameter.assign ((size_t) numParameters, 0);

    for (int i = 0; i < juce::jmin (numParameters, (int) snapshot.versions.size()); ++i)
    {
        // A change the first copy hadn't played a block with by then is caught up with again:
        // setting a parameter to the value it has already does no harm.
        const auto played = stamps[(size_t) i].load (std::memory_order_acquire) <= snapshot.leadPlayed;
        f.seenParameter[(size_t) i] = snapshot.versions[(size_t) i] - (played ? 0u : 1u);
    }

    // Every chunk is looked through once, for the changes made since the snapshot.
    f.seenChunk.assign ((size_t) numChunks, ~0u);
}

void HostedSynth::ParameterMirror::catchUp (Follower& f, juce::AudioPluginInstance& plugin) const
{
    if ((int) f.seenChunk.size() != numChunks || (int) f.seenParameter.size() != numParameters)
        return;

    const auto& parameters = plugin.getParameters();
    const auto count = juce::jmin (numParameters, parameters.size());

    for (int chunk = 0; chunk < numChunks; ++chunk)
    {
        const auto chunkVersion = chunkVersions[(size_t) chunk].load (std::memory_order_acquire);

        if (chunkVersion == f.seenChunk[(size_t) chunk])
            continue;

        // Taken before looking through it: a change made meanwhile is caught next time.
        f.seenChunk[(size_t) chunk] = chunkVersion;

        for (int i = chunk * chunkSize; i < juce::jmin (count, (chunk + 1) * chunkSize); ++i)
        {
            const auto version = versions[(size_t) i].load (std::memory_order_acquire);

            if (version == f.seenParameter[(size_t) i])
                continue;

            f.seenParameter[(size_t) i] = version;
            parameters.getUnchecked (i)->setValue (values[(size_t) i].load (std::memory_order_acquire));
        }
    }
}

void HostedSynth::audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value)
{
    // A change the first copy reports while it plays its block follows from the MIDI it was sent
    // (a parameter a synth maps a controller or the pitch bend to): that MIDI is its own, not the
    // other copies'. So does its pitch wheel or mod wheel parameter, which a synth may report from
    // its window as the wheel it draws follows the notes: not copied, but counted, so the look at
    // its sound waits for the wheels as for the knobs. Other changes from its window, and its
    // presets, are copied.
    if (playingFirstCopy)
        return;

    if (mirror.isPerformanceControl (index))
        wheelMoves.fetch_add (1, std::memory_order_relaxed);
    else
        mirror.set (index, value);
}

bool HostedSynth::isFollowingItsNotes (int index) const noexcept
{
    return playingFirstCopy || mirror.isPerformanceControl (index);
}

bool HostedSynth::setParameterValue (int index, float value)
{
    return withParameter (index, [this, index, value] (juce::AudioProcessorParameter& p)
    {
        p.setValue (value);
        mirror.set (index, value);
    });
}

//==============================================================================
// One copy per note: loading and letting go of the copies (message thread).

bool HostedSynth::copiesWanted() const noexcept
{
    return copySettings.mode == OutputSettings::Mode::copies && instance != nullptr && description != nullptr;
}

int HostedSynth::getWantedCopies() const noexcept
{
    return juce::jlimit (OutputSettings::minCopies, maxCopies, copySettings.copies);
}

void HostedSynth::setCopySettings (const OutputSettings& requested)
{
    auto settings = requested;

    // An export's copies are loaded with the sound to render, and keep it.
    if (offlineOnly)
        settings.copiesFollow = false;

    const auto changed = settings.mode != copySettings.mode || settings.copies != copySettings.copies
                      || settings.copiesUseCores != copySettings.copiesUseCores
                      || settings.copiesFollow != copySettings.copiesFollow;
    const auto askedForMore = settings.mode == OutputSettings::Mode::copies
                           && (copySettings.mode != OutputSettings::Mode::copies || settings.copies > copySettings.copies);
    copySettings = settings;

    if (! changed)
        return;

    // Asking for copies again, or for more of them, tries again after loading stopped short.
    if (askedForMore)
        copiesProblem.clear();

    if (! settings.copiesFollow)
    {
        soundCheck.stopTimer();
        checkAskedAt = 0.0;
        soundChangeReported = false;
        windowJustClosed = false;
    }

    updateCopies();
}

void HostedSynth::updateCopies()
{
    updateWorkers();

    if (! copiesWanted())
    {
        removeAllCopies();
        copiesWatch.stopTimer();
        soundCheck.stopTimer();
        soundCopyStep.stopTimer();
        endSoundCopy();
        checkAskedAt = 0.0;
        settleCopies();
        notifyCopiesChanged();
        return;
    }

    if (! copiesWatch.isTimerRunning())
        copiesWatch.startTimer (250);

    const auto wanted = getWantedCopies();

    for (int c = maxCopies - 1; c >= wanted; --c)
        if (followers[(size_t) c].state.load() != Follower::State::empty)
            removeCopy (c);

    // One at a time: each copy loads like the synth itself, which can take a while.
    if (! loadingCopy && copiesProblem.isEmpty())
    {
        for (int c = 1; c < wanted; ++c)
        {
            if (followers[(size_t) c].state.load() == Follower::State::empty)
            {
                loadCopy (c);
                return;
            }
        }
    }

    // Copy mode or Follow turned on with the first copy's window already open.
    keepLookingWhileOpen();

    settleCopies();
    notifyCopiesChanged();
}

void HostedSynth::loadCopy (int index)
{
    const auto name = getName();

    if (copiesBlockedAfterCrash())
    {
        copiesProblem = "FL Studio (or MPE Roll) closed while loading copies of " + name + " last time, so they "
                        "aren't loaded on their own now. To try again, press Load the copies again in MPE settings.";
        PluginLog::add ("Not loading copies of " + name + ": a crash cut short loading them last time");
        settleCopies();
        notifyCopiesChanged();
        return;
    }

    // Each copy takes about as much memory as the first: stop while the computer still has some to spare.
    const auto freeMegabytes = CopySafety::getAvailableMemoryMB();
    const auto needed = juce::jmax (minimumFreeMegabytes, 2 * lastCopyMegabytes);

    if (freeMegabytes >= 0 && freeMegabytes < needed)
    {
        const auto loaded = getCopyStatus().loaded;
        copiesProblem = "Stopped at " + juce::String (loaded) + (loaded == 1 ? " copy" : " copies") + ": the computer has only "
                      + juce::String (freeMegabytes) + " MB of memory free"
                      + (lastCopyMegabytes > 0 ? ", and each copy of " + name + " takes about " + juce::String (lastCopyMegabytes) + " MB"
                                               : juce::String())
                      + ". Close other programs, or ask for fewer copies.";
        PluginLog::add (copiesProblem);
        settleCopies();
        notifyCopiesChanged();
        return;
    }

    auto& f = followers[(size_t) index];
    f.state = Follower::State::busy;
    f.crashed = false;
    loadingCopy = true;

    // On disk before the copy is made: if FL closes while it loads, the next run finds this.
    setLoadingMarker (true);

    // The first copy's sound as it is now goes to the new copy as soon as it's loaded.
    const auto versions = mirror.snapshot();
    juce::MemoryBlock state;
    instance->getStateInformation (state);

    const auto before = CopySafety::getProcessMemoryMB();
    const auto started = juce::Time::getMillisecondCounterHiRes();
    CrashReport::setActivity ("loading copy " + juce::String (index + 1) + " of the synth " + describe (*description));

    formatManager.createPluginInstanceAsync (*description, sampleRate, blockSize,
        [this, index, generation = copiesGeneration, sound = soundGeneration, state, versions, before, started,
         alive = std::weak_ptr<int> (lifetime)]
        (std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& error)
        {
            if (alive.expired())
                return;

            loadingCopy = false;

            // The synth changed, or this copy is no longer wanted, while it loaded.
            if (generation != copiesGeneration || ! copiesWanted() || index >= getWantedCopies()
                || followers[(size_t) index].state.load() != Follower::State::busy || followers[(size_t) index].plugin != nullptr)
            {
                setLoadingMarker (false);
                plugin.reset();
                updateCopies();
                return;
            }

            finishLoadingCopy (index, std::move (plugin), error, state, versions, sound, before, started);
        });
}

void HostedSynth::finishLoadingCopy (int index, std::unique_ptr<juce::AudioPluginInstance> plugin, const juce::String& error,
                                     const juce::MemoryBlock& state, const ParameterMirror::Snapshot& versions,
                                     uint32_t soundGenerationThen, juce::int64 megabytesBefore, double started)
{
    auto& f = followers[(size_t) index];
    const auto name = getName();
    const auto copyName = "Copy " + juce::String (index + 1) + " of " + name;

    if (plugin == nullptr)
    {
        f.state = Follower::State::empty;
        setLoadingMarker (false);
        copiesProblem = copyName + " couldn't be loaded" + (error.isNotEmpty() ? ": " + error : juce::String ("."));

        // The first copy loaded from the same file, so the file and its class are fine here: what
        // failed is the plugin making one more of itself.
        if (instance != nullptr)
            copiesProblem << "\n\nThe first copy loaded from the same file, so the plugin works here; it wouldn't make "
                             "another of itself. Some plugins allow only one or a few at a time (by their licence, "
                             "say), and each copy needs its own memory: try fewer copies.";

        PluginLog::add (copiesProblem);
        settleCopies();
        notifyCopiesChanged();
        return;
    }

    // Offline or not before it's prepared: a VST3 synth is told in its setup, as it's prepared.
    CrashReport::setActivity ("setting up copy " + juce::String (index + 1) + " of the synth " + describe (*description));
    plugin->setNonRealtime (nonRealtime);
    configure (*plugin);

    // Its buffer, while the audio thread leaves the copy alone (it isn't ready yet).
    renders[(size_t) index].audio.setSize (juce::jmax (8, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()),
                                           blockSize);

    SetStateCall call { plugin.get(), &state };
    unsigned int code = 0;

    if (state.getSize() > 0 && ! CopySafety::call (callSetState, &call, &code))
    {
        // Whatever it held when it crashed may still be held: it's never called again, not even to be deleted.
        stopCalling (*plugin);
        (void) plugin.release();
        f.crashed = true;
        f.state = Follower::State::ready;
        setLoadingMarker (false);
        copiesProblem = "Copies stopped loading after one crashed.";
        copyCrashed (index, code, "while taking the first copy's sound");
        settleCopies();
        return;
    }

    mirror.markSeen (f, versions);

    {
        const ScopedExclusive exclusive (f.guard);
        f.plugin = std::move (plugin);

       #if MPEROLL_HOST_CLAP
        f.clap = dynamic_cast<ClapSynthControls*> (f.plugin.get());
       #endif

        f.stateHash = hashOf (state);
        f.fresh = true;
    }

    f.state.store (Follower::State::ready, std::memory_order_release);
    setLoadingMarker (false);

    // It took the first copy's sound as it was when it started loading. If that may have changed
    // since (a preset picked, a project opened), it's given the sound as it is now.
    if (soundGenerationThen != soundGeneration)
    {
        if (soundCopy.running)
        {
            soundCopy.copies.set ((size_t) index);
        }
        else
        {
            const auto nowVersions = mirror.snapshot();
            juce::MemoryBlock now;
            instance->getStateInformation (now);
            CopyRouter::CopySet just;
            just.set ((size_t) index);
            startCopyingSound (std::move (now), nowVersions, just, false, "it changed while " + copyName + " loaded");
        }
    }

    if (const auto after = CopySafety::getProcessMemoryMB(); megabytesBefore >= 0 && after >= 0)
        lastCopyMegabytes = juce::jmax ((juce::int64) 0, after - megabytesBefore);

    PluginLog::add ("  " + copyName + " loaded in " + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started))
                    + " ms" + (lastCopyMegabytes > 0 ? ", about " + juce::String (lastCopyMegabytes) + " MB" : juce::String()));
    CrashReport::setActivity ("playing the synth " + describe (*description) + " in copies");
    notifyCopiesChanged();

    // The next one after a breath, so FL's window keeps up (paints wait for an empty message queue).
    juce::Timer::callAfterDelay (50, [this, generation = copiesGeneration, alive = std::weak_ptr<int> (lifetime)]
    {
        if (! alive.expired() && generation == copiesGeneration)
            updateCopies();
    });
}

void HostedSynth::removeCopy (int index)
{
    auto& f = followers[(size_t) index];

    if (f.crashed.load())
    {
        // Whatever it held when it crashed may still be held: it's never called again, not even to be
        // deleted. The block it crashed in may not have ended yet.
        {
            const ScopedExclusive exclusive (f.guard);
            (void) f.plugin.release();
            f.clap = nullptr;
        }

        f.crashed = false;
        f.stateHash = 0;
        crashCodes[(size_t) index] = 0;     // reported or not, it isn't the next copy's
        f.state = Follower::State::empty;
        return;
    }

    // The audio thread stops playing it at the end of its block, if it was.
    f.state = Follower::State::busy;
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const ScopedExclusive exclusive (f.guard);
        old = std::move (f.plugin);
        f.clap = nullptr;
    }

    f.stateHash = 0;

    // It crashed in the block that was being played when it was asked to go.
    if (f.crashed.load())
    {
        if (old != nullptr)
        {
            stopCalling (*old);
            (void) old.release();
        }

        f.crashed = false;

        if (const auto code = crashCodes[(size_t) index].exchange (0); code != 0)
            copyCrashed (index, code, "while playing");
    }

    f.state = Follower::State::empty;

    if (old != nullptr)
    {
        CrashReport::setActivity ("closing copy " + juce::String (index + 1) + " of the synth " + old->getName());
        old->releaseResources();
        old.reset();
    }
}

void HostedSynth::removeAllCopies()
{
    for (int c = maxCopies - 1; c >= 1; --c)
        if (followers[(size_t) c].state.load() != Follower::State::empty)
            removeCopy (c);
}

void HostedSynth::updateWorkers()
{
    const auto wanted = copiesWanted() && copySettings.copiesUseCores;

    if (wanted == (workers != nullptr))
        return;

    auto next = wanted ? CopyWorkers::getShared (! offlineOnly) : nullptr;

    {
        const ScopedExclusive exclusive (guard);
        std::swap (workers, next);
    }

    next.reset();       // the old threads, now unused, stop

    if (workers != nullptr)
        PluginLog::add ("Copies of " + getName() + " are played on " + juce::String (workers->getNumWorkers() + 1) + " threads");
}

bool HostedSynth::copiesSettled() const
{
    if (! copiesWanted())
        return true;

    if (loadingCopy || soundCopy.running)
        return false;

    if (copiesProblem.isNotEmpty())
        return true;

    for (int c = 1; c < getWantedCopies(); ++c)
        if (followers[(size_t) c].state.load() == Follower::State::empty)
            return false;

    return true;
}

void HostedSynth::settleCopies()
{
    if (copiesLoadedCallbacks.empty() || ! copiesSettled())
        return;

    auto callbacks = std::move (copiesLoadedCallbacks);
    copiesLoadedCallbacks.clear();

    for (auto& callback : callbacks)
        callback();
}

void HostedSynth::whenCopiesLoaded (std::function<void()> callback)
{
    if (copiesSettled())
        callback();
    else
        copiesLoadedCallbacks.push_back (std::move (callback));
}

void HostedSynth::notifyCopiesChanged()
{
    if (onCopiesChanged)
        onCopiesChanged();
}

void HostedSynth::copyCrashed (int index, unsigned int exceptionCode, const juce::String& doing)
{
    if (auto* plugin = followers[(size_t) index].plugin.get())
        stopCalling (*plugin);

    crashNote = "Copy " + juce::String (index + 1) + " of " + getName() + " crashed (" + CopySafety::describeException (exceptionCode)
              + ") " + doing + ", and was switched off; the others play on. Save your project soon: a synth that has "
                "crashed once may do it again.";
    PluginLog::add (crashNote);
    notifyCopiesChanged();
}

void HostedSynth::reloadCopies()
{
    // Copies that crashed are let go of for good, and new ones loaded in their place.
    for (int c = 1; c < maxCopies; ++c)
        if (followers[(size_t) c].crashed.load())
            removeCopy (c);

    if (auto* settings = getUserSettings())
    {
        auto crashed = juce::StringArray::fromLines (settings->getValue (crashedWhileLoadingKey));
        crashed.removeString (getCopyIdentity());
        crashed.removeEmptyStrings();
        settings->setValue (crashedWhileLoadingKey, crashed.joinIntoString ("\n"));
        settings->saveIfNeeded();
    }

    copiesProblem.clear();
    crashNote.clear();

    if (copiesWanted())
        PluginLog::add ("Loading the copies of " + getName() + " again, as asked");

    updateCopies();
}

HostedSynth::CopyStatus HostedSynth::getCopyStatus() const
{
    CopyStatus s;
    s.on = copiesWanted();

    if (! s.on)
        return s;

    s.wanted = getWantedCopies();
    s.loaded = 1;
    s.inStep = 1;

    for (int c = 1; c < maxCopies; ++c)
    {
        const auto& f = followers[(size_t) c];

        if (f.crashed.load())
        {
            ++s.crashed;
        }
        else if (f.plugin != nullptr)
        {
            ++s.loaded;

            if (! (soundCopy.running && soundCopy.copies[(size_t) c]))
                ++s.inStep;
        }
    }

    s.playing = copiesPlaying.load();
    s.asleep = copiesAsleep.load();
    s.workers = workers != nullptr ? workers->getNumWorkers() : 0;
    s.loading = loadingCopy;
    s.copyingSound = soundCopy.running;
    s.problem = (copiesProblem + " " + crashNote).trim();
    return s;
}

juce::AudioPluginInstance* HostedSynth::getCopy (int index) const
{
    if (index == 0)
        return instance.get();

    if (! juce::isPositiveAndBelow (index, maxCopies) || followers[(size_t) index].crashed.load())
        return nullptr;

    return followers[(size_t) index].plugin.get();
}

void HostedSynth::watchCopies()
{
    if (anyCopyCrashed.exchange (false))
        for (int c = 1; c < maxCopies; ++c)
            if (const auto code = crashCodes[(size_t) c].exchange (0); code != 0)
                copyCrashed (c, code, "while playing");

    // The counts the status shows.
    const auto playing = copiesPlaying.load(), asleep = copiesAsleep.load();

    if (playing != lastPlaying || asleep != lastAsleep)
    {
        lastPlaying = playing;
        lastAsleep = asleep;
        notifyCopiesChanged();
    }
}

//==============================================================================
// One copy per note: the loading marker, so a crash while loading copies isn't repeated.

juce::String HostedSynth::getCopyIdentity() const
{
    if (description == nullptr)
        return {};

    const auto& d = *description;
    return d.pluginFormatName + ":" + juce::String (d.uniqueId != 0 ? d.uniqueId : d.deprecatedUid) + ":" + d.name;
}

void HostedSynth::setLoadingMarker (bool isLoading)
{
    auto* settings = getUserSettings();

    if (settings == nullptr)
        return;

    // Which run of which program left it: its ID, and when it started (the ID alone may be another
    // program's by the time it's read).
    const auto processId = CopySafety::getProcessId();

    if (isLoading)
        settings->setValue (loadingMarker, thisRun() + "|" + juce::String (processId) + "@"
                                               + juce::String (CopySafety::getProcessStartTime (processId)) + "|" + getCopyIdentity());
    else
        settings->removeValue (loadingMarker);

    settings->saveIfNeeded();
}

bool HostedSynth::copiesBlockedAfterCrash()
{
    auto* settings = getUserSettings();

    if (settings == nullptr)
        return false;

    auto crashed = juce::StringArray::fromLines (settings->getValue (crashedWhileLoadingKey));
    crashed.removeEmptyStrings();

    // A marker left by an earlier run: it closed while that copy was loading. Not one of another
    // program still running (FL Studio and the MPE Roll app share the settings file), nor of
    // another MPE Roll plugin file in this one (the VST2 and the VST3 in one project), which is
    // loading copies right now.
    const auto properties = settings->getAllProperties();   // a copy: markers are removed as they're found
    bool found = false;

    for (int i = 0; i < properties.size(); ++i)
    {
        const auto key = properties.getAllKeys()[i];
        const auto value = properties.getAllValues()[i];

        if (! key.startsWith (markerPrefix) || value.upToFirstOccurrenceOf ("|", false, false) == thisRun())
            continue;

        const auto afterRun = value.fromFirstOccurrenceOf ("|", false, false);
        const auto process = afterRun.upToFirstOccurrenceOf ("|", false, false);
        const auto processId = process.upToFirstOccurrenceOf ("@", false, false).getLargeIntValue();
        const auto startTime = process.fromFirstOccurrenceOf ("@", false, false).getLargeIntValue();

        if (CopySafety::isProcessRunning (processId, startTime))
            continue;

        const auto identity = afterRun.fromFirstOccurrenceOf ("|", false, false);
        crashed.addIfNotAlreadyThere (identity);
        settings->removeValue (key);
        found = true;
        PluginLog::add ("Loading copies of " + identity.fromLastOccurrenceOf (":", false, false)
                        + " was cut short last time: FL Studio or MPE Roll closed meanwhile");
    }

    if (found)
    {
        settings->setValue (crashedWhileLoadingKey, crashed.joinIntoString ("\n"));
        settings->saveIfNeeded();
    }

    return crashed.contains (getCopyIdentity());
}

//==============================================================================
// The synth's own loading marker, so a plugin that took FL Studio down while it was loading isn't
// loaded again on its own by every project that names it.

namespace
{
    /** The plugin and the file it's in, as the crash list keeps it: a second install of a plugin is
        another entry. The name first, so the log can take it. */
    juce::String synthIdentity (const juce::PluginDescription& d)
    {
        return d.name + "|" + d.pluginFormatName + ":" + juce::String (d.uniqueId != 0 ? d.uniqueId : d.deprecatedUid)
             + "|" + d.fileOrIdentifier;
    }
}

void HostedSynth::setSynthLoadingMarker (const juce::PluginDescription* desc)
{
    auto* settings = getUserSettings();

    if (settings == nullptr)
        return;

    // Which run of which program left it, as the copies' marker says it.
    const auto processId = CopySafety::getProcessId();

    if (desc != nullptr)
        settings->setValue (synthLoadingMarker, thisRun() + "|" + juce::String (processId) + "@"
                                                    + juce::String (CopySafety::getProcessStartTime (processId)) + "|" + synthIdentity (*desc));
    else
        settings->removeValue (synthLoadingMarker);

    settings->saveIfNeeded();
}

bool HostedSynth::synthLoadBlockedAfterCrash (const juce::PluginDescription& desc)
{
    auto* settings = getUserSettings();

    if (settings == nullptr)
        return false;

    auto crashed = juce::StringArray::fromLines (settings->getValue (synthCrashedKey));
    crashed.removeEmptyStrings();

    // Markers left by runs that closed while a synth was loading: not this run's, nor another
    // program's that still runs (FL Studio and the MPE Roll app share the settings file).
    const auto properties = settings->getAllProperties();   // a copy: markers are removed as they're found
    bool found = false;

    for (int i = 0; i < properties.size(); ++i)
    {
        const auto key = properties.getAllKeys()[i];
        const auto value = properties.getAllValues()[i];

        if (! key.startsWith (synthMarkerPrefix) || value.upToFirstOccurrenceOf ("|", false, false) == thisRun())
            continue;

        const auto afterRun = value.fromFirstOccurrenceOf ("|", false, false);
        const auto process = afterRun.upToFirstOccurrenceOf ("|", false, false);
        const auto processId = process.upToFirstOccurrenceOf ("@", false, false).getLargeIntValue();
        const auto startTime = process.fromFirstOccurrenceOf ("@", false, false).getLargeIntValue();

        if (CopySafety::isProcessRunning (processId, startTime))
            continue;

        const auto identity = afterRun.fromFirstOccurrenceOf ("|", false, false);
        crashed.addIfNotAlreadyThere (identity);
        settings->removeValue (key);
        found = true;
        PluginLog::add ("Loading " + identity.upToFirstOccurrenceOf ("|", false, false) + " from "
                        + identity.fromLastOccurrenceOf ("|", false, false)
                        + " was cut short last time: FL Studio or MPE Roll closed meanwhile, so it isn't loaded again on its own");
    }

    if (found)
    {
        settings->setValue (synthCrashedKey, crashed.joinIntoString ("\n"));
        settings->saveIfNeeded();
    }

    return crashed.contains (synthIdentity (desc));
}

void HostedSynth::forgetSynthCrash (const juce::PluginDescription& desc)
{
    auto* settings = getUserSettings();

    if (settings == nullptr)
        return;

    auto crashed = juce::StringArray::fromLines (settings->getValue (synthCrashedKey));
    crashed.removeEmptyStrings();

    if (! crashed.contains (synthIdentity (desc)))
        return;

    crashed.removeString (synthIdentity (desc));
    settings->setValue (synthCrashedKey, crashed.joinIntoString ("\n"));
    settings->saveIfNeeded();
}

//==============================================================================
// One copy per note: giving the copies the first copy's sound (message thread).

void HostedSynth::soundMayHaveChanged()
{
    if (! copiesWanted() || ! copySettings.copiesFollow)
        return;

    // A synth picking a preset may say so several times in a row: one look once it's settled.
    if (! std::exchange (soundChangeReported, true))
        reportedAt = juce::Time::getMillisecondCounterHiRes();

    ++soundGeneration;      // a copy loading now took the sound from before
    askForSoundCheck (checkAfterReportMs);
}

void HostedSynth::askForSoundCheck (int delayMs)
{
    // Each report puts the look off again, but never more than a second past the first one asked
    // for: a synth that reports changes all the time (on every parameter change, say) is still
    // looked at.
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (checkAskedAt <= 0.0)
        checkAskedAt = now;

    const auto due = juce::jmin (now + delayMs, checkAskedAt + longestCheckWaitMs);
    soundCheck.startTimer (juce::jmax (1, juce::roundToInt (due - now)));
}

void HostedSynth::keepLookingWhileOpen()
{
    if (windowOpen.load() && copiesWanted() && copySettings.copiesFollow && ! soundCheck.isTimerRunning())
        soundCheck.startTimer (checkWhileOpenMs);
}

void HostedSynth::forgetSoundChecks()
{
    soundCheck.stopTimer();
    soundCopyStep.stopTimer();
    endSoundCopy();
    checkAskedAt = 0.0;
    soundChangeReported = false;
    windowJustClosed = false;
    stateChangesByItself = false;
    unexplainedChanges = 0;
    hashAtLastCheck = 0;
    changesAtLastCheck = mirror.getChanges();
    wheelMovesAtLastCheck = wheelMoves.load (std::memory_order_relaxed);
    stateMilliseconds = 0.0;
    ++soundGeneration;
}

void HostedSynth::copySoundToAllCopies()
{
    if (! copiesWanted())
        return;

    const auto versions = mirror.snapshot();
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    startCopyingSound (std::move (state), versions, everyFollower(), true, "asked for");
}

void HostedSynth::checkSound()
{
    soundCheck.stopTimer();
    checkAskedAt = 0.0;

    if (! copiesWanted() || ! copySettings.copiesFollow)
        return;

    const auto reported = std::exchange (soundChangeReported, false);
    const auto closed = std::exchange (windowJustClosed, false);
    const auto changes = mirror.getChanges();
    const auto wheels = wheelMoves.load (std::memory_order_relaxed);
    const auto parametersMoved = changes != changesAtLastCheck || wheels != wheelMovesAtLastCheck;
    changesAtLastCheck = changes;
    wheelMovesAtLastCheck = wheels;

    // A change reported while knobs are still being turned (some synths report every move): it's
    // looked at once they've been still for a moment, or after two seconds of automation, when
    // the copies have played the moves too and are found to have the sound rather than given it.
    if (reported && ! closed && parametersMoved
        && juce::Time::getMillisecondCounterHiRes() - reportedAt < longestReportWaitMs)
    {
        soundChangeReported = true;
        soundCheck.startTimer (checkAfterReportMs);
        return;
    }

    // Otherwise it's the look taken every few seconds while the first copy's window is open. Not
    // for a synth whose state is slow to get (a big sample library), nor one whose state changes
    // by itself: theirs are copied when the synth reports a change, or when its window closes.
    const auto justLooking = ! reported && ! closed;

    if (justLooking && (stateChangesByItself || stateMilliseconds > cheapStateMs))
        return;

    // Knobs being turned: the copies get those as they move. Whatever else changed is looked at
    // once they've been left alone for a moment, so the copies aren't given the whole sound over
    // and over meanwhile.
    if (justLooking && parametersMoved)
    {
        hashAtLastCheck = 0;
        soundCheck.startTimer (checkWhileOpenMs);
        return;
    }

    const auto versions = mirror.snapshot();
    juce::MemoryBlock state;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    instance->getStateInformation (state);
    stateMilliseconds = juce::Time::getMillisecondCounterHiRes() - started;
    const auto hash = hashOf (state);

    // A state that changes with nothing done to the synth (a time stamp in it, say) would be
    // copied every few seconds for nothing.
    if (justLooking && hashAtLastCheck != 0 && hash != hashAtLastCheck)
    {
        if (++unexplainedChanges >= 6)
        {
            stateChangesByItself = true;
            PluginLog::add (getName() + "'s state changes with nothing done to it: its copies get its sound only when it "
                            "reports a change or its window closes");
        }
    }
    else if (justLooking)
    {
        unexplainedChanges = 0;
    }

    hashAtLastCheck = hash;
    CopyRouter::CopySet behind;

    for (int c = 1; c < maxCopies; ++c)
        if (const auto& f = followers[(size_t) c]; f.plugin != nullptr && ! f.crashed.load() && f.stateHash != hash)
            behind.set ((size_t) c);

    // This very sound being given already (to a copy that just loaded, say): the copies behind
    // are given it too, with the rest.
    if (behind.any() && soundCopy.running && soundCopy.hash == hash)
        soundCopy.copies |= behind;
    else if (behind.any())
        startCopyingSound (std::move (state), versions, everyFollower(), false,
                           reported ? "it reported a change" : closed ? "its window closed" : "it changed");

    if (windowOpen.load())
        soundCheck.startTimer (checkWhileOpenMs);
}

void HostedSynth::startCopyingSound (juce::MemoryBlock state, ParameterMirror::Snapshot versions, CopyRouter::CopySet copies,
                                     bool evenIfInStep, const juce::String& reason)
{
    endSoundCopy();
    soundCopy.hash = hashOf (state);
    soundCopy.state = std::move (state);
    soundCopy.versions = std::move (versions);
    soundCopy.copies = copies;
    soundCopy.reason = reason;
    soundCopy.running = true;
    hashAtLastCheck = soundCopy.hash;
    ++soundGeneration;

    // Asked for: each copy is looked at, even one given this very state before.
    if (evenIfInStep)
        for (int c = 1; c < maxCopies; ++c)
            if (copies[(size_t) c])
                followers[(size_t) c].stateHash = 0;

    copySoundToNext();
}

void HostedSynth::copySoundToNext()
{
    soundCopyStep.stopTimer();

    if (! soundCopy.running)
        return;

    if (! copiesWanted())
    {
        endSoundCopy();
        settleCopies();
        return;
    }

    // Gone, crashed, or still loading (it takes the sound as it is when it's loaded), or has it.
    for (int c = 1; c < maxCopies; ++c)
    {
        const auto& f = followers[(size_t) c];

        if (soundCopy.copies[(size_t) c]
            && (f.state.load() != Follower::State::ready || f.plugin == nullptr || f.crashed.load() || f.stateHash == soundCopy.hash))
            leaveSoundCopy (c);
    }

    if (soundCopy.due != 0)
    {
        giveDueCopyTheSound();
        return;
    }

    if (soundCopy.copies.none())
    {
        if (soundCopy.copied > 0)
            PluginLog::add ("Gave " + juce::String (soundCopy.copied) + (soundCopy.copied == 1 ? " copy of " : " copies of ")
                            + getName() + " its sound (" + soundCopy.reason + ")"
                            + (soundCopy.alreadyHad > 0 ? "; " + juce::String (soundCopy.alreadyHad) + " had it already" : juce::String()));
        else if (soundCopy.alreadyHad > 0)
            PluginLog::add ("The copies of " + getName() + " had its sound already (" + soundCopy.reason + ")");

        endSoundCopy();
        notifyCopiesChanged();
        settleCopies();
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (soundCopy.lookingSince <= 0.0)
        soundCopy.lookingSince = now;

    const auto looked = now - soundCopy.lookingSince;

    // The copies are given it one at a time, each chosen afresh at every look while it's still
    // open to new notes: one holding no note whose sound has died away, or, with none such, the
    // one that has held no note longest once that's 10 seconds (a drone, long tails in dense
    // playing). A copy holding notes is given it once they've ended: new state would cut them short.
    const auto holding = (uint32_t) copiesSeen.load (std::memory_order_acquire);
    const auto sounding = copiesSounding.load (std::memory_order_acquire);

    for (int c = 1; c < maxCopies; ++c)
    {
        auto& since = soundCopy.freeSince[(size_t) c];

        if (! soundCopy.copies[(size_t) c] || (holding & (1u << c)) != 0)
            since = 0.0;
        else if (since <= 0.0)
            since = now;
    }

    int next = 0;
    bool quiet = true;

    for (int c = 1; c < maxCopies && next == 0; ++c)
        if (soundCopy.freeSince[(size_t) c] > 0.0 && (sounding & (1u << c)) == 0)
            next = c;

    if (next == 0)
    {
        quiet = false;

        for (int c = 1; c < maxCopies; ++c)
            if (const auto since = soundCopy.freeSince[(size_t) c];
                since > 0.0 && now - since >= longestTailWaitMs && (next == 0 || since < soundCopy.freeSince[(size_t) next]))
                next = c;
    }

    if (next == 0)
    {
        soundCopyStep.startTimer (heldCopyRetryMs);
        return;
    }

    // It has played the knob moves made before the first copy's state was taken, else its own
    // state would seem to lack them (or a moment has passed: a copy asleep plays none).
    if (looked < catchUpWaitMs
        && (int32_t) (followers[(size_t) next].playedChanges.load (std::memory_order_acquire) - soundCopy.versions.changes) < 0)
    {
        soundCopyStep.startTimer (soundStepMs);
        return;
    }

    // Closed to new notes for a block or two: then given the sound, once a block that knew shows
    // no note reached it just before. Closed first and the blocks counted after, so any block
    // counted later either knew or shows it was still open.
    copiesDueSound.fetch_or (1u << next, std::memory_order_seq_cst);
    soundCopy.due = next;
    soundCopy.closedQuiet = quiet;
    soundCopy.blocksAtClose = soundCopy.blocksSeen = copyBlocks.load (std::memory_order_seq_cst);
    soundCopy.blockSeenAt = now;
    soundCopyStep.startTimer (soundStepMs);
}

void HostedSynth::giveDueCopyTheSound()
{
    const auto c = soundCopy.due;
    const auto bit = 1u << c;
    auto& f = followers[(size_t) c];
    const auto now = juce::Time::getMillisecondCounterHiRes();

    const auto ended = copyBlocks.load (std::memory_order_seq_cst);

    if (ended != soundCopy.blocksSeen)
    {
        soundCopy.blocksSeen = ended;
        soundCopy.blockSeenAt = now;
    }

    // A block that ended since it was closed, and knew: no note can reach it from then on. Or no
    // block has ended for a while and none is under way (the host has stopped playing): the next
    // one will know.
    const auto seen = copiesSeen.load (std::memory_order_acquire);
    const auto knew = ended != soundCopy.blocksAtClose && ((seen >> 32) & bit) != 0;
    const auto idle = now - soundCopy.blockSeenAt >= hostIdleMs && copyBlocksStarted.load (std::memory_order_seq_cst) == ended;

    if (! knew && ! idle)
    {
        soundCopyStep.startTimer (soundStepMs);
        return;
    }

    // A note reached it before it was closed (it's held, or still dying away): it's open again, and
    // another copy goes first. The sound is read after the notes: from the same block, or a later one.
    const auto soundingNow = (copiesSounding.load (std::memory_order_acquire) & bit) != 0;

    if ((seen & bit) != 0 || (soundCopy.closedQuiet && soundingNow))
    {
        copiesDueSound.fetch_and (~bit, std::memory_order_seq_cst);
        soundCopy.due = 0;
        soundCopyStep.startTimer (soundStepMs);
        return;
    }

    // Out of play while it's looked at and given the sound, so nothing else calls it meanwhile and
    // a crash on either side is seen before it's called again; the others play on.
    enum class Outcome { skipped, had, given, crashedLooking, crashedTaking };
    auto outcome = Outcome::skipped;
    unsigned int code = 0;
    f.state = Follower::State::busy;

    {
        const ScopedExclusive exclusive (f.guard);

        if (! f.crashed.load())     // it may have crashed in the block that just ended
        {
            // Most often it has the sound already, its parameters having followed the first copy's:
            // its own state says so, and it's left as it is.
            juce::MemoryBlock own;
            GetStateCall look { f.plugin.get(), &own };

            if (! CopySafety::call (callGetState, &look, &code))
            {
                outcome = Outcome::crashedLooking;
            }
            else if (hashOf (own) == soundCopy.hash)
            {
                outcome = Outcome::had;
            }
            else
            {
                SetStateCall call { f.plugin.get(), &soundCopy.state };
                outcome = CopySafety::call (callSetState, &call, &code) ? Outcome::given : Outcome::crashedTaking;

                if (outcome == Outcome::given)
                {
                    mirror.markSeen (f, soundCopy.versions);
                    f.fresh = true;
                }
            }

            if (outcome == Outcome::had || outcome == Outcome::given)
                f.stateHash = soundCopy.hash;
        }
    }

    // Crashed before it's ready again, so the audio thread never sees it ready and not crashed.
    const auto crashedNow = outcome == Outcome::crashedLooking || outcome == Outcome::crashedTaking;

    if (crashedNow)
        f.crashed = true;

    f.state.store (Follower::State::ready, std::memory_order_release);
    leaveSoundCopy (c);
    soundCopy.lookingSince = 0.0;

    if (crashedNow)
        copyCrashed (c, code, outcome == Outcome::crashedLooking ? "while MPE Roll looked at its sound"
                                                                 : "while taking the first copy's sound");
    else if (outcome == Outcome::had)
        ++soundCopy.alreadyHad;
    else if (outcome == Outcome::given)
        ++soundCopy.copied;

    notifyCopiesChanged();
    soundCopyStep.startTimer (soundStepMs);     // the next one after a breath
}

void HostedSynth::leaveSoundCopy (int copy)
{
    soundCopy.copies.reset ((size_t) copy);

    if (soundCopy.due == copy)
        soundCopy.due = 0;

    copiesDueSound.fetch_and (~(1u << copy), std::memory_order_seq_cst);
}

void HostedSynth::endSoundCopy()
{
    soundCopy = {};
    copiesDueSound.store (0, std::memory_order_seq_cst);
}

//==============================================================================
// One copy per note: playing the copies (audio thread).

juce::Optional<juce::AudioPlayHead::PositionInfo> HostedSynth::FallbackHead::getPosition() const
{
    PositionInfo info;
    info.setBpm (120.0);
    info.setTimeSignature (TimeSignature {});
    info.setTimeInSamples (0);
    info.setTimeInSeconds (0.0);
    info.setPpqPosition (0.0);
    info.setPpqPositionOfLastBarStart (0.0);
    info.setIsPlaying (false);
    return info;
}

void HostedSynth::processCopies (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead,
                                 bool justStarted, const juce::MidiBuffer* leadEndings)
{
    const auto numSamples = buffer.getNumSamples();

    // Copies about to be given the first copy's sound, closed to new notes for the whole block. The
    // block is counted as begun first, so the message thread can tell that it's under way.
    copyBlocksStarted.fetch_add (1, std::memory_order_seq_cst);
    const auto due = copiesDueSound.load (std::memory_order_seq_cst);

    for (auto& m : copyMidi)
        m.clear();

    // Into copy mode afresh: nothing the router knew from an earlier time in it holds now (the
    // pedal, the zone values, what the notes' channels last had).
    if (justStarted)
        router.reset();

    // What the copies are played with. A change ends their notes and puts their controllers back
    // at rest (written to the MIDI just cleared): the copies asleep wake for it.
    if (router.setSettings (outputSettings, copyMidi.data(), 0))
        for (auto& r : renders)
            r.asleep = false;

    copyPlay = copyPlaySettings (outputSettings);
    const auto numCopies = router.getNumCopies();

    // Into copy mode: the notes the first copy was playing the other way end as they started. The
    // processor sent their note-offs the old way, which mean nothing to the router, and a VST3
    // synth only hears All Notes Off if it maps that controller.
    if (justStarted)
    {
        for (const auto metadata : midi)
        {
            const auto status = metadata.data[0] & 0xf0;

            if (metadata.numBytes == 3 && (status == 0x80 || (status == 0x90 && metadata.data[2] == 0)))
                copyMidi[0].addEvent (metadata.data, metadata.numBytes, 0);
        }

        if (leadEndings != nullptr)
            for (const auto metadata : *leadEndings)
                copyMidi[0].addEvent (metadata.data, metadata.numBytes, 0);
    }

    // The copies to play this block: the first, and the others that are ready. Each is held until
    // the block ends, so the message thread can't change it meanwhile.
    CopyRouter::CopySet available;
    available.set (0);

    for (int c = 1; c < numCopies; ++c)
    {
        auto& f = followers[(size_t) c];

        if (f.state.load (std::memory_order_acquire) != Follower::State::ready || f.crashed.load() || ! f.guard.tryEnter())
            continue;

        if (f.state.load (std::memory_order_acquire) == Follower::State::ready && f.plugin != nullptr && ! f.crashed.load())
        {
            available.set ((size_t) c);
            enteredCopies.set ((size_t) c);
        }
        else
        {
            f.guard.exit();
        }
    }

    // Let go at the end of the block however it ends: if the first copy's crash is caught further
    // up (by the host), a copy left held would wait forever for whoever removes it.
    struct LetGo
    {
        HostedSynth& synth;

        ~LetGo()
        {
            for (int c = 1; c < maxCopies; ++c)
                if (synth.enteredCopies[(size_t) c])
                    synth.followers[(size_t) c].guard.exit();

            synth.enteredCopies.reset();
        }
    } letGo { *this };

    // Copies that went (removed, crashed, or taking new state) lose their notes, which are owed
    // note-offs. Copies that came, or were given new state, start afresh: their notes end, and
    // they're told their bend range and every zone-wide value.
    const auto everyone = introduceAll.exchange (false) || justStarted;
    const auto leadIsFresh = leadFresh.exchange (false);

    for (int c = 0; c < maxCopies; ++c)
    {
        const auto was = playedCopies[(size_t) c], is = available[(size_t) c];
        const auto flagged = c == 0 ? leadIsFresh : followers[(size_t) c].fresh.exchange (false);

        if (was && ! is)
            router.dropCopy (c);

        if (is && (everyone || flagged || ! was))
        {
            router.dropCopy (c);
            router.endDroppedNotes (c, copyMidi[(size_t) c], 0);
            addEndOfAllNotes (copyMidi[(size_t) c], 0);
            router.introduce (c, copyMidi[(size_t) c], 0);
            router.resendEveryCopyKeys (c, copyMidi[(size_t) c], 0);     // a keyswitch still held, say

            auto& r = renders[(size_t) c];
            r.quietSeconds = 0.0;
            r.quiet = true;
            r.asleep = false;
        }
    }

    playedCopies = available;
    openCopies = available;

    for (int c = 1; c < maxCopies; ++c)
        if ((due & (1u << c)) != 0)
            openCopies.reset ((size_t) c);

    if (configurationRequested.exchange (false))
        router.writeConfiguration (copyMidi.data(), 0);

    // Copies that mayn't sleep any more (sleep turned off, or the first copy's window opened) wake,
    // with the zone-wide values they missed.
    for (int c = 0; c < numCopies; ++c)
    {
        auto& r = renders[(size_t) c];

        if (r.asleep && available[(size_t) c] && (! outputSettings.copiesSleep || (c == 0 && windowOpen.load())))
        {
            router.introduce (c, copyMidi[(size_t) c], 0);
            r.asleep = false;
            r.quietSeconds = 0.0;
        }
    }

    // Never without a transport: some synths crash when the host gives none. A block longer than
    // prepare() said is played in pieces it was prepared for, each with the transport moved on.
    auto* head = playHead != nullptr ? playHead : &fallbackHead;
    const auto pieceSize = juce::jmax (1, blockSize);

    for (int start = 0; start < numSamples; start += pieceSize)
    {
        const auto length = juce::jmin (pieceSize, numSamples - start);
        const juce::MidiBuffer* pieceMidi = &midi;

        if (length < numSamples)
        {
            midiPiece.clear();
            midiPiece.addEvents (midi, start, length, -start);
            pieceMidi = &midiPiece;
        }

        pieceHead.source = head;
        pieceHead.offset = start;
        pieceHead.sampleRate = sampleRate;

        juce::AudioBuffer<float> piece (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, length);
        processCopiesPiece (piece, *pieceMidi, start == 0 ? head : &pieceHead);
    }

    copiesPlaying = router.getNumCopiesPlaying();

    // For the message thread, giving copies a sound: which hold notes and which still sound, as
    // the block ends, and which it was told were closed to new notes.
    uint32_t holding = 0, sounding = 0;

    for (int c = 0; c < maxCopies; ++c)
    {
        if (router.holdsNotes (c))
            holding |= 1u << c;

        if (playedCopies[(size_t) c] && ! renders[(size_t) c].quiet)
            sounding |= 1u << c;
    }

    copiesSounding.store (sounding, std::memory_order_release);
    copiesSeen.store (holding | ((uint64_t) due << 32), std::memory_order_release);
    copyBlocks.fetch_add (1, std::memory_order_seq_cst);
}

void HostedSynth::processCopiesPiece (juce::AudioBuffer<float>& out, const juce::MidiBuffer& in, juce::AudioPlayHead* head)
{
    const auto numSamples = out.getNumSamples();
    const auto numCopies = router.getNumCopies();
    CopyRouter::CopySet quiet, asleep;

    for (int c = 0; c < numCopies; ++c)
    {
        quiet.set ((size_t) c, renders[(size_t) c].quiet);
        asleep.set ((size_t) c, renders[(size_t) c].asleep);
    }

    router.route (in, copyMidi.data(), playedCopies, openCopies, quiet, asleep);

    // The copies to process: the ones playing and awake, a note having woken some. The MIDI of the
    // rest goes: a sleeping copy is sent what it missed when it wakes.
    numJobs = 0;

    for (int c = 0; c < maxCopies; ++c)
    {
        auto& r = renders[(size_t) c];

        if (r.asleep && ! asleep[(size_t) c])
        {
            r.asleep = false;
            r.quietSeconds = 0.0;
        }

        if (c < numCopies && playedCopies[(size_t) c] && ! r.asleep)
            jobs[(size_t) numJobs++] = c;
    }

    jobSamples = numSamples;
    jobHead = head;

    // The first copy is always played on this thread (the host's), never a worker's: only the
    // others are protected from crashing (where that can be caught), so a crash of the first is
    // the host's to handle, as it would be without copies. The workers are set going on the others
    // first, and are waited for even if it is handled further up.
    const auto withLead = numJobs > 0 && jobs[0] == 0;
    jobOffset = withLead ? 1 : 0;
    const auto numFollowers = numJobs - jobOffset;

    if (workers != nullptr && outputSettings.copiesUseCores && numFollowers > 0)
    {
        struct Finish
        {
            CopyWorkers& workers;
            CopyWorkers::Batch batch;
            ~Finish()                   { workers.finish (batch); }
        } finish { *workers, workers->begin (numFollowers, renderJob, this, withLead) };

        if (withLead)
            renderCopy (0);
    }
    else
    {
        for (int i = 0; i < numFollowers; ++i)
            renderCopy (jobs[(size_t) (jobOffset + i)]);

        if (withLead)
            renderCopy (0);
    }

    // Mixed down, in the copies' order (so it comes out the same however they were spread over
    // cores), and each copy's level noted.
    out.clear();
    const auto seconds = numSamples / sampleRate;

    for (int i = 0; i < numJobs; ++i)
    {
        const auto c = jobs[(size_t) i];
        auto& r = renders[(size_t) c];

        for (int ch = 0; ch < out.getNumChannels() && r.outputs > 0; ++ch)
            out.addFrom (ch, 0, r.audio, juce::jmin (ch, r.outputs - 1), 0, numSamples);

        // A copy with no note left whose sound has died away is preferred for the next note, and
        // after a second of quiet it stops being processed (but not the first copy while its
        // window is open: its own keyboard may be played, nor one holding a key to every copy,
        // which would miss its note-off). A key to every copy alone makes no sound.
        if (router.holdsNotes (c) || r.level > quietLevel)
        {
            r.quietSeconds = 0.0;
            r.quiet = false;
            continue;
        }

        r.quietSeconds += seconds;
        r.quiet = r.quietSeconds >= 0.05;

        if (outputSettings.copiesSleep && ! router.isBusy (c) && r.quietSeconds >= quietBeforeSleep && ! (c == 0 && windowOpen.load()))
            r.asleep = true;
    }

    int sleeping = 0;

    for (int c = 0; c < numCopies; ++c)
        if (playedCopies[(size_t) c] && renders[(size_t) c].asleep)
            ++sleeping;

    copiesAsleep = sleeping;

    for (auto& m : copyMidi)
        m.clear();
}

void HostedSynth::renderJob (void* context, int index) noexcept
{
    auto& self = *static_cast<HostedSynth*> (context);
    self.renderCopy (self.jobs[(size_t) (self.jobOffset + index)]);
}

void HostedSynth::renderCopy (int c)
{
    auto& r = renders[(size_t) c];
    r.outputs = 0;
    r.level = 0.0f;

    // One that crashed in an earlier piece of this block is never called again.
    if (c > 0 && followers[(size_t) c].crashed.load())
        return;

    auto* plugin = c == 0 ? instance.get() : followers[(size_t) c].plugin.get();

    if (plugin == nullptr || plugin->isSuspended())
        return;

    const auto outputs = plugin->getTotalNumOutputChannels();
    const auto needed = juce::jmax (plugin->getTotalNumInputChannels(), outputs);

    if (needed > r.audio.getNumChannels() || jobSamples > r.audio.getNumSamples())
        return;

   #if MPEROLL_HOST_CLAP
    if (auto* clap = c == 0 ? clapControls : followers[(size_t) c].clap)
        clap->setMpeOutputSettings (copyPlay);
   #endif

    juce::AudioBuffer<float> view (r.audio.getArrayOfWritePointers(), needed, jobSamples);
    view.clear();
    plugin->setPlayHead (jobHead);

    if (c == 0)
    {
        // The parameter changes made so far reach it with this block (a VST3 or CLAP synth is only
        // given them with one).
        const auto changes = mirror.getChanges();
        playingFirstCopy = true;
        plugin->processBlock (view, copyMidi[0]);
        playingFirstCopy = false;
        mirror.leadPlayed (changes);
    }
    else
    {
        // The first copy's parameter changes reach this one before its block. A copy that crashes,
        // in either, is switched off where a crash can be caught, rather than taking FL with it.
        struct CatchUp
        {
            const ParameterMirror* mirror;
            Follower* follower;
            juce::AudioPluginInstance* plugin;
        } catchUp { &mirror, &followers[(size_t) c], plugin };

        ProcessCall call { plugin, &view, &copyMidi[(size_t) c],
                           [] (void* context)
                           {
                               auto& u = *static_cast<CatchUp*> (context);
                               u.mirror->catchUp (*u.follower, *u.plugin);
                           },
                           &catchUp };
        unsigned int code = 0;
        const auto changes = mirror.getChanges();      // all caught up with below

        if (! CopySafety::call (callProcess, &call, &code))
        {
            // At once, before the host's timers (a VST3's parameter changes, say) next call it.
            stopCalling (*plugin);
            followers[(size_t) c].crashed = true;
            crashCodes[(size_t) c] = code != 0 ? code : 1u;
            anyCopyCrashed = true;
            return;
        }

        followers[(size_t) c].playedChanges.store (changes, std::memory_order_release);
    }

    r.outputs = outputs;

    for (int ch = 0; ch < juce::jmin (outputs, view.getNumChannels()); ++ch)
        r.level = juce::jmax (r.level, view.getMagnitude (ch, 0, jobSamples));
}

} // namespace mperoll
