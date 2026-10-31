#include "PluginEditor.h"
#include "BuildInfo.h"
#include "PluginLog.h"
#include "ui/DonatePanel.h"
#include "core/FlScriptExporter.h"
#include "core/MidiFileIO.h"
#include "ui/SettingsPanel.h"
#include "ui/WindowDrawing.h"

namespace mperoll
{

namespace
{
    constexpr int rulerHeight = 24;
    constexpr int splitterHeight = 6;
    const juce::String dropDownArrow = juce::String::fromUTF8 (" \xe2\x96\xbe");
    const juce::String findingOutWhyStatus = "The synth didn't load: finding out why (if it opens a window of its own, "
                                             "answer it)...";
}

/** The handle between the grid and the expression lane. */
class MpeRollEditor::Splitter : public juce::Component
{
public:
    explicit Splitter (MpeRollEditor& e) : editor (e)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::outline);
        g.fillRect (getWidth() / 2 - 20, getHeight() / 2 - 1, 40, 2);
    }

    void mouseDown (const juce::MouseEvent&) override { startHeight = editor.laneHeight; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        editor.laneHeight = juce::jlimit (90, editor.getHeight() - 240, startHeight - e.getDistanceFromDragStartY());
        editor.resized();
    }

private:
    MpeRollEditor& editor;
    int startHeight = 0;
};

/** Dragging the MIDI button drops the clip into FL (or anywhere else) as a .mid file. */
class MpeRollEditor::MidiDragListener : public juce::MouseListener
{
public:
    explicit MidiDragListener (MpeRollEditor& e) : editor (e) {}

    void mouseDown (const juce::MouseEvent&) override { dragged = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged || e.getDistanceFromDragStart() < 8)
            return;

        dragged = true;
        const auto file = editor.writeTemporaryMidiFile();

        if (file.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, &editor.toolbar.midiButton);
    }

    bool dragged = false;

private:
    MpeRollEditor& editor;
};

//==============================================================================
MpeRollEditor::MpeRollEditor (MpeRollProcessor& p)
    : AudioProcessorEditor (p),
      rollProcessor (p),
      document (p.getDocument()),
      ruler (p.getDocument(), view),
      keys (p, view),
      grid (p, view),
      lane (p, view)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (toolbar);
    addAndMakeVisible (ruler);
    addAndMakeVisible (keys);
    addAndMakeVisible (grid);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    addAndMakeVisible (lane);

    splitter = std::make_unique<Splitter> (*this);
    addAndMakeVisible (*splitter);

    midiDragListener = std::make_unique<MidiDragListener> (*this);
    toolbar.midiButton.addMouseListener (midiDragListener.get(), false);

    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    vScroll.setAutoHide (false);

    view.addListener (this);
    document.addChangeListener (this);
    document.getUndoManager().addChangeListener (this);
    skins->addChangeListener (this);
    rollProcessor.onStatusChanged = [safe = juce::Component::SafePointer<MpeRollEditor> (this)]
    {
        if (safe != nullptr)
        {
            if (safe->rollProcessor.getStatusMessage().isNotEmpty())
                safe->setStatus (safe->rollProcessor.getStatusMessage());

            safe->refreshToolbar();
        }
    };

    rollProcessor.onCopiesChanged = [safe = juce::Component::SafePointer<MpeRollEditor> (this)]
    {
        if (safe != nullptr)
            safe->refreshToolbar();
    };

    wireToolbar();

    setResizable (true, true);
    setResizeLimits (860, 540, 3200, 2200);
    setSize (1200, 760);

    scrollToContent();
    refreshToolbar();
    startTimerHz (30);
}

MpeRollEditor::~MpeRollEditor()
{
    // The settings call-out is deleted later, by the message loop, and FL may delete the processor
    // before then: cut its panel off from the processor now, and close it. (Not hidden: the editor
    // has usually left the screen already, and hiding would hand it the keyboard focus.)
    if (auto* box = settingsBox.getComponent())
    {
        for (auto* child : box->getChildren())
            if (auto* panel = dynamic_cast<SettingsPanel*> (child))
                panel->detach();

        box->exitModalState (0);
    }

    // A tool's dialog likewise: its preview is withdrawn while the document is still here.
    closeToolDialog (true);

    // The "No sound from ..." and "... has no MPE" boxes: closed, they keep MPE, so nothing is
    // changed on the way out.
    if (silenceAlert != nullptr)
        silenceAlert->exitModalState (0);

    if (mpeAdvice != nullptr)
        mpeAdvice->exitModalState (0);

    stopTimer();
    rollProcessor.onStatusChanged = nullptr;
    rollProcessor.onCopiesChanged = nullptr;
    toolbar.midiButton.removeMouseListener (midiDragListener.get());
    skins->removeChangeListener (this);
    document.getUndoManager().removeChangeListener (this);
    document.removeChangeListener (this);
    view.removeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void MpeRollEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    // Corner above the keyboard.
    g.setColour (Theme::panel);
    g.fillRect (0, Toolbar::preferredHeight, ViewState::keysWidth, rulerHeight);
}

void MpeRollEditor::resized()
{
    auto area = getLocalBounds();
    toolbar.setBounds (area.removeFromTop (Toolbar::preferredHeight));

    laneHeight = juce::jlimit (90, juce::jmax (90, area.getHeight() - 220), laneHeight);
    lane.setBounds (area.removeFromBottom (laneHeight));
    splitter->setBounds (area.removeFromBottom (splitterHeight));

    auto rulerRow = area.removeFromTop (rulerHeight);
    ruler.setBounds (rulerRow.withTrimmedLeft (ViewState::keysWidth).withTrimmedRight (ViewState::scrollbarSize));

    auto scrollRow = area.removeFromBottom (ViewState::scrollbarSize);
    hScroll.setBounds (scrollRow.withTrimmedLeft (ViewState::keysWidth).withTrimmedRight (ViewState::scrollbarSize));

    vScroll.setBounds (area.removeFromRight (ViewState::scrollbarSize));
    keys.setBounds (area.removeFromLeft (ViewState::keysWidth));
    grid.setBounds (area);

    view.clampVertical ((float) grid.getHeight());
    updateScrollBars();
}

void MpeRollEditor::parentHierarchyChanged()
{
    // The window the editor is in only exists once the host has put it on screen, and it's a new
    // one each time the host reopens it.
    WindowDrawing::apply (*this, WindowDrawing::getMode (rollProcessor.getSynth().getUserSettings()));
    ticksSinceWindowCheck = 0;
}

bool MpeRollEditor::keyPressed (const juce::KeyPress& key)
{
    // The Tools' keys (Alt+U for Chop, as in FL) reach here from the grid, which leaves them alone.
    if (handleToolKey (key))
        return true;

    return grid.keyPressed (key);
}

//==============================================================================
void MpeRollEditor::wireToolbar()
{
    auto& synth = rollProcessor.getSynth();

    toolbar.synthButton.onClick = [this] { showSynthMenu(); };
    toolbar.showSynthButton.onClick = [this, &synth] { synth.showEditor (this); };

    toolbar.slotBox.onChange = [this]
    {
        if (! updatingToolbar)
            document.setEditSlot (toolbar.slotBox.getSelectedId() - 1);
    };

    toolbar.lengthBox.onChange = [this]
    {
        const auto bars = toolbar.lengthBox.getSelectedId();

        if (! updatingToolbar && bars > 0)
            document.perform ("Clip length", [bars] (MpeClip& c) { c.length = bars * 4.0; });
    };

    toolbar.loopButton.onClick = [this]
    {
        const auto loop = toolbar.loopButton.getToggleState();

        if (! updatingToolbar)
            document.perform (loop ? "Loop on" : "Loop off", [loop] (MpeClip& c) { c.looping = loop; });
    };

    toolbar.modeBox.onChange = [this]
    {
        if (updatingToolbar)
            return;

        auto s = document.getSettings();
        s.playback.mode = toolbar.modeBox.getSelectedId() == 2 ? PlaybackSettings::Mode::noteTrigger
                                                                : PlaybackSettings::Mode::followHost;
        document.setSettings (s);

        if (s.playback.mode == PlaybackSettings::Mode::noteTrigger)
            setStatus ("Note trigger: " + juce::MidiMessage::getMidiNoteName (s.playback.triggerBaseKey, true, true, 5)
                       + " in FL's piano roll plays clip 1, the next key clip 2, and so on.");
    };

    toolbar.recordButton.onClick = [this] { rollProcessor.setRecordArmed (toolbar.recordButton.getToggleState()); };
    toolbar.settingsButton.onClick = [this] { showSettings(); };

    for (auto [button, tool] : { std::pair { &toolbar.drawToolButton, ViewState::Tool::edit },
                                 std::pair { &toolbar.paintToolButton, ViewState::Tool::paint },
                                 std::pair { &toolbar.eraseToolButton, ViewState::Tool::erase },
                                 std::pair { &toolbar.sliceToolButton, ViewState::Tool::slice },
                                 std::pair { &toolbar.selectToolButton, ViewState::Tool::select },
                                 std::pair { &toolbar.glideToolButton, ViewState::Tool::glide } })
        button->onClick = [this, tool] { view.tool = tool; view.changed(); };

    toolbar.glideSnapButton.onClick = [this] { view.snapGlide = toolbar.glideSnapButton.getToggleState(); view.changed(); };
    toolbar.toolsButton.onClick = [this] { showToolsMenu(); };

    toolbar.snapBox.onChange = [this]
    {
        if (! updatingToolbar)
        {
            view.snap = Toolbar::snapForIndex (toolbar.snapBox.getSelectedId() - 1);
            view.changed();
        }
    };

    toolbar.undoButton.onClick = [this] { document.getUndoManager().undo(); };
    toolbar.redoButton.onClick = [this] { document.getUndoManager().redo(); };

    toolbar.midiButton.onClick = [this]
    {
        if (! midiDragListener->dragged)
            showMidiMenu();

        midiDragListener->dragged = false;
    };

    toolbar.flButton.onClick = [this] { sendToFl(); };
}

void MpeRollEditor::refreshToolbar()
{
    const juce::ScopedValueSetter<bool> svs (updatingToolbar, true);
    auto& synth = rollProcessor.getSynth();
    const auto& clip = document.getEditClip();
    const auto& settings = document.getSettings();

    if (synth.isFindingOutWhy())
        toolbar.synthButton.setButtonText ("Finding out why...");
    else if (synth.isLoading())
        toolbar.synthButton.setButtonText ("Loading...");
    else if (synth.isLoaded())
        toolbar.synthButton.setButtonText (synth.getName() + dropDownArrow);
    else if (synth.getLoadProblem().isNotEmpty())
        toolbar.synthButton.setButtonText ("Not loaded: " + synth.getMissingSynthName() + dropDownArrow);
    else if (synth.getMissingSynthName().isNotEmpty())
        toolbar.synthButton.setButtonText ("Missing: " + synth.getMissingSynthName() + dropDownArrow);
    else
        toolbar.synthButton.setButtonText ("Load synth..." + dropDownArrow);

    toolbar.showSynthButton.setEnabled (synth.isLoaded());
    toolbar.slotBox.setSelectedId (document.getEditSlot() + 1, juce::dontSendNotification);

    const auto bars = clip.length / 4.0;

    if (std::abs (bars - std::round (bars)) < 1.0e-9 && toolbar.lengthBox.indexOfItemId ((int) std::round (bars)) >= 0)
        toolbar.lengthBox.setSelectedId ((int) std::round (bars), juce::dontSendNotification);
    else
        toolbar.lengthBox.setText (juce::String (bars, 2) + " bars", juce::dontSendNotification);

    toolbar.loopButton.setToggleState (clip.looping, juce::dontSendNotification);
    toolbar.modeBox.setSelectedId (settings.playback.mode == PlaybackSettings::Mode::noteTrigger ? 2 : 1, juce::dontSendNotification);
    toolbar.snapBox.setSelectedId (Toolbar::indexForSnap (view.snap) + 1, juce::dontSendNotification);
    syncToolButtons();
    toolbar.undoButton.setEnabled (document.getUndoManager().canUndo());
    toolbar.redoButton.setEnabled (document.getUndoManager().canRedo());
    toolbar.recordButton.setToggleState (rollProcessor.isRecordArmed(), juce::dontSendNotification);

    const bool showTransient = transientStatus.isNotEmpty()
                            && juce::Time::getMillisecondCounter() - transientStatusTime < 6000;
    toolbar.status.setText (showTransient ? transientStatus : describeOutput(), juce::dontSendNotification);
}

juce::String MpeRollEditor::describeOutput() const
{
    // A failed load is retraced in the scanner, which can take a while; the plugin may open a
    // window of its own there.
    if (rollProcessor.getSynth().isFindingOutWhy())
        return findingOutWhyStatus;

    // The "No sound from ..." box asks once. This stays until the synth is heard, for a box that
    // went behind FL's window or was closed by reflex. Played another way, its advice no longer applies.
    if (auto& synth = rollProcessor.getSynth(); synth.isSilentOverMpe() && synth.isLoaded()
                                                && document.getSettings().output.mode == OutputSettings::Mode::mpe)
        return "No sound from " + synth.getName() + " yet: turn MPE on in it (Omnisphere 3: its System page), "
               "or pick Plain MIDI in MPE settings";

    const auto& s = document.getSettings().output;
    const auto& clip = document.getEditClip();
    juce::String text;

    // A glide stopped short at a bend range's edge in the last minute: what to set, until it stops.
    if (const auto stopped = describeBendOverflow (rollProcessor.getRecentBendOverflow(), s); stopped.isNotEmpty())
        return stopped;

    if (s.mode == OutputSettings::Mode::copies)
    {
        const auto copies = rollProcessor.getSynth().getCopyStatus();
        text << "One copy per note: ";

        if (! copies.on)
            text << "load a synth";
        else if (copies.loading)
            text << "loading copies, " << copies.loaded << " of " << copies.wanted;
        else
            text << copies.loaded << " copies, " << copies.playing << " playing";

        if (copies.problem.isNotEmpty())
            text << " (see MPE settings)";

        text << juce::String::fromUTF8 (", \xc2\xb1") << juce::roundToInt (s.masterBendRange) << " st";
    }
    else if (s.mode == OutputSettings::Mode::singleChannel || ! s.zones.hasAnyZone())
    {
        text << "Plain MIDI out, channel " << s.singleChannel;
    }
    else
    {
        text << "MPE out: ";

        if (s.zones.hasLowerZone())
            text << "lower zone " << s.zones.lowerMembers << " ch";

        if (s.zones.hasLowerZone() && s.zones.hasUpperZone())
            text << " + ";

        if (s.zones.hasUpperZone())
            text << "upper zone " << s.zones.upperMembers << " ch";

        text << juce::String::fromUTF8 (", \xc2\xb1") << juce::roundToInt (s.memberBendRange) << " st";
    }

    text << "   |   " << (int) clip.notes.size() << " notes";
    return text;
}

juce::String MpeRollEditor::describeBendOverflow (const MpeEncoder::BendOverflow& over, const OutputSettings& s) const
{
    if (over.isEmpty())
        return {};

    const auto& synth = rollProcessor.getSynth();
    const auto name = synth.isLoaded() ? synth.getName() : juce::String ("the synth");
    const auto reached = juce::String (over.semitonesReached());
    const auto stopsAt = juce::String (juce::roundToInt (over.range));
    const auto setTo = juce::String (over.suggestedRange());
    const auto scaledTo = [&] (float range) { return std::abs (over.range - range) < 0.01f; };

    if (s.mode == OutputSettings::Mode::mpe && s.zones.hasAnyZone())
    {
        // A note's bend is scaled to the per-note range, a zone bend to the zone range.
        if (scaledTo (s.memberBendRange))
            return "A glide reaches " + reached + " semitones but stops at " + stopsAt + ", the Per-note bend range: set it and "
                 + name + "'s MPE bend range to " + setTo + " (MPE settings)";

        return "A zone bend reaches " + reached + " semitones but stops at " + stopsAt + ", the Zone bend range: set it and "
             + name + "'s own pitch-bend range to " + setTo + " (MPE settings)";
    }

    // Plain MIDI, or one copy per note: the synth's own pitch-bend range scales every bend.
    const auto profile = synth.getProfile();
    return "Glides stop at " + stopsAt + " semitones, " + name + "'s own pitch-bend range, but one reaches " + reached + ": set "
         + name + "'s range to " + setTo + " and Zone bend range (MPE settings) to " + setTo
         + (profile.has_value() && profile->bendRange.isNotEmpty() ? "; MPE settings says where in " + name : juce::String());
}

void MpeRollEditor::setStatus (const juce::String& message)
{
    transientStatus = message;
    transientStatusTime = juce::Time::getMillisecondCounter();
    refreshToolbar();
}

void MpeRollEditor::showStatusNow (const juce::String& message)
{
    // For right before a call that holds up the message thread, like loading a synth: the window
    // puts repaints off until the message thread is free, so the status would be painted too late.
    setStatus (message);

    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
}

//==============================================================================
void MpeRollEditor::timerCallback()
{
    const auto transport = rollProcessor.getTransport();
    const bool visible = transport.playing && transport.slot == document.getEditSlot();
    grid.setPlayhead (transport.clipBeat, visible);
    ruler.setPlayhead (transport.clipBeat, visible);

    // Keep the playhead in view while playing.
    if (visible && grid.getWidth() > 0)
    {
        const auto x = view.beatToX (transport.clipBeat);

        if (x > grid.getWidth() * 0.95 || x < 0.0)
        {
            view.scrollBeat = juce::jmax (0.0, transport.clipBeat - 0.05 * grid.getWidth() / view.pixelsPerBeat);
            view.changed();
        }
    }

    if (rollProcessor.getRecordingSlot() >= 0)
        grid.refreshRecording();

    if (rollProcessor.getSynth().takeSilenceWarning())
        warnSynthSilent();

    if (rollProcessor.getSynth().isSilentOverMpe() != showingSilentStatus)
    {
        showingSilentStatus = ! showingSilentStatus;
        refreshToolbar();
    }

    // A glide that stopped short at a bend range's edge: the status line says so while it's recent.
    if (const auto over = rollProcessor.getRecentBendOverflow();
        ! juce::exactlyEqual (over.asked, shownBendOverflow.asked) || ! juce::exactlyEqual (over.range, shownBendOverflow.range))
    {
        shownBendOverflow = over;
        refreshToolbar();
    }

    // A load that failed is retraced in the scanner before its error shows, which takes a moment.
    if (rollProcessor.getSynth().isFindingOutWhy() != showingFindingOutWhy)
    {
        showingFindingOutWhy = ! showingFindingOutWhy;

        if (showingFindingOutWhy)
            showStatusNow (findingOutWhyStatus);

        refreshToolbar();
    }

    if (toolbar.recordButton.getToggleState() != rollProcessor.isRecordArmed())
        toolbar.recordButton.setToggleState (rollProcessor.isRecordArmed(), juce::dontSendNotification);

    if (transientStatus.isNotEmpty() && juce::Time::getMillisecondCounter() - transientStatusTime >= 6000)
    {
        transientStatus.clear();
        refreshToolbar();
    }

    // A host can resize, hide or rescale the window without JUCE hearing about it (FL Studio
    // reopening a plugin window), which leaves clicks landing away from what's drawn.
    if (++ticksSinceWindowCheck >= 15)
    {
        ticksSinceWindowCheck = 0;
        WindowDrawing::resync (*this);
    }
}

void MpeRollEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &skins.getObject())
    {
        // The skin has changed Theme's colours: everything takes them up and is drawn again.
        lookAndFeel.applyTheme();
        sendLookAndFeelChange();
        repaint();
        return;
    }

    if (document.getEditSlot() != lastEditSlot)
    {
        lastEditSlot = document.getEditSlot();
        scrollToContent();
    }

    refreshToolbar();
    updateScrollBars();
}

void MpeRollEditor::viewChanged()
{
    updateScrollBars();

    const juce::ScopedValueSetter<bool> svs (updatingToolbar, true);
    syncToolButtons();
    toolbar.snapBox.setSelectedId (Toolbar::indexForSnap (view.snap) + 1, juce::dontSendNotification);
}

void MpeRollEditor::syncToolButtons()
{
    toolbar.drawToolButton.setToggleState (view.tool == ViewState::Tool::edit, juce::dontSendNotification);
    toolbar.paintToolButton.setToggleState (view.tool == ViewState::Tool::paint, juce::dontSendNotification);
    toolbar.eraseToolButton.setToggleState (view.tool == ViewState::Tool::erase, juce::dontSendNotification);
    toolbar.sliceToolButton.setToggleState (view.tool == ViewState::Tool::slice, juce::dontSendNotification);
    toolbar.selectToolButton.setToggleState (view.tool == ViewState::Tool::select, juce::dontSendNotification);
    toolbar.glideToolButton.setToggleState (view.tool == ViewState::Tool::glide, juce::dontSendNotification);
    toolbar.glideSnapButton.setToggleState (view.snapGlide, juce::dontSendNotification);
}

void MpeRollEditor::updateScrollBars()
{
    if (grid.getWidth() <= 0 || grid.getHeight() <= 0)
        return;

    const auto& clip = document.getEditClip();
    const auto visibleBeats = grid.getWidth() / view.pixelsPerBeat;
    const auto totalBeats = std::max ({ clip.length, clip.contentEnd(), view.scrollBeat + visibleBeats }) + 16.0;

    hScroll.setRangeLimits (0.0, totalBeats, juce::dontSendNotification);
    hScroll.setCurrentRange (view.scrollBeat, visibleBeats, juce::dontSendNotification);

    vScroll.setRangeLimits (0.0, 128.0, juce::dontSendNotification);
    vScroll.setCurrentRange (128.0 - view.scrollKey, (double) grid.getHeight() / view.rowHeight, juce::dontSendNotification);
}

void MpeRollEditor::scrollBarMoved (juce::ScrollBar* bar, double newRangeStart)
{
    if (bar == &hScroll)
        view.scrollBeat = juce::jmax (0.0, newRangeStart);
    else
        view.scrollKey = (float) (128.0 - newRangeStart);

    view.changed();
}

void MpeRollEditor::scrollToContent()
{
    const auto& clip = document.getEditClip();
    const auto visibleRows = juce::jmax (12.0f, (float) juce::jmax (1, grid.getHeight()) / view.rowHeight);

    if (clip.notes.empty())
    {
        view.scrollKey = 60.0f + visibleRows * 0.6f;    // C5 (middle C) a little below the middle
    }
    else
    {
        int low = 127, high = 0;

        for (const auto& n : clip.notes)
        {
            low = std::min (low, n.key);
            high = std::max (high, n.key);
        }

        view.scrollKey = (float) (low + high) * 0.5f + visibleRows * 0.5f + 0.5f;
    }

    view.scrollBeat = 0.0;
    view.clampVertical ((float) juce::jmax (1, grid.getHeight()));
    view.changed();
}

//==============================================================================
void MpeRollEditor::showSynthMenu()
{
    auto& synth = rollProcessor.getSynth();
    const auto types = synth.getInstrumentTypes();

    // Two installs of one plugin would otherwise be two lines reading the same.
    const auto shown = HostedSynth::withDistinctNames (types);
    const auto* instance = synth.getInstance();
    juce::String currentId;

    if (instance != nullptr)
    {
        const auto loadedId = instance->getPluginDescription().createIdentifierString();

        for (int i = 0; i < types.size(); ++i)
            if (types[i].createIdentifierString() == loadedId)
                currentId = shown[i].createIdentifierString();
    }

    enum { scanId = -2, browseId = -3, unloadId = -4, windowBackId = -5, logId = -6 };

    juce::PopupMenu menu;

    if (types.isEmpty())
        menu.addItem (-1, "No synths found yet: scan first", false);
    else
        juce::KnownPluginList::addToMenu (menu, shown, juce::KnownPluginList::sortByManufacturer, currentId);

    menu.addSeparator();
    menu.addItem (scanId, "Scan for plugins...");
    menu.addItem (browseId, "Load plugin file...");

    if (synth.isLoaded())
    {
        menu.addItem (windowBackId, "Bring the synth's window back on screen");
        menu.addItem (unloadId, "Unload synth");
    }

    menu.addSeparator();
    menu.addItem (logId, "Show the plugin log (what happened when synths loaded)");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolbar.synthButton),
                        [safe = juce::Component::SafePointer<MpeRollEditor> (this), types, shown] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result == scanId)
            safe->showPluginScanner();
        else if (result == browseId)
            safe->browseForPlugin();
        else if (result == unloadId)
            safe->rollProcessor.getSynth().unload();
        else if (result == windowBackId)
            safe->rollProcessor.getSynth().bringEditorOnScreen (safe.getComponent());
        else if (result == logId)
            PluginLog::reveal();
        else if (const auto index = juce::KnownPluginList::getIndexChosenByMenu (shown, result); index >= 0)
            safe->loadSynth (types[index]);
    });
}

void MpeRollEditor::loadSynth (const juce::PluginDescription& description)
{
    showStatusNow ("Loading " + description.name + "...");

    rollProcessor.getSynth().load (description, [processor = &rollProcessor, description] (const juce::String& error)
    {
        // Answered by whichever editor is open by then: finding out why a load failed takes a moment,
        // and this one may have been closed meanwhile. (The synth only calls back while it, and so the
        // processor holding it, is still there.)
        auto* editor = dynamic_cast<MpeRollEditor*> (processor->getActiveEditor());

        if (error.isEmpty())
            processor->requestConfiguration();

        if (editor == nullptr)
            return;

        if (error.isNotEmpty())
        {
            editor->setStatus ("Couldn't load " + description.name);
            editor->showLoadFailure (error, &description);
            return;
        }

        editor->setStatus (editor->describeLoaded (description.name));
        editor->offerPlainMidi();
    });

    refreshToolbar();
}

juce::String MpeRollEditor::describeLoaded (const juce::String& name) const
{
    // A synth played with MPE over MIDI only gets per-note expression if it's in MPE mode.
    const auto& output = document.getSettings().output;
    const auto& synth = rollProcessor.getSynth();

    if (output.mode == OutputSettings::Mode::copies)
        return "Loaded " + name + ": loading " + juce::String (output.copies - 1) + " more copies of it, one at a time";

    if (output.mode != OutputSettings::Mode::mpe)
        return "Loaded " + name;

    if (synth.playsNoteExpressions (output))
        return "Loaded " + name + (synth.getVst3Support().isVst3 ? ", played with VST3 note expressions" : "");

    // In the order MPE settings has it: no MPE at all, then MPE that can't reach its notes, then
    // where its MPE switch is.
    const auto profile = synth.getProfile();

    if (profile.has_value() && profile->mpe == SynthProfile::Mpe::none)
        return "Loaded " + name + ". " + profile->name + " " + profile->describeNone()
             + ": set Synth expects to Plain MIDI in MPE settings.";

    if (synth.getVst3Support().midiMissesNotes())
        return "Loaded " + name + ". MPE can't reach its notes one by one (see MPE settings): set Synth expects to Plain MIDI.";

    if (profile.has_value())
        return "Loaded " + name + ". " + profile->advice;

    return "Loaded " + name + ". Turn on MPE in it; if it has no MPE mode, set Synth expects to Plain MIDI in MPE settings.";
}

void MpeRollEditor::offerPlainMidi()
{
    auto& synth = rollProcessor.getSynth();
    const auto& output = document.getSettings().output;

    // One box at a time: the "No sound" box may be asking already.
    if (mpeAdvice != nullptr || silenceAlert != nullptr || ! synth.isLoaded() || output.mode != OutputSettings::Mode::mpe
        || synth.playsNoteExpressions (output))
        return;

    // Only for a synth that can't take each note's own expression, whatever is set in it: one
    // MPE Roll knows has no MPE, or a VST3 that takes pitch bend on one channel and no note
    // expressions (that this build can send).
    const auto profile = synth.getProfile();
    const auto& vst3 = synth.getVst3Support();
    const auto name = synth.getName();
    juce::String why, title;

    if (profile.has_value() && profile->mpe == SynthProfile::Mpe::none)
    {
        why = profile->advice.upToLastOccurrenceOf ("Set Synth expects", false, false).trim();
        title = name + " " + profile->describeNone();
    }
    else if (vst3.midiMissesNotes() && ! (vst3.takesTuning() && Vst3NoteExpressions::isAvailable()))
    {
        why = name + " takes pitch bend " + vst3.describeBend() + (vst3.takesTuning() ? juce::String() : " and no VST3 note expressions")
              + ", so MPE Roll can't reach its notes one by one. If it has an MPE switch of its own, turning it on may "
                "change that, and MPE settings would then say so.";
        title = "MPE can't reach " + name + "'s notes";
    }
    else
    {
        return;
    }

    PluginLog::add ("Offered Plain MIDI for " + name + ": " + why);

    const auto message = why + "\n\nOver MPE, each note's own glides, pressure and timbre can't reach it: only the "
                         "zone-wide lanes would. With Plain MIDI on one channel, the newest note's glides, pressure and "
                         "timbre drive the synth. With one copy per note, MPE Roll loads several copies of " + name
                         + " and plays each note on a copy of its own, so every note keeps its own glides, pressure and "
                         "timbre (each copy takes the CPU and memory of one " + name + ").\n\nPlay " + name + " with Plain MIDI?";

    // Return takes the suggestion; Escape and closing the box keep MPE.
    auto* alert = new juce::AlertWindow (title, message, juce::MessageBoxIconType::QuestionIcon, this);
    alert->addButton ("Use Plain MIDI", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton ("One copy per note", 2);
    alert->addButton ("Keep MPE", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    mpeAdvice = alert;
    alert->setAlwaysOnTop (true);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MpeRollEditor> (this), name] (int result)
    {
        if (safe != nullptr && result != 0)
            safe->playWithoutMpe (name, result == 2);
    }), true);

    alert->toFront (true);
}

void MpeRollEditor::warnSynthSilent()
{
    if (silenceAlert != nullptr || mpeAdvice != nullptr)
        return;     // one box is already asking (the status line keeps saying it hasn't been heard)

    auto& synth = rollProcessor.getSynth();
    const auto& output = document.getSettings().output;

    // The settings may have changed since the audio thread noticed.
    if (! synth.isLoaded() || output.mode != OutputSettings::Mode::mpe || synth.playsNoteExpressions (output))
        return;

    const auto name = synth.getName();
    PluginLog::add ("No sound from " + name + " over MPE on channels " + output.zones.describeMemberChannels());

    const auto message = name + " hasn't made a sound in the last few seconds of notes MPE Roll has played to it.\n\n"
                         "MPE Roll plays each note on its own MIDI channel (" + output.zones.describeMemberChannels()
                         + "). A synth that isn't in MPE mode usually listens on channel 1 only, so it hears none of "
                         "them. Or the synth may still be loading its sounds.\n\n"
                         "To keep MPE, turn it on in " + name + " if it's off (in Omnisphere 3, on its System page). "
                         "Or play the synth with Plain MIDI on one channel, where only the newest note gets glides, "
                         "pressure and timbre; or with one copy of it per note, where every note keeps its own.";

    // Only the Use Plain MIDI and One copy per note buttons change the project. Keep MPE takes Return
    // and Escape both, and closing the box counts as Keep MPE too, so a reflex key or a closed window
    // changes nothing. (JUCE's own two-button box would give Escape and the close button to the second button.)
    auto* alert = new juce::AlertWindow ("No sound from " + name, message, juce::MessageBoxIconType::WarningIcon, this);
    alert->addButton ("Keep MPE", 0, juce::KeyPress (juce::KeyPress::returnKey), juce::KeyPress (juce::KeyPress::escapeKey));
    alert->addButton ("Use Plain MIDI", 1);
    alert->addButton ("One copy per note", 2);
    silenceAlert = alert;

    // Plugin windows aren't owned by the host's, so FL's window would otherwise cover the box.
    alert->setAlwaysOnTop (true);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MpeRollEditor> (this), name] (int result)
    {
        if (safe != nullptr && result != 0)
            safe->playWithoutMpe (name, result == 2);
    }), true);

    alert->toFront (true);
}

void MpeRollEditor::playWithoutMpe (const juce::String& name, bool oneCopyPerNote)
{
    auto settings = document.getSettings();
    settings.output.mode = oneCopyPerNote ? OutputSettings::Mode::copies : OutputSettings::Mode::singleChannel;
    document.setSettings (settings);

    // Either way the synth's own pitch-bend range scales every glide, and most synths start at 2.
    const auto bend = ". Glides stop at " + name + "'s own pitch-bend range (usually 2 semitones): set it to 12 or 24, and "
                      "Zone bend range (MPE settings) to the same";
    setStatus (oneCopyPerNote ? "Playing " + name + " with one copy per note: loading " + juce::String (settings.output.copies - 1)
                                    + " more copies, one at a time" + bend
                              : "Playing " + name + " with Plain MIDI on channel " + juce::String (settings.output.singleChannel) + bend);
}

void MpeRollEditor::showLoadFailure (const juce::String& error, const juce::PluginDescription* plugin)
{
    // With what to send along: the box can copy the details, and the plugin log has the rest.
    const auto details = PluginLog::report (plugin, error);
    auto* alert = new juce::AlertWindow ("Couldn't load " + (plugin != nullptr ? plugin->name : juce::String ("the synth")),
                                         error + "\n\nCopy details puts what happened, and the end of MPE Roll's plugin log, "
                                                 "on the clipboard, to paste into a message.",
                                         juce::MessageBoxIconType::WarningIcon, this);
    alert->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey), juce::KeyPress (juce::KeyPress::escapeKey));
    alert->addButton ("Copy details", 1);
    alert->setAlwaysOnTop (true);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MpeRollEditor> (this), details] (int result)
    {
        if (result != 1)
            return;

        juce::SystemClipboard::copyTextToClipboard (details);

        if (safe != nullptr)
            safe->setStatus ("The details are on the clipboard.");
    }), true);

    alert->toFront (true);
}

void MpeRollEditor::showPluginScanner()
{
    // The synth owns the window, so it closes with the synth rather than scanning into one that's gone.
    rollProcessor.getSynth().showScanWindow (Theme::panel);
}

void MpeRollEditor::browseForPlugin()
{
    auto defaultFolder = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);

   #if JUCE_WINDOWS
    defaultFolder = juce::File ("C:\\Program Files\\Common Files\\VST3");
   #elif JUCE_MAC
    defaultFolder = juce::File ("/Library/Audio/Plug-Ins/VST3");
   #else
    defaultFolder = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".vst3");
   #endif

    // A synth can be a file or a bundle folder. Asked for both, Windows' own dialog only picks
    // folders, so JUCE's chooser is used there instead.
   #if JUCE_WINDOWS
    constexpr auto useNativeDialog = false;
   #else
    constexpr auto useNativeDialog = true;
   #endif
    chooser = std::make_unique<juce::FileChooser> ("Choose a synth", defaultFolder, HostedSynth::getPluginFilePatterns(),
                                                   useNativeDialog);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer<MpeRollEditor> (this)] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (safe == nullptr || file == juce::File())
            return;

        // The file is checked in the scanner first, which can take a moment.
        safe->setStatus ("Checking " + file.getFileName() + "...");
        safe->rollProcessor.getSynth().loadFromFile (file, [processor = &safe->rollProcessor, file] (const juce::String& error)
        {
            // As for loadSynth: answered by the editor open by then, if any.
            processor->requestConfiguration();
            auto* editor = dynamic_cast<MpeRollEditor*> (processor->getActiveEditor());

            if (editor == nullptr)
                return;

            if (error.isNotEmpty())
            {
                juce::PluginDescription fileOnly;
                fileOnly.name = file.getFileNameWithoutExtension();
                fileOnly.fileOrIdentifier = file.getFullPathName();
                editor->showLoadFailure (error, &fileOnly);
            }
            else
            {
                editor->setStatus (editor->describeLoaded (file.getFileNameWithoutExtension()));
                editor->offerPlainMidi();
            }
        },
        [safe, file]
        {
            // Loading the synth itself holds up the message thread, so the status has to change now.
            if (safe != nullptr)
                safe->showStatusNow ("Loading " + file.getFileNameWithoutExtension() + "...");
        });
    });
}

//==============================================================================
void MpeRollEditor::showMidiMenu()
{
    const auto hasSynth = rollProcessor.getSynth().isLoaded();

    juce::PopupMenu menu;
    menu.addItem (1, "Import MIDI file...");
    menu.addItem (2, "Export MPE MIDI file...");
    menu.addSeparator();
    menu.addItem (4, "Export audio as WAV...", hasSynth);
    menu.addItem (5, juce::String ("Export audio as MP3...") + (AudioExport::canWriteMp3() ? "" : " (not in this build)"),
                  hasSynth && AudioExport::canWriteMp3());

    if (! hasSynth)
        menu.addItem (6, "(Load a synth to export audio: the clip is played through it)", false);

    menu.addSeparator();
    menu.addItem (3, "Tip: drag this button into FL to drop the clip as a MIDI file", false);
    menu.addSeparator();
    menu.addItem (7, "Support MPE Roll (donate)");
    menu.addItem (8, getBuildName(), false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolbar.midiButton),
                        [safe = juce::Component::SafePointer<MpeRollEditor> (this)] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 7)
        {
            DonatePanel::show (safe.getComponent());
            return;
        }

        if (result == 1)
            safe->importMidi();
        else if (result == 2)
            safe->exportMidi();
        else if (result == 4)
            safe->exportAudio (AudioExport::Format::wav);
        else if (result == 5)
            safe->exportAudio (AudioExport::Format::mp3);
    });
}

void MpeRollEditor::exportAudio (AudioExport::Format format)
{
    const auto& clip = document.getEditClip();
    const auto name = clip.name.isNotEmpty() ? clip.name : juce::String ("MPE Roll clip");
    const auto extension = format == AudioExport::Format::mp3 ? juce::String (".mp3") : juce::String (".wav");

    chooser = std::make_unique<juce::FileChooser> (format == AudioExport::Format::mp3 ? "Export audio as MP3" : "Export audio as WAV",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (juce::File::createLegalFileName (name) + extension),
                                                   "*" + extension);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<MpeRollEditor> (this), format, extension] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        const auto target = fc.getResult().withFileExtension (extension);
        const auto transport = safe->rollProcessor.getTransport();
        const auto settings = safe->document.getSettings();

        AudioExport::Options options;
        options.format = format;
        options.sampleRate = safe->rollProcessor.getSampleRate() > 0.0 ? safe->rollProcessor.getSampleRate() : 48000.0;
        options.bpm = transport.bpm;
        options.controlIntervalMs = settings.playback.controlIntervalMs;
        options.title = safe->document.getEditClip().name;

        safe->setStatus ("Exporting " + target.getFileName() + "...");

        AudioExport::exportClip (safe->rollProcessor, safe->document.getEditClip(), target, options,
                                 [safe, target] (const juce::String& error)
        {
            if (safe == nullptr)
                return;

            if (error.isEmpty())
            {
                safe->setStatus ("Exported " + target.getFileName());
                target.revealToUser();
            }
            else
            {
                safe->setStatus ("Export: " + error.upToFirstOccurrenceOf (".", true, false));
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export audio", error);
            }
        });
    });
}

void MpeRollEditor::importMidi()
{
    chooser = std::make_unique<juce::FileChooser> ("Import MIDI file",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MpeRollEditor> (this)] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult().existsAsFile())
            safe->importMidiFile (fc.getResult());
    });
}

void MpeRollEditor::importMidiFile (const juce::File& file)
{
    juce::FileInputStream stream (file);

    if (! stream.openedOk())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import MIDI",
                                                "Couldn't open " + file.getFileName() + ".");
        return;
    }

    auto result = MidiFileIO::importClip (stream, document.getSettings().input);

    if (! result.ok)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import MIDI", result.error);
        return;
    }

    if (result.clip.name.isEmpty())
        result.clip.name = file.getFileNameWithoutExtension();

    const auto count = (int) result.clip.notes.size();
    document.replaceClip (document.getEditSlot(), std::move (result.clip), "Import MIDI");
    document.selection.clear();
    scrollToContent();
    setStatus ("Imported " + juce::String (count) + " notes from " + file.getFileName());
}

void MpeRollEditor::exportMidi()
{
    const auto& clip = document.getEditClip();
    const auto name = clip.name.isNotEmpty() ? clip.name : juce::String ("MPE Roll clip");

    chooser = std::make_unique<juce::FileChooser> ("Export MPE MIDI file",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (juce::File::createLegalFileName (name) + ".mid"),
                                                   "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<MpeRollEditor> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;

        const auto target = fc.getResult().withFileExtension (".mid");
        MidiFileIO::ExportOptions options;
        options.tempoBpm = safe->rollProcessor.getTransport().bpm;

        juce::FileOutputStream out (target);

        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            MidiFileIO::writeClip (safe->document.getEditClip(), safe->fileOutputSettings(), options, out);
            safe->setStatus ("Exported " + target.getFileName());
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export MIDI",
                                                    "Couldn't write " + target.getFullPathName());
        }
    });
}

juce::File MpeRollEditor::writeTemporaryMidiFile()
{
    const auto& clip = document.getEditClip();
    const auto name = juce::File::createLegalFileName (clip.name.isNotEmpty() ? clip.name : juce::String ("MPE Roll clip"));
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("MPE Roll").getChildFile (name + ".mid");
    file.getParentDirectory().createDirectory();

    MidiFileIO::ExportOptions options;
    options.tempoBpm = rollProcessor.getTransport().bpm;
    options.loopOnce = true;

    juce::FileOutputStream out (file);

    if (! out.openedOk())
        return {};

    out.setPosition (0);
    out.truncate();
    MidiFileIO::writeClip (clip, fileOutputSettings(), options, out);
    return file;
}

OutputSettings MpeRollEditor::fileOutputSettings() const
{
    // One copy per note plays the synth from an MPE stream with the Zone bend range for every
    // note. A file is for other tools: plain MPE, with the zones and per-note bend range as set.
    auto output = document.getSettings().output;

    if (output.mode == OutputSettings::Mode::copies)
        output.mode = OutputSettings::Mode::mpe;

    return output;
}

void MpeRollEditor::sendToFl()
{
    const auto& clip = document.getEditClip();

    if (clip.notes.empty())
    {
        setStatus ("Nothing to send: this clip has no notes.");
        return;
    }

    FlScriptExporter::Options options;
    options.title = clip.name;
    options.tempoBpm = rollProcessor.getTransport().bpm;
    auto result = std::make_shared<FlScriptExporter::Result> (FlScriptExporter::generate (clip, options));

    auto write = [safe = juce::Component::SafePointer<MpeRollEditor> (this), result] (const juce::File& target)
    {
        if (safe == nullptr)
            return;

        if (! target.replaceWithText (result->script))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Send to FL piano roll",
                                                    "Couldn't write " + target.getFullPathName());
            return;
        }

        juce::String message;
        message << "Saved \"" << target.getFileNameWithoutExtension() << "\" (" << (int) result->notes.size()
                << " notes, " << result->numSlides << " slide notes) to\n" << target.getParentDirectory().getFullPathName()
                << "\n\nIn FL Studio, open the piano roll of a channel with a native synth (FLEX, Sytrus, Harmor...), "
                   "then pick \"" << target.getFileNameWithoutExtension() << "\" from the piano roll's Scripts menu"
                << ". If it isn't listed yet, reopen the menu or restart FL once."
                << "\n\nGlides become slide notes, so every overlapping note gets its own colour. "
                   "Pressure and timbre become Mod X / Mod Y: map those in the synth's per-note modulation.";

        if (result->numColourConflicts > 0)
            message << "\n\nNote: " << result->numColourConflicts << " notes overlap more than 16 others, so they "
                    << "share a colour and their glides may bend each other.";

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Sent to FL piano roll", message);
        safe->setStatus ("FL script written: " + target.getFileName());
    };

    const auto folder = FlScriptExporter::defaultScriptFolder();

    if (folder.isDirectory())
    {
        write (folder.getChildFile (FlScriptExporter::defaultScriptFileName));
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Save FL Studio piano roll script",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (FlScriptExporter::defaultScriptFileName),
                                                   "*.pyscript");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [write] (const juce::FileChooser& fc)
    {
        if (fc.getResult() != juce::File())
            write (fc.getResult().withFileExtension (".pyscript"));
    });
}

void MpeRollEditor::showSettings()
{
    auto panel = std::make_unique<SettingsPanel> (rollProcessor);
    const auto area = toolbar.settingsButton.getBounds() + toolbar.getPosition();
    settingsBox = &juce::CallOutBox::launchAsynchronously (std::move (panel), area, this);
}

//==============================================================================
bool MpeRollEditor::handleToolKey (const juce::KeyPress& key)
{
    for (const auto& tool : NoteToolDialog::all())
    {
        if (NoteToolDialog::matches (tool.quickKey, key))
        {
            applyTool (tool.kind);
            return true;
        }

        if (NoteToolDialog::matches (tool.key, key))
        {
            openTool (tool.kind);
            return true;
        }
    }

    return false;
}

void MpeRollEditor::showToolsMenu()
{
    // FL's piano roll Tools menu: the quick version of a tool first, where there is one, then its dialog.
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<MpeRollEditor> (this);
    int id = 1;

    for (const auto& tool : NoteToolDialog::all())
    {
        if (tool.quickKey.isValid())
        {
            juce::PopupMenu::Item quick ("Quick " + juce::String (tool.name).toLowerCase());
            quick.itemID = id++;
            quick.shortcutKeyDescription = NoteToolDialog::describeKey (tool.quickKey);
            quick.action = [safe, kind = tool.kind] { if (safe != nullptr) safe->applyTool (kind); };
            menu.addItem (quick);
        }

        juce::PopupMenu::Item item (juce::String (tool.name) + "...");
        item.itemID = id++;
        item.shortcutKeyDescription = NoteToolDialog::describeKey (tool.key);
        item.action = [safe, kind = tool.kind] { if (safe != nullptr) safe->openTool (kind); };
        menu.addItem (item);

        if (tool.kind != NoteToolDialog::Kind::scaleLevels)
            menu.addSeparator();
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (toolbar.toolsButton));
}

void MpeRollEditor::applyTool (NoteToolDialog::Kind kind)
{
    closeToolDialog (false);

    if (document.getEditClip().notes.empty())
    {
        setStatus ("Nothing for " + juce::String (NoteToolDialog::info (kind).name) + " to work on: this clip has no notes.");
        return;
    }

    NoteToolDialog::applyNow (kind, document, view.snap > 0.0 ? view.snap : 0.25);
    setStatus (juce::String (NoteToolDialog::info (kind).name) + " applied (Ctrl+Z undoes it).");
}

void MpeRollEditor::openTool (NoteToolDialog::Kind kind)
{
    closeToolDialog (false);

    if (document.getEditClip().notes.empty())
    {
        setStatus ("Nothing for " + juce::String (NoteToolDialog::info (kind).name) + " to work on: this clip has no notes.");
        return;
    }

    auto dialog = std::make_unique<NoteToolDialog> (kind, document, view.snap > 0.0 ? view.snap : 0.25);
    dialog->onFinished = [safe = juce::Component::SafePointer<MpeRollEditor> (this)]
    {
        if (safe != nullptr)
            if (auto* box = safe->toolsBox.getComponent())
                box->dismiss();
    };

    const auto area = toolbar.toolsButton.getBounds() + toolbar.getPosition();
    toolsBox = &juce::CallOutBox::launchAsynchronously (std::move (dialog), area, this);
}

void MpeRollEditor::closeToolDialog (bool detachFirst)
{
    if (auto* box = toolsBox.getComponent())
    {
        if (detachFirst)
            for (auto* child : box->getChildren())
                if (auto* dialog = dynamic_cast<NoteToolDialog*> (child))
                    dialog->detach();

        box->exitModalState (0);
        toolsBox = nullptr;
    }
}

//==============================================================================
bool MpeRollEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".mid") || f.endsWithIgnoreCase (".midi"))
            return true;

    return false;
}

void MpeRollEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
    {
        if (f.endsWithIgnoreCase (".mid") || f.endsWithIgnoreCase (".midi"))
        {
            importMidiFile (juce::File (f));
            return;
        }
    }
}

} // namespace mperoll
