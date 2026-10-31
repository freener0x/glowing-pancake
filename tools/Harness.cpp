// Developer harness for MPE Roll (built with -DMPEROLL_BUILD_DEV_TOOLS=ON).
//
//   MpeRollHarness snapshot <out.png> [width height]   renders the editor with a demo clip
//   MpeRollHarness settings <out.png>                   renders the MPE settings panel
//   MpeRollHarness skins <folder>                       renders the editor in every skin, and a sheet of them
//   MpeRollHarness export <synth> <folder>              exports a clip as WAV and MP3 and reads them back
//   MpeRollHarness render <synth.vst3> <out.wav>        plays a test clip through a hosted synth and
//                                                       checks that per-note glides reach it
//   MpeRollHarness selftest                             live input, recording, state, note trigger, undo
//   MpeRollHarness uitest                               simulated mouse and keyboard editing
//   MpeRollHarness params <synth.vst3>                  synth parameters exposed for host automation
//   MpeRollHarness copies <plain synth>                 one copy per note, through a synth without MPE
//   MpeRollHarness crashtest <report.txt> [overflow]    Windows: crashes on purpose (or overflows the stack)
//                                                       to check the crash report
//   MpeRollHarness scantest <scanner> <synth> <crashing.clap> <hanging.clap>
//                                                       plugin files are looked at in the scanner process
//   MpeRollHarness loadfailtest <scanner> <synth.vst3> <VST2 synth>
//                                                       a load that fails says why

#include "plugin/BuildInfo.h"
#include "plugin/CopySafety.h"
#include "plugin/CrashReport.h"
#include "plugin/Links.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginLog.h"
#include "plugin/PluginProcessor.h"
#include "plugin/PluginScanner.h"
#include "plugin/ui/DonatePanel.h"
#include "plugin/ui/NoteToolDialog.h"
#include "plugin/ui/SettingsPanel.h"
#include "plugin/ui/WindowPlacement.h"
#include "plugin/ui/Toolbar.h"
#include "plugin/ui/WindowDrawing.h"

#if MPEROLL_HOST_CLAP
 #include "plugin/clap/ClapHosting.h"
#endif

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

#if JUCE_LINUX
 #include <dlfcn.h>
#endif

namespace
{
using namespace mperoll;

/** JUCE only catches X errors in apps with a JUCEApplication, and Xlib's own handler ends the process.
    Under Xvfb there's no window manager, so some of the window properties JUCE sets are refused. */
void ignoreXErrors()
{
   #if JUCE_LINUX
    using Handler = int (*) (void*, void*);
    using SetHandler = Handler (*) (Handler);

    if (auto* x11 = dlopen ("libX11.so.6", RTLD_LAZY))
        if (auto setHandler = reinterpret_cast<SetHandler> (dlsym (x11, "XSetErrorHandler")))
            setHandler ([] (void*, void*) { return 0; });
   #endif
}

MpeNote note (int key, double start, double length, float strike)
{
    MpeNote n;
    n.key = key;
    n.start = start;
    n.length = length;
    n.strike = strike;
    return n;
}

MpeClip makeDemoClip()
{
    MpeClip clip;
    clip.name = "Demo";
    clip.length = 16.0;

    // A chord whose top note scoops up and whose root swells.
    auto root = note (55, 0.0, 3.5, 0.75f);
    root.lanes.pressure = { { 0.0, 0.1f }, { 1.5, 0.9f }, { 3.5, 0.3f } };
    root.lanes.timbre = { { 0.0, 0.2f }, { 3.5, 0.8f } };
    clip.addNote (root);

    clip.addNote (note (59, 0.0, 3.5, 0.6f));

    auto top = note (62, 0.0, 3.5, 0.9f);
    top.lanes.pitch = { { 0.0, -2.0f }, { 0.6, 0.0f }, { 2.6, 0.0f }, { 3.5, 2.0f } };
    top.lanes.pressure = { { 0.0, 0.3f }, { 1.0, 0.7f }, { 3.5, 0.5f } };
    clip.addNote (top);

    // A melody whose vibrato fades in, and a long glide.
    auto lead = note (67, 4.0, 2.0, 0.85f);
    lead.lanes.vibrato = { { 0.0, 0.0f }, { 0.5, 0.0f }, { 1.0, 0.35f } };
    lead.lanes.pressure = { { 0.0, 0.2f }, { 1.0, 1.0f }, { 2.0, 0.4f } };
    clip.addNote (lead);

    auto glide = note (69, 6.0, 2.0, 0.7f);
    glide.lanes.pitch = { { 0.0, 0.0f }, { 0.5, 0.0f }, { 1.5, 5.0f }, { 2.0, 5.0f } };
    glide.lanes.timbre = { { 0.0, 0.5f }, { 2.0, 1.0f } };
    clip.addNote (glide);

    for (int i = 0; i < 4; ++i)
    {
        auto n = note (60 + (i % 2) * 3, 8.0 + i, 0.75, 0.5f + 0.1f * (float) i);
        n.lanes.pressure = { { 0.0, 0.0f }, { 0.3, 0.6f + 0.1f * (float) i }, { 0.75, 0.0f } };
        clip.addNote (n);
    }

    auto fall = note (72, 12.0, 3.0, 1.0f);
    fall.lanes.pitch = { { 0.0, 0.0f }, { 2.0, 0.0f }, { 3.0, -7.0f } };
    clip.addNote (fall);

    clip.zone.get (LaneId::cc (64)) = { { 0.0, 1.0f }, { 3.9, 1.0f }, { 3.95, 0.0f } };
    return clip;
}

bool writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    return out.openedOk() && png.writeImageToStream (image, out);
}

template <typename ComponentType>
ComponentType* findChild (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
        if (auto* match = dynamic_cast<ComponentType*> (child))
            return match;

    return nullptr;
}

int snapshot (const juce::File& target, int width, int height)
{
    MpeRollProcessor processor;
    auto& document = processor.getDocument();
    document.replaceClip (0, makeDemoClip(), "Demo");

    for (const auto& n : document.getEditClip().notes)
        if (n.key == 62 || n.key == 67)
            document.selection.insert (n.id);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
    editor->setSize (width, height);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

    const auto ok = writePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f), target);
    processor.editorBeingDeleted (editor.get());
    editor.reset();
    std::printf ("%s %s\n", ok ? "Wrote" : "Failed to write", target.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}

int settingsSnapshot (const juce::File& target, const juce::String& openSections)
{
    MpeRollProcessor processor;
    MpeLookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    {
        SettingsPanel panel (processor);
        panel.setSize (460, 980);

        // Given section names (comma-separated), only those are open.
        if (auto* sections = findChild<juce::PropertyPanel> (panel); sections != nullptr && openSections.isNotEmpty())
        {
            const auto wanted = juce::StringArray::fromTokens (openSections, ",", "");
            const auto names = sections->getSectionNames();

            for (int i = 0; i < names.size(); ++i)
                sections->setSectionOpen (i, wanted.contains (names[i]));
        }

        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        writePng (panel.createComponentSnapshot (panel.getLocalBounds(), true, 1.0f), target);
    }

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    std::printf ("Wrote %s\n", target.getFullPathName().toRawUTF8());
    return 0;
}

/** A test picture: a gradient with diagonal stripes, so it's plain to see whether and how strongly it's drawn. */
juce::Image testPicture()
{
    juce::Image image (juce::Image::RGB, 320, 200, true);
    juce::Graphics g (image);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a6cc4), 0.0f, 0.0f, juce::Colour (0xffc4462a), 320.0f, 200.0f, false));
    g.fillAll();
    g.setColour (juce::Colours::white.withAlpha (0.5f));

    for (int x = -200; x < 320; x += 40)
        g.drawLine ((float) x, 200.0f, (float) x + 200.0f, 0.0f, 8.0f);

    return image;
}

/** Renders the editor in every skin, once more with a background picture, and a sheet of them all. */
int skinsSnapshots (const juce::File& folder)
{
    folder.createDirectory();
    juce::SharedResourcePointer<SkinManager> skins;
    const auto look = folder.getChildFile ("look.settings");
    look.deleteFile();
    skins->useSettingsFile (look);

    MpeRollProcessor processor;
    auto& document = processor.getDocument();
    document.replaceClip (0, makeDemoClip(), "Demo");

    for (const auto& n : document.getEditClip().notes)
        if (n.key == 62 || n.key == 67)
            document.selection.insert (n.id);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
    editor->setSize (1200, 760);
    auto& mm = *juce::MessageManager::getInstance();
    mm.runDispatchLoopUntil (300);

    const auto& presets = presetSkins();
    constexpr int thumbWidth = 600, thumbHeight = 380, columns = 2;
    const auto count = (int) presets.size() + 1;
    juce::Image sheet (juce::Image::RGB, thumbWidth * columns, thumbHeight * ((count + columns - 1) / columns), true);
    juce::Graphics sheetGraphics (sheet);
    sheetGraphics.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

    for (int i = 0; i < count; ++i)
    {
        const auto withPicture = i == (int) presets.size();

        if (withPicture)
        {
            const auto pictureFile = folder.getChildFile ("picture.png");
            writePng (testPicture(), pictureFile);
            skins->setPreset (0);
            skins->setBackgroundFile (pictureFile);
            skins->setBackgroundStrength (0.4f);
        }
        else
        {
            skins->setPreset (i);
        }

        mm.runDispatchLoopUntil (100);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
        const auto name = withPicture ? juce::String ("MPE Roll with a picture") : presets[(size_t) i].name;
        writePng (image, folder.getChildFile ("skin-" + juce::String (i + 1).paddedLeft ('0', 2) + "-"
                                              + name.toLowerCase().replaceCharacter (' ', '-') + ".png"));

        juce::Rectangle<int> cell ((i % columns) * thumbWidth, (i / columns) * thumbHeight, thumbWidth, thumbHeight);
        sheetGraphics.drawImage (image, cell.toFloat(), juce::RectanglePlacement::stretchToFit);
        const auto label = cell.removeFromBottom (26).removeFromLeft (230);
        sheetGraphics.setColour (juce::Colours::black.withAlpha (0.7f));
        sheetGraphics.fillRect (label);
        sheetGraphics.setColour (juce::Colours::white);
        sheetGraphics.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        sheetGraphics.drawText (juce::String (i + 1) + ". " + name, label.reduced (8, 0), juce::Justification::centredLeft);
    }

    const auto sheetFile = folder.getChildFile ("skins.png");
    const auto ok = writePng (sheet, sheetFile);

    skins->clearBackground();
    skins->setPreset (0);
    skins->flush();
    processor.editorBeingDeleted (editor.get());
    editor.reset();
    std::printf ("%s %s\n", ok ? "Wrote" : "Failed to write", sheetFile.getFullPathName().toRawUTF8());
    return ok ? 0 : 1;
}

//==============================================================================
struct FakePlayHead : juce::AudioPlayHead
{
    double ppq = 0.0;
    double bpm = 120.0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        info.setTimeInSamples ((juce::int64) (ppq / bpm * 60.0 * 48000.0));
        return info;
    }
};

double goertzel (const float* samples, int numSamples, double frequency, double sampleRate)
{
    const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const auto coefficient = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // Hann window to keep neighbouring notes from leaking into each other.
        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (numSamples - 1));
        const auto s = samples[i] * window + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    return std::sqrt (s1 * s1 + s2 * s2 - coefficient * s1 * s2) / numSamples;
}

struct Check
{
    int failures = 0;

    void operator() (bool ok, const char* what)
    {
        std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        failures += ok ? 0 : 1;
    }

    void operator() (bool ok, const juce::String& what)     { (*this) (ok, what.toRawUTF8()); }
};

/** The average frequency of a sine between two samples, from its upward zero crossings. */
double sineFrequency (const juce::AudioBuffer<float>& audio, int from, int to, double sampleRate)
{
    const auto* data = audio.getReadPointer (0);
    double first = -1.0, last = -1.0;
    int cycles = -1;

    for (auto i = juce::jmax (1, from); i < juce::jmin (to, audio.getNumSamples()); ++i)
    {
        if (data[i - 1] < 0.0f && data[i] >= 0.0f)
        {
            last = i - 1 + data[i - 1] / (data[i - 1] - data[i]);
            first = first < 0.0 ? last : first;
            ++cycles;
        }
    }

    return cycles > 0 ? sampleRate * cycles / (last - first) : 0.0;
}

/** Exports a clip as WAV and MP3 through a copy of the synth, the way the File menu does, and reads both back. */
bool loadSynth (MpeRollProcessor&, const juce::File&);   // further down

/** Checks for the export fixes: a cancel while the file is written, the plugin going mid-export,
    and (CLAP) the synth copy being told it renders offline.
*/
void exportFixChecks (Check& check, const juce::File& synthFile, const juce::File& folder, double sampleRate)
{
    auto& mm = *juce::MessageManager::getInstance();

    std::printf ("Cancelling while the file is written\n");
    {
        // Three seconds of noise: long enough to be written in several pieces.
        juce::AudioBuffer<float> audio (2, (int) (3.0 * sampleRate));
        juce::Random random (1);

        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                audio.setSample (ch, i, random.nextFloat() * 0.2f - 0.1f);

        auto cancelPartWay = [&] (const juce::File& file, auto&& write)
        {
            file.replaceWithText ("what was there before");
            const auto filesBefore = folder.getNumberOfChildFiles (juce::File::findFiles);
            int asked = 0;

            // Cancelled the second time it asks: after the first piece.
            const auto result = write (file, [&] { return ++asked > 1; });

            check (result == "Export cancelled.", file.getFileExtension() + ": the export says it was cancelled, not: " + result);
            check (file.loadFileAsString() == "what was there before", "the file that was there is left as it was");
            check (folder.getNumberOfChildFiles (juce::File::findFiles) == filesBefore, "and no temporary file is left behind");
        };

        cancelPartWay (folder.getChildFile ("cancelled.wav"), [&] (const juce::File& file, const std::function<bool()>& cancel)
        {
            return AudioExport::writeWav (audio, sampleRate, file, 24, cancel);
        });

        if (AudioExport::canWriteMp3())
            cancelPartWay (folder.getChildFile ("cancelled.mp3"), [&] (const juce::File& file, const std::function<bool()>& cancel)
            {
                return AudioExport::writeMp3 (audio, sampleRate, file, 320, "Cancelled", cancel);
            });
    }

    // The CLAP test synth counts its instances, and how many times one was set to render offline.
    juce::DynamicLibrary hooks;
    using Count = int (*)();
    Count liveInstances = nullptr, offlineRenders = nullptr;

    if (synthFile.hasFileExtension ("clap") && hooks.open (synthFile.getFullPathName()))
    {
        liveInstances = (Count) hooks.getFunction ("mperoll_test_synth_live_instances");
        offlineRenders = (Count) hooks.getFunction ("mperoll_test_synth_offline_renders");
    }

    if (offlineRenders != nullptr)
    {
        std::printf ("The synth copy renders offline\n");
        check (offlineRenders() > 0, "a copy of the CLAP synth was told it renders offline");
    }

    std::printf ("Removing MPE Roll while an export runs\n");
    {
        const auto abandoned = folder.getChildFile ("abandoned.wav");
        abandoned.deleteFile();
        const auto instancesBefore = liveInstances != nullptr ? liveInstances() : 0;
        bool reported = false;

        {
            MpeRollProcessor other;
            other.setRateAndBufferSizeDetails (sampleRate, 512);
            other.prepareToPlay (sampleRate, 512);

            if (loadSynth (other, synthFile))
            {
                // Five minutes of one note in the smallest blocks: far more than any build renders in
                // the moment between seeing the export under way (below) and the plugin going, and
                // the render's buffer for it is about 110 MB.
                MpeClip clip;
                clip.length = 600.0;
                clip.looping = false;
                clip.addNote (note (60, 0.0, 600.0, 0.8f));

                AudioExport::Options options;
                options.sampleRate = sampleRate;
                options.blockSize = 16;
                AudioExport::exportClip (other, clip, abandoned, options, [&] (const juce::String&) { reported = true; });
                auto& jobs = other.getExportJobs();
                check (jobs.getNumRunning() == 1, "the export is one of the plugin's jobs");

                // Wait for the render to be under way, not for a set time: how long the synth copy takes
                // to load varies, and a Release build renders minutes of audio in well under a second,
                // so a half-second wait once saw the export finish and the file written before the
                // plugin went. Progress above 0 means the copy has loaded and is playing the clip;
                // below 0.9, that the file isn't being written yet.
                bool underWay = false;

                for (int i = 0; i < 2000 && ! underWay && jobs.getNumRunning() == 1; ++i)
                {
                    mm.runDispatchLoopUntil (5);
                    const auto progress = jobs.getProgress (0);
                    underWay = progress > 0.0 && progress < 0.9;
                }

                check (underWay, "the export got under way: the synth copy loaded and started playing the clip");
            }

            other.releaseResources();
        }

        mm.runDispatchLoopUntil (200);
        check (! reported && ! abandoned.existsAsFile(), "the export ends with the plugin, reporting and writing nothing");

        if (liveInstances != nullptr)
            check (liveInstances() == instancesBefore, "the synth copy is gone with it");
    }
}

int exportTest (const juce::File& synthFile, const juce::File& folder)
{
    Check check;
    constexpr double sampleRate = 48000.0;
    folder.createDirectory();

    MpeRollProcessor processor;
    processor.setRateAndBufferSizeDetails (sampleRate, 512);
    processor.prepareToPlay (sampleRate, 512);

    auto& mm = *juce::MessageManager::getInstance();
    bool loaded = false;
    juce::String error;
    processor.getSynth().loadFromFile (synthFile, [&] (const juce::String& e) { error = e; loaded = true; });

    for (int i = 0; i < 200 && ! loaded; ++i)
        mm.runDispatchLoopUntil (50);

    if (! loaded || error.isNotEmpty())
    {
        std::printf ("Couldn't load %s: %s\n", synthFile.getFullPathName().toRawUTF8(), error.toRawUTF8());
        return 1;
    }

    // At 120 bpm: A4 for half a second, then C5 for half a second.
    MpeClip clip;
    clip.name = "Export test";
    clip.length = 2.0;
    clip.looping = false;
    clip.addNote (note (69, 0.0, 1.0, 0.8f));
    clip.addNote (note (72, 1.0, 1.0, 0.8f));

    auto exportAs = [&] (AudioExport::Format format, const juce::File& target)
    {
        AudioExport::Options options;
        options.format = format;
        options.sampleRate = sampleRate;
        options.bpm = 120.0;
        options.title = clip.name;

        bool finished = false;
        juce::String result;
        AudioExport::exportClip (processor, clip, target, options, [&] (const juce::String& e) { result = e; finished = true; });

        for (int i = 0; i < 600 && ! finished; ++i)
            mm.runDispatchLoopUntil (50);

        check (finished && result.isEmpty(), "the export finishes without a problem: " + result);
    };

    auto readBack = [] (juce::AudioFormat& format, const juce::File& file, double& rate)
    {
        juce::AudioBuffer<float> audio;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (file.createInputStream().release(), true));

        if (reader != nullptr)
        {
            rate = reader->sampleRate;
            audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&audio, 0, (int) reader->lengthInSamples, 0, true, true);
        }

        return audio;
    };

    const auto a4 = juce::MidiMessage::getMidiNoteInHertz (69), c5 = juce::MidiMessage::getMidiNoteInHertz (72);

    std::printf ("WAV\n");
    {
        const auto wavFile = folder.getChildFile ("export.wav");
        exportAs (AudioExport::Format::wav, wavFile);

        juce::WavAudioFormat wav;
        double rate = 0.0;
        const auto audio = readBack (wav, wavFile, rate);
        const auto seconds = audio.getNumSamples() / juce::jmax (1.0, rate);
        const auto first = sineFrequency (audio, (int) (0.1 * rate), (int) (0.4 * rate), rate);
        const auto second = sineFrequency (audio, (int) (0.6 * rate), (int) (0.9 * rate), rate);
        std::printf ("%.3f s at %.0f Hz, %d channels: %.1f Hz then %.1f Hz\n", seconds, rate, audio.getNumChannels(), first, second);

        check (audio.getNumChannels() == 2 && std::abs (rate - sampleRate) < 1.0, "a stereo WAV at the project's rate");
        check (seconds > 0.99 && seconds < 1.4, "as long as the clip, plus the synth's tail");
        check (std::abs (first - a4) < 1.0 && std::abs (second - c5) < 1.0, "the notes play in time and in tune");
    }

    if (AudioExport::canWriteMp3())
    {
        std::printf ("MP3\n");
        const auto mp3File = folder.getChildFile ("export.mp3");
        exportAs (AudioExport::Format::mp3, mp3File);

       #if JUCE_USE_MP3AUDIOFORMAT
        juce::MP3AudioFormat mp3;
        double rate = 0.0;
        const auto audio = readBack (mp3, mp3File, rate);
        const auto seconds = audio.getNumSamples() / juce::jmax (1.0, rate);
        const auto first = sineFrequency (audio, (int) (0.15 * rate), (int) (0.4 * rate), rate);
        const auto second = sineFrequency (audio, (int) (0.65 * rate), (int) (0.9 * rate), rate);
        std::printf ("%.3f s at %.0f Hz, %lld bytes: %.1f Hz then %.1f Hz\n", seconds, rate,
                     (long long) mp3File.getSize(), first, second);

        check (std::abs (rate - sampleRate) < 1.0 && audio.getNumChannels() == 2, "a stereo MP3 at 48 kHz");
        check (seconds > 0.95 && seconds < 1.5, "as long as the clip, give or take the encoder's padding");
        check (std::abs (first - a4) < 3.0 && std::abs (second - c5) < 3.0, "and it sounds the same");
       #endif
    }

    exportFixChecks (check, synthFile, folder, sampleRate);
    check (processor.getSynth().isLoaded(), "the synth playing live is still there");

    processor.releaseResources();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

int render (const juce::File& synthFile, const juce::File& target)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    MpeRollProcessor processor;
    FakePlayHead playHead;
    processor.setPlayHead (&playHead);

    // The synth is loaded at another rate and block size, then the host changes both, as FL does
    // when its audio settings change. The pitches below are only right if the synth heard that.
    processor.setRateAndBufferSizeDetails (44100.0, 256);
    processor.prepareToPlay (44100.0, 256);

    bool done = false;
    juce::String error;
    processor.getSynth().loadFromFile (synthFile, [&] (const juce::String& e) { error = e; done = true; });

    for (int i = 0; i < 200 && ! done; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    if (! done || error.isNotEmpty())
    {
        std::printf ("Couldn't load %s: %s\n", synthFile.getFullPathName().toRawUTF8(), error.toRawUTF8());
        return 1;
    }

    std::printf ("Hosting: %s\n", processor.getSynth().getName().toRawUTF8());
    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    if (const auto& vst3 = processor.getSynth().getVst3Support(); vst3.isVst3)
        std::printf ("%s\n", vst3.describe().toRawUTF8());

    std::printf ("Played as: %s\n", processor.getSynth().describeConnection (processor.getDocument().getSettings().output).toRawUTF8());

    // C4 glides up an octave between beats 0.5 and 1.5 while E4 holds still. Then A4 has MPE Roll's
    // vibrato: a semitone either way, five times a second.
    MpeClip clip;
    clip.length = 8.0;
    auto glide = note (60, 0.0, 2.0, 0.8f);
    glide.lanes.pitch = { { 0.0, 0.0f }, { 0.5, 0.0f }, { 1.5, 12.0f } };
    clip.addNote (glide);
    clip.addNote (note (64, 0.0, 2.0, 0.8f));
    auto wobble = note (69, 3.0, 2.0, 0.8f);
    wobble.lanes.vibrato = { { 0.0, 1.0f } };
    clip.addNote (wobble);
    clip.vibratoRate = 5.0;
    processor.getDocument().replaceClip (0, clip, "Test");

    const auto totalSamples = (int) (5.5 / 2.0 * sampleRate);
    juce::AudioBuffer<float> output (2, totalSamples);
    output.clear();

    std::map<int, int> noteOnChannels, bendsPerChannel;

    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const auto n = std::min (blockSize, totalSamples - pos);
        juce::AudioBuffer<float> block (2, n);
        juce::MidiBuffer midi;
        playHead.ppq = pos / sampleRate * 2.0;
        processor.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            output.copyFrom (ch, pos, block, ch, 0, n);

        for (const auto m : midi)
        {
            const auto msg = m.getMessage();

            if (msg.isNoteOn())
                noteOnChannels[msg.getNoteNumber()] = msg.getChannel();

            if (msg.isPitchWheel())
                bendsPerChannel[msg.getChannel()]++;
        }
    }

    {
        target.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());

        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                                                     .withNumChannels (2)
                                                                                     .withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (output, 0, totalSamples);
    }

    const auto c4 = juce::MidiMessage::getMidiNoteInHertz (60);
    const auto e4 = juce::MidiMessage::getMidiNoteInHertz (64);
    const auto c5 = juce::MidiMessage::getMidiNoteInHertz (72);
    constexpr int window = 4096;

    auto measure = [&] (double beat, double frequency)
    {
        const auto centre = (int) (beat / 2.0 * sampleRate);
        return goertzel (output.getReadPointer (0, centre - window / 2), window, frequency, sampleRate);
    };

    const auto earlyC4 = measure (0.25, c4), earlyE4 = measure (0.25, e4), earlyC5 = measure (0.25, c5);
    const auto lateC4 = measure (1.85, c4), lateE4 = measure (1.85, e4), lateC5 = measure (1.85, c5);

    std::printf ("Note-on channels: C4 -> %d, E4 -> %d\n", noteOnChannels[60], noteOnChannels[64]);
    std::printf ("Pitch bends: C4's channel %d, E4's channel %d\n",
                 bendsPerChannel[noteOnChannels[60]], bendsPerChannel[noteOnChannels[64]]);
    std::printf ("Beat 0.25:  C4 %.4f  E4 %.4f  C5 %.4f\n", earlyC4, earlyE4, earlyC5);
    std::printf ("Beat 1.85:  C4 %.4f  E4 %.4f  C5 %.4f\n", lateC4, lateE4, lateC5);

    const bool separateChannels = noteOnChannels[60] != noteOnChannels[64];
    const bool startsInTune = earlyC4 > 10.0 * earlyC5 && earlyE4 > 0.01;
    const bool glidedUp = lateC5 > 10.0 * lateC4;
    const bool otherNoteHeld = lateE4 > 0.5 * earlyE4;

    // The sine's frequency from one upward zero crossing to the next, between beats 3.2 and 4.8.
    double highest = 0.0, lowest = 1.0e9;
    int swings = 0;
    {
        const auto a4 = juce::MidiMessage::getMidiNoteInHertz (69);
        const auto* data = output.getReadPointer (0);
        double lastCrossing = -1.0;
        bool high = false;

        for (auto i = (int) (3.2 / 2.0 * sampleRate); i < (int) (4.8 / 2.0 * sampleRate); ++i)
        {
            if (data[i - 1] >= 0.0f || data[i] < 0.0f)
                continue;

            const auto crossing = i - 1 + data[i - 1] / (data[i - 1] - data[i]);

            if (lastCrossing >= 0.0)
            {
                const auto frequency = sampleRate / (crossing - lastCrossing);
                highest = std::max (highest, frequency);
                lowest = std::min (lowest, frequency);

                if (! high && frequency > a4 * 1.02)
                    ++swings;

                high = frequency > a4 * 1.02 || (high && frequency > a4 / 1.02);
            }

            lastCrossing = crossing;
        }
    }

    // A semitone either way is 415 to 466 Hz; 0.8 seconds at 5 Hz is four swings.
    const bool vibrato = highest > 460.0 && highest < 472.0 && lowest > 410.0 && lowest < 421.0 && swings >= 3 && swings <= 5;
    const bool ok = separateChannels && startsInTune && glidedUp && otherNoteHeld && vibrato;

    std::printf ("Vibrato on A4: %.1f to %.1f Hz, %d swings\n", lowest, highest, swings);
    std::printf ("Separate channels: %s, starts in tune: %s, C4 glided to C5: %s, E4 unaffected: %s, vibrato: %s\n",
                 separateChannels ? "yes" : "NO", startsInTune ? "yes" : "NO",
                 glidedUp ? "yes" : "NO", otherNoteHeld ? "yes" : "NO", vibrato ? "yes" : "NO");
    std::printf ("%s (wrote %s)\n", ok ? "PASS" : "FAIL", target.getFullPathName().toRawUTF8());

    processor.releaseResources();
    return ok ? 0 : 1;
}

//==============================================================================
std::vector<juce::MidiMessage> runBlock (MpeRollProcessor& processor, FakePlayHead& playHead, double& ppq,
                                         std::initializer_list<std::pair<int, juce::MidiMessage>> input = {})
{
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    for (const auto& [pos, message] : input)
        midi.addEvent (message, pos);

    playHead.ppq = ppq;
    processor.processBlock (audio, midi);

    if (playHead.playing)
        ppq += blockSize / 48000.0 * playHead.bpm / 60.0;

    std::vector<juce::MidiMessage> out;

    for (const auto m : midi)
        out.push_back (m.getMessage());

    return out;
}

/** The settings every MPE Roll on the computer shares, the scanned plugin list and the Window
    drawing choice among them: one copy in the process, so a change in one MPE Roll is seen by the
    others and kept when any of them saves.
*/
void sharedSettingsChecks (Check& check)
{
    std::printf ("Every MPE Roll in the process shares the computer's settings\n");
    const auto file = juce::File::createTempFile (".settings");

    juce::PluginDescription scanned;
    scanned.name = "Scanned Elsewhere";
    scanned.pluginFormatName = "VST3";
    scanned.fileOrIdentifier = "/plugins/Scanned Elsewhere.vst3";
    scanned.uniqueId = 0x5ca11ed;
    scanned.isInstrument = true;

    // The names of the plugins the settings file lists right now.
    const auto savedNames = [&file]
    {
        juce::PropertiesFile settings (file, {});
        juce::KnownPluginList saved;
        juce::StringArray names;

        if (auto xml = settings.getXmlValue ("knownPlugins"))
            saved.recreateFromXml (*xml);

        for (const auto& type : saved.getTypes())
            names.add (type.name);

        return names;
    };

    {
        HostedSynth first;
        first.useSettingsFile (file);

        {
            HostedSynth second;
            second.getKnownPlugins().addType (scanned);
            const auto types = first.getKnownPlugins().getTypes();
            check (types.size() == 1 && types[0].name == scanned.name, "a plugin scanned in one MPE Roll is listed in the others");

            WindowDrawing::setMode (second.getUserSettings(), WindowDrawing::Mode::software);
            check (WindowDrawing::getMode (first.getUserSettings()) == WindowDrawing::Mode::software, "and so is a Window drawing choice");
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);     // the list's change message, which saves it
        }

        // The MPE Roll left saving its settings after the other has gone mustn't write an older list back.
        first.getUserSettings()->setValue ("checked", true);
        first.getUserSettings()->saveIfNeeded();

        juce::PropertiesFile settings (file, {});
        check (savedNames() == juce::StringArray { scanned.name }, "and is still in the settings file once that MPE Roll has gone");
        check (WindowDrawing::getMode (&settings) == WindowDrawing::Mode::software, "with the Window drawing choice");
    }

    std::printf ("The last MPE Roll to go saves what a scan found after the list's last change message\n");
    auto late = scanned;
    late.name = "Found Last";
    late.fileOrIdentifier = "/plugins/Found Last.vst3";
    late.uniqueId = 0x1a7e;

    {
        // As FL deleting the last MPE Roll (closing the project, say) while a scan is still finding
        // plugins: the list announces each find with a message, and the last ones' never arrive. The
        // message loop isn't run here, as it wouldn't be then. This is the only MPE Roll in the
        // process, so the settings are let go with it.
        HostedSynth last;
        last.useSettingsFile (file);
        last.getKnownPlugins().addType (late);
    }

    check (savedNames() == juce::StringArray { scanned.name, late.name }, "the settings file lists the plugin found last");
    file.deleteFile();
}

int selfTest()
{
    Check check;
    FakePlayHead playHead;
    double ppq = 0.0;

    // Let go before the settings checks: some need the last MPE Roll in the process to go.
    auto processorHolder = std::make_unique<MpeRollProcessor>();
    auto& processor = *processorHolder;
    processor.setPlayHead (&playHead);
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);
    auto& document = processor.getDocument();

    std::printf ("Same-named plugins are told apart in the synth menu\n");
    {
        const auto plugin = [] (const juce::String& name, const juce::String& format, const juce::File& file)
        {
            juce::PluginDescription d;
            d.name = name;
            d.pluginFormatName = format;
            d.fileOrIdentifier = file.getFullPathName();
            return d;
        };

        // Two installs of Analog Lab, as a user had: the same name, and the same format twice.
        const auto base = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto arturia = base.getChildFile ("Arturia"), x64 = arturia.getChildFile ("Analog Lab V").getChildFile ("x64");
        juce::Array<juce::PluginDescription> types;
        types.add (plugin ("Pigments", "VST3", base.getChildFile ("Pigments.vst3")));
        types.add (plugin ("Analog Lab V", "VST3", x64.getChildFile ("Analog Lab V.vst3")));
        types.add (plugin ("Analog Lab V", "VST", arturia.getChildFile ("Analog Lab V.dll")));
        types.add (plugin ("Analog Lab V", "VST", x64.getChildFile ("Analog Lab V.dll")));

        const auto shown = HostedSynth::withDistinctNames (types);
        check (shown.size() == 4 && shown[0].name == "Pigments", "a name of its own stays as it is");
        check (shown[1].name == "Analog Lab V (VST3)", "one sharing its name gets its format");
        check (shown[2].name == "Analog Lab V (VST, " + arturia.getFullPathName() + ")"
                   && shown[3].name == "Analog Lab V (VST, " + x64.getFullPathName() + ")",
               "two installs of one plugin get their folders");
        check (shown[2].fileOrIdentifier == types[2].fileOrIdentifier && shown[3].fileOrIdentifier == types[3].fileOrIdentifier,
               "in the same order, so a pick finds its plugin");
    }

    std::printf ("Live input passes through as MPE\n");
    {
        playHead.playing = false;
        runBlock (processor, playHead, ppq);   // first block sends the MPE configuration
        const auto out = runBlock (processor, playHead, ppq,
                                   { { 0, juce::MidiMessage::pitchWheel (2, 8192 + 2048) },
                                     { 10, juce::MidiMessage::noteOn (2, 60, (juce::uint8) 100) } });
        int noteChannel = 0, bendBefore = -1;

        for (const auto& m : out)
        {
            if (m.isPitchWheel() && noteChannel == 0)
                bendBefore = m.getPitchWheelValue();

            if (m.isNoteOn() && m.getNoteNumber() == 60)
                noteChannel = m.getChannel();
        }

        check (noteChannel >= 2, "note arrives on a member channel");
        check (bendBefore == 8192 + 2048, "its pitch bend is sent before the Note On");
        runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::noteOff (2, 60, (juce::uint8) 64) } });
    }

    std::printf ("Recording a take from an MPE controller\n");
    {
        playHead.playing = true;
        ppq = 0.0;
        processor.setRecordArmed (true);

        runBlock (processor, playHead, ppq);
        runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::noteOn (3, 64, (juce::uint8) 90) } });

        for (int i = 1; i <= 20; ++i)
            runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::pitchWheel (3, MpeEncoder::semitonesToPitchBend ((float) i * 0.1f, 48.0f)) },
                                                  { 0, juce::MidiMessage::channelPressureChange (3, i * 5) } });

        runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::noteOff (3, 64, (juce::uint8) 40) } });
        playHead.playing = false;
        runBlock (processor, playHead, ppq);
        processor.setRecordArmed (false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        const auto& clip = document.getClip (0);
        const MpeNote* recorded = nullptr;

        for (const auto& n : clip.notes)
            if (n.key == 64)
                recorded = &n;

        check (recorded != nullptr, "the recorded note is in the clip");

        if (recorded != nullptr)
        {
            const auto blockBeats = 512.0 / 48000.0 * 2.0;
            check (std::abs (recorded->start - blockBeats) < 1.0e-6, "it starts where it was played");
            check (std::abs (recorded->lanes.pitch.valueAt (recorded->length, 0.0f) - 2.0f) < 0.05f, "its glide was captured");
            check (! recorded->lanes.pressure.isEmpty(), "its pressure was captured");
            check (std::abs (recorded->lift - 40.0f / 127.0f) < 1.0e-4f, "its release velocity was captured");
        }

        check (document.getUndoManager().canUndo(), "the take can be undone");
    }

    std::printf ("Saving and restoring the plugin state\n");
    {
        auto settings = document.getSettings();
        settings.output.memberBendRange = 24.0f;
        settings.output.highResVelocity = true;
        document.setSettings (settings);

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        MpeRollProcessor restored;
        restored.setStateInformation (state.getData(), (int) state.getSize());
        check (restored.getDocument().getClip (0) == document.getClip (0), "clips are restored");
        check (restored.getDocument().getSettings() == document.getSettings(), "settings are restored");
    }

    std::printf ("A synth that isn't installed is remembered\n");
    {
        juce::XmlElement state ("MpeRoll");
        auto* synthXml = state.createNewChildElement ("HostedSynth");
        juce::PluginDescription missing;
        missing.name = "Not Installed Synth";
        missing.pluginFormatName = "VST3";
        missing.fileOrIdentifier = "/nowhere/Not Installed Synth.vst3";
        synthXml->addChildElement (missing.createXml().release());
        synthXml->createNewChildElement ("State")->addTextElement ("AAAA");

        juce::MemoryBlock data;
        juce::AudioProcessor::copyXmlToBinary (state, data);

        MpeRollProcessor other;
        other.setStateInformation (data.getData(), (int) data.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        juce::MemoryBlock saved;
        other.getStateInformation (saved);
        const auto savedXml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        const auto* kept = savedXml != nullptr ? savedXml->getChildByName ("HostedSynth") : nullptr;
        check (kept != nullptr && kept->getChildByName ("PLUGIN") != nullptr
                   && kept->getChildByName ("PLUGIN")->getStringAttribute ("name") == missing.name,
               "saving keeps the missing synth in the project");
        check (other.getSynth().getMissingSynthName() == missing.name, "the UI can say which synth is missing");
    }

    std::printf ("Note-trigger mode launches clip slots\n");
    {
        auto settings = document.getSettings();
        settings.playback.mode = PlaybackSettings::Mode::noteTrigger;
        settings.playback.triggerBaseKey = 24;
        document.setSettings (settings);

        MpeClip clip;
        clip.addNote ([] { MpeNote n; n.key = 77; n.start = 0.0; n.length = 1.0; return n; }());
        document.replaceClip (3, clip, "Slot 4");

        playHead.playing = false;
        const auto out = runBlock (processor, playHead, ppq, { { 100, juce::MidiMessage::noteOn (1, 27, (juce::uint8) 100) } });
        bool clipNote = false, triggerLeaked = false;

        for (const auto& m : out)
        {
            clipNote |= m.isNoteOn() && m.getNoteNumber() == 77;
            triggerLeaked |= m.isNoteOn() && m.getNoteNumber() == 27;
        }

        check (clipNote, "key 27 starts slot 4's note");
        check (! triggerLeaked, "the trigger key itself isn't played");
    }

    std::printf ("Undo\n");
    {
        const auto before = document.getClip (3);
        document.setEditSlot (3);
        document.perform ("Clear", [] (MpeClip& c) { c.notes.clear(); });
        check (document.getClip (3).notes.empty(), "edit applied");
        document.getUndoManager().undo();
        check (document.getClip (3) == before, "undo restores the clip");
    }

    processor.releaseResources();
    processorHolder.reset();
    sharedSettingsChecks (check);

    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

//==============================================================================
bool loadSynth (MpeRollProcessor& processor, const juce::File& file)
{
    bool done = false;
    juce::String error;
    processor.getSynth().loadFromFile (file, [&] (const juce::String& e) { error = e; done = true; });

    for (int i = 0; i < 200 && ! done; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

    if (! done || error.isNotEmpty())
        std::printf ("Couldn't load %s: %s\n", file.getFullPathName().toRawUTF8(), error.toRawUTF8());

    return done && error.isEmpty();
}

struct ValueRecorder : juce::AudioProcessorParameter::Listener
{
    float last = -1.0f;
    int gestures = 0, offMessageThread = 0;

    void parameterValueChanged (int, float v) override  { last = v; note(); }
    void parameterGestureChanged (int, bool) override    { ++gestures; note(); }

    void note()
    {
        if (! juce::MessageManager::existsAndIsCurrentThread())
            ++offMessageThread;
    }
};

/** Checks only the CLAP test synth can help with: its exported hooks act as its user. */
void clapSynthChecks (Check& check, MpeRollProcessor& processor, FakePlayHead& playHead, const juce::File& synthFile)
{
    juce::DynamicLibrary hooks;

    if (! hooks.open (synthFile.getFullPathName()))
    {
        std::printf ("  (the CLAP test synth's hooks aren't reachable here: skipped)\n");
        return;
    }

    using TurnKnob = bool (*) (double);
    using Rebuild = bool (*)();
    using StaleCookies = int (*)();
    auto turnKnob = (TurnKnob) hooks.getFunction ("mperoll_test_synth_turn_tone_knob");
    auto rebuild = (Rebuild) hooks.getFunction ("mperoll_test_synth_rebuild_parameters");
    auto staleCookies = (StaleCookies) hooks.getFunction ("mperoll_test_synth_stale_cookies");
    auto currentCookies = (StaleCookies) hooks.getFunction ("mperoll_test_synth_current_cookies");

    if (turnKnob == nullptr || rebuild == nullptr || staleCookies == nullptr || currentCookies == nullptr)
    {
        std::printf ("  (the CLAP test synth has no hooks: skipped)\n");
        return;
    }

    auto& params = processor.getParameters();

    // One block on another thread, as a host's audio thread would run it.
    auto processOnAudioThread = [&]
    {
        std::thread audioThread ([&]
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi);
        });

        audioThread.join();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    };

    std::printf ("Edits in a CLAP synth's own window reach the host as edits\n");
    {
        ValueRecorder recorder;
        params[1]->addListener (&recorder);
        turnKnob (0.25);
        processOnAudioThread();
        params[1]->removeListener (&recorder);

        check (std::abs (recorder.last - 0.25f) < 1.0e-4f, "the new value reaches the slot");
        check (recorder.gestures == 2, "with its gesture");
        check (recorder.offMessageThread == 0, "on the message thread, where hosts record edits");
    }

    std::printf ("A CLAP synth that rebuilds its parameters gets current cookies\n");
    {
        playHead.playing = false;
        processor.releaseResources();
        check (rebuild(), "the synth rebuilt its parameters while inactive");
        processor.prepareToPlay (48000.0, 512);

        const auto currentBefore = currentCookies();
        params[1]->setValue (0.6f);
        processOnAudioThread();
        check (currentCookies() > currentBefore, "the value reached the synth with its new cookie");
        check (staleCookies() == 0, "no value was sent with an out-of-date cookie");
    }
}

/** A processor with the synth loaded afresh, for playing it notes and asking whether MPE Roll
    warned that it made no sound. Muting the synth stands in for one that isn't listening on MPE
    Roll's channels.
*/
struct SilenceProbe
{
    SilenceProbe (const juce::File& synthFile, OutputSettings::Mode mode, bool muted)
    {
        playHead.playing = false;
        processor.setPlayHead (&playHead);
        processor.setRateAndBufferSizeDetails (48000.0, 512);
        processor.prepareToPlay (48000.0, 512);
        loaded = loadSynth (processor, synthFile);

        auto settings = processor.getDocument().getSettings();
        settings.output.mode = mode;
        processor.getDocument().setSettings (settings);
        mute (muted);
    }

    ~SilenceProbe()
    {
        processor.releaseResources();
    }

    void mute (bool muted)
    {
        for (int i = 0; processor.getSynth().withParameter (i, [muted] (juce::AudioProcessorParameter& p)
                                                            { if (p.getName (64) == "Gain") p.setValue (muted ? 0.0f : 1.0f); }); ++i) {}
    }

    /** Plays a note for this long, then lets it go. */
    void hold (int key, double seconds)
    {
        runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::noteOn (2, key, (juce::uint8) 100) } });
        rest (seconds - 512.0 / 48000.0);
        runBlock (processor, playHead, ppq, { { 0, juce::MidiMessage::noteOff (2, key) } });
    }

    /** Plays nothing for this long. */
    void rest (double seconds)
    {
        for (int block = 0; block < juce::roundToInt (seconds * 48000.0 / 512.0); ++block)
            runBlock (processor, playHead, ppq);
    }

    bool warned()
    {
        return processor.getSynth().takeSilenceWarning();
    }

    FakePlayHead playHead;
    MpeRollProcessor processor;
    double ppq = 0.0;
    bool loaded = false;
};

/** Loads the synth afresh, plays it a note held for fourteen seconds (past the ten a new synth is
    given to load) and says whether MPE Roll warned that the synth made no sound. Empty if the synth
    didn't load.
*/
std::optional<bool> warnsOfSilence (const juce::File& synthFile, OutputSettings::Mode mode, bool muted)
{
    SilenceProbe probe (synthFile, mode, muted);

    if (! probe.loaded)
        return {};

    probe.hold (60, 14.0);
    return probe.warned();
}

/** The "No sound from ..." warning: given once for a synth that stays silent over MPE, with short
    notes adding up as one long one would, but not while it may still be loading and not once it
    has sounded.
*/
void silenceWatchChecks (Check& check, const juce::File& synthFile)
{
    std::printf ("A synth that makes no sound over MPE is pointed out\n");

    // A CLAP synth played with note expressions gets its notes whatever the channels.
    const auto midiOnly = ! synthFile.hasFileExtension ("clap");
    check (warnsOfSilence (synthFile, OutputSettings::Mode::mpe, true) == midiOnly,
           midiOnly ? "a silent synth played over MPE is pointed out" : "not with note expressions");
    check (warnsOfSilence (synthFile, OutputSettings::Mode::mpe, false) == false, "a synth that sounds isn't");
    check (warnsOfSilence (synthFile, OutputSettings::Mode::singleChannel, true) == false,
           "nor a silent one played with Plain MIDI");

    if (! midiOnly)
        return;

    std::printf ("Not while it may still be loading, and not once it has sounded\n");
    {
        SilenceProbe probe (synthFile, OutputSettings::Mode::mpe, true);
        probe.hold (60, 8.0);
        check (probe.loaded && ! probe.warned(), "a synth silent for its first seconds is given time to load");

        probe.rest (2.5);               // the grace is over
        probe.hold (60, 2.0);           // four seconds of silent notes, none held for three
        probe.hold (62, 2.0);
        check (probe.warned(), "short notes add up to the warning: the synth heard none of them");
        check (probe.processor.getSynth().isSilentOverMpe(), "and the synth is marked silent for the status line");

        probe.hold (60, 3.5);
        check (! probe.warned(), "the warning comes once");
    }

    {
        SilenceProbe probe (synthFile, OutputSettings::Mode::mpe, true);
        probe.hold (60, 14.0);          // warned, but the warning hasn't been shown
        probe.mute (false);
        probe.hold (60, 0.5);
        check (probe.loaded && ! probe.warned(), "a warning not shown yet is taken back once the synth sounds");
        check (! probe.processor.getSynth().isSilentOverMpe(), "and the mark for the status line goes with it");
    }

    {
        SilenceProbe probe (synthFile, OutputSettings::Mode::mpe, true);
        probe.hold (60, 14.0);          // warned, but the warning hasn't been shown

        juce::MemoryBlock state;
        probe.processor.getStateInformation (state);
        probe.processor.setStateInformation (state.getData(), (int) state.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        probe.hold (60, 5.0);
        check (probe.loaded && ! probe.warned(), "a synth given a project's state gets the time to load again");

        probe.rest (9.0);               // the grace is over, but nothing is held
        check (! probe.warned(), "and silence with no note held doesn't count against it");

        probe.hold (60, 3.5);
        check (probe.warned(), "a note held for three seconds afterwards is pointed out as before");
    }
}

/** The "No sound from ..." box the editor shows for the warning. Only its Use Plain MIDI and One
    copy per note buttons may change the project: Return and Escape both keep MPE, as closing the
    box does.
*/
void silenceAlertChecks (Check& check, const juce::File& synthFile)
{
    if (synthFile.hasFileExtension ("clap"))     // played with note expressions, so never warned about
        return;

    std::printf ("Only the box's Use Plain MIDI and One copy per note buttons switch the project away from MPE\n");
    ignoreXErrors();
    auto& mm = *juce::MessageManager::getInstance();
    SilenceProbe probe (synthFile, OutputSettings::Mode::mpe, true);
    check (probe.loaded, "the synth loads");

    if (! probe.loaded)
        return;

    std::unique_ptr<juce::AudioProcessorEditor> editor (probe.processor.createEditorAndMakeActive());
    editor->setSize (1200, 760);

    const auto findBox = []() -> juce::AlertWindow*
    {
        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* box = dynamic_cast<juce::AlertWindow*> (desktop.getComponent (i)))
                if (box->getName().startsWith ("No sound from"))
                    return box;

        return nullptr;
    };

    const auto boxCount = []
    {
        int count = 0;
        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* box = dynamic_cast<juce::AlertWindow*> (desktop.getComponent (i)))
                count += box->getName().startsWith ("No sound from") ? 1 : 0;

        return count;
    };

    // Plays the synth into the warning, which the editor's timer turns into the box.
    const auto provoke = [&]() -> juce::AlertWindow*
    {
        probe.hold (60, 14.0);

        for (int i = 0; i < 20 && findBox() == nullptr; ++i)
            mm.runDispatchLoopUntil (50);

        return findBox();
    };

    // The warning comes once per synth loaded or given a project's state: given its own state back,
    // the synth is listened to afresh.
    const auto listenAgain = [&]
    {
        juce::MemoryBlock state;
        probe.processor.getStateInformation (state);
        probe.processor.setStateInformation (state.getData(), (int) state.getSize());
        mm.runDispatchLoopUntil (50);
    };

    const auto mode = [&] { return probe.processor.getDocument().getSettings().output.mode; };

    // As the window's peer hands a key on (the key handler is the component's).
    const auto press = [&mm] (juce::Component& window, int key)
    {
        window.keyPressed (juce::KeyPress (key));
        mm.runDispatchLoopUntil (100);
    };

    // Whether a label somewhere in the editor (the toolbar's status line) starts with this.
    const std::function<bool (juce::Component&, const juce::String&)> labelSays = [&labelSays] (juce::Component& c, const juce::String& start)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&c))
            if (label->getText().startsWith (start))
                return true;

        for (auto* child : c.getChildren())
            if (labelSays (*child, start))
                return true;

        return false;
    };

    const auto statusSays = [&] (const juce::String& start)
    {
        mm.runDispatchLoopUntil (100);      // the editor's timer refreshes the status line
        return labelSays (*editor, start);
    };

    check (! statusSays ("No sound from"), "the status line says nothing of it before the warning");

    auto* box = provoke();
    check (box != nullptr, "the warning opens the box");

    if (box != nullptr)
    {
        check (box->isAlwaysOnTop(), "which stays above FL's window, as the synth's own window does");
        check (statusSays ("No sound from"), "and the status line says so too");
        press (*box, juce::KeyPress::escapeKey);
        check (findBox() == nullptr && mode() == OutputSettings::Mode::mpe, "Escape closes it and keeps MPE");
        check (statusSays ("No sound from"), "and the status line keeps saying so while nothing is heard");
    }

    listenAgain();
    box = provoke();
    check (box != nullptr, "the box comes back for a synth given its state again");

    if (box != nullptr)
    {
        press (*box, juce::KeyPress::returnKey);
        check (findBox() == nullptr && mode() == OutputSettings::Mode::mpe, "Return closes it and keeps MPE too");
    }

    // A synth given its state again while the box is open (as FL sending a project's state again)
    // is listened to afresh and can warn again: the editor keeps the open box rather than putting
    // a second one over it, which closing the editor wouldn't take down.
    listenAgain();
    box = provoke();

    if (box != nullptr)
    {
        listenAgain();
        provoke();                          // returns at once, a box being up...
        mm.runDispatchLoopUntil (300);      // ...so give the editor's timer the new warning
        check (boxCount() == 1, "warned again while the box is open, the editor keeps that box rather than stacking another");
        press (*box, juce::KeyPress::escapeKey);
        check (findBox() == nullptr && mode() == OutputSettings::Mode::mpe, "and Escape closes it as before");
    }

    listenAgain();
    box = provoke();

    if (box != nullptr)
    {
        if (auto* button = box->getButton ("Use Plain MIDI"))
            button->triggerClick();

        mm.runDispatchLoopUntil (100);
        check (findBox() == nullptr && mode() == OutputSettings::Mode::singleChannel, "Use Plain MIDI switches the project to Plain MIDI");
    }

    // Back to MPE, unheard again, for the box's other way out. Two copies are quick to load.
    {
        auto settings = probe.processor.getDocument().getSettings();
        settings.output.mode = OutputSettings::Mode::mpe;
        settings.output.copies = OutputSettings::minCopies;
        probe.processor.getDocument().setSettings (settings);
    }

    listenAgain();
    box = provoke();

    if (box != nullptr)
    {
        if (auto* button = box->getButton ("One copy per note"))
            button->triggerClick();

        mm.runDispatchLoopUntil (100);
        check (findBox() == nullptr && mode() == OutputSettings::Mode::copies, "One copy per note switches the project to copies");

        // Past the message the switch puts up, the status line counts copies, with no word of the
        // MPE advice that no longer applies.
        mm.runDispatchLoopUntil (6200);
        check (! statusSays ("No sound from") && statusSays ("One copy per note"),
               "and the status line then speaks of the copies, not of turning MPE on");
    }

    probe.processor.editorBeingDeleted (editor.get());
    editor.reset();
    mm.runDispatchLoopUntil (50);
}

/** A synth loaded while a clip plays (in place of another, or where there was none) must get the
    zone lanes' values, which the encoder otherwise sends once and then only as they change.
*/
void synthReloadChecks (Check& check, MpeRollProcessor& processor, FakePlayHead& playHead, const juce::File& synthFile)
{
    std::printf ("A synth loaded while a clip plays gets the zone lanes' values\n");

    // One note under a mod wheel lane that never moves.
    MpeClip clip;
    clip.length = 4.0;
    clip.addNote (note (60, 0.0, 4.0, 0.8f));
    clip.zone.get (LaneId::cc (1)) = { { 0.0, 0.5f } };
    processor.getDocument().replaceClip (0, clip, "Zone lane");

    auto modWheelIn = [] (const std::vector<juce::MidiMessage>& out)
    {
        for (const auto& m : out)
            if (m.isController() && m.getControllerNumber() == 1 && m.getControllerValue() == MpeEncoder::toSevenBit (0.5f))
                return true;

        return false;
    };

    double ppq = 0.0;

    auto playBlocks = [&] (int count)
    {
        bool seen = false;

        for (int block = 0; block < count; ++block)
            seen |= modWheelIn (runBlock (processor, playHead, ppq));

        return seen;
    };

    playHead.playing = true;
    check (playBlocks (4), "the lane's value is sent as the clip starts");
    check (! playBlocks (4), "and not again while it stays the same");

    if (loadSynth (processor, synthFile))
        check (playBlocks (4), "a synth loaded in its place gets the value too");
    else
        check (false, "the synth loads again");

    playHead.playing = false;
    runBlock (processor, playHead, ppq);
}

int parameterTest (const juce::File& synthFile)
{
    Check check;
    FakePlayHead playHead;
    MpeRollProcessor processor;
    processor.setPlayHead (&playHead);
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    if (! loadSynth (processor, synthFile))
        return 1;

    const auto& params = processor.getParameters();
    auto* hosted = processor.getSynth().getInstance();

    std::printf ("The synth's window opens on screen\n");
    {
        auto& mm = *juce::MessageManager::getInstance();
        ignoreXErrors();
        auto& synth = processor.getSynth();
        synth.showEditor (nullptr);
        mm.runDispatchLoopUntil (200);

        juce::DocumentWindow* window = nullptr;

        for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
            if (auto* w = dynamic_cast<juce::DocumentWindow*> (juce::Desktop::getInstance().getComponent (i)))
                if (w->getName() == hosted->getName())
                    window = w;

        auto titleBarOnScreen = [&]
        {
            const auto work = WindowPlacement::workAreaNear (window);
            const auto outer = WindowPlacement::frameOf (*window).addedTo (window->getBounds());
            return work.contains (outer.getTopLeft()) && outer.getY() >= work.getY();
        };

        check (window != nullptr && titleBarOnScreen(), "with its title bar inside the display's work area");

        if (window != nullptr)
        {
            // As a window dragged or grown off the top: the synth menu's item brings it back.
            window->setTopLeftPosition (window->getX(), WindowPlacement::workAreaNear (window).getY() - 500);
            synth.bringEditorOnScreen (nullptr);
            mm.runDispatchLoopUntil (100);
            check (titleBarOnScreen(), "and Bring the synth's window back on screen brings back one that went off the top");
        }

        synth.closeEditor();
        mm.runDispatchLoopUntil (50);
    }

    std::printf ("Synth parameters appear as MPE Roll parameters\n");
    check (params.size() == SynthParameterBank::numSlots, "MPE Roll offers a fixed set of automatable slots");
    check (params[0]->getName (64) == "Gain" && params[1]->getName (64) == "Tone", "slots take the synth's parameter names");
    check (params[2]->getName (64).contains ("unused"), "slots beyond the synth's parameters say they're unused");

    std::printf ("Automation reaches the synth, and synth edits reach the host\n");
    params[1]->setValue (0.3f);
    check (std::abs (hosted->getParameters()[1]->getValue() - 0.3f) < 1.0e-4f, "setting a slot sets the synth parameter");

    ValueRecorder recorder;
    params[1]->addListener (&recorder);
    hosted->getParameters()[1]->beginChangeGesture();
    hosted->getParameters()[1]->setValueNotifyingHost (0.75f);
    hosted->getParameters()[1]->endChangeGesture();
    params[1]->removeListener (&recorder);
    check (std::abs (recorder.last - 0.75f) < 1.0e-4f, "a change in the synth is reported to the host");
    check (recorder.gestures == 2, "and so are its gestures (for FL's 'last tweaked')");

    // Gain 0 through the slot must silence the synth.
    params[0]->setValue (0.0f);
    MpeClip clip;
    clip.addNote (note (60, 0.0, 1.0, 1.0f));
    processor.getDocument().replaceClip (0, clip, "Test");

    double ppq = 0.0;
    float peak = 0.0f;

    for (int block = 0; block < 40; ++block)
    {
        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer midi;
        playHead.ppq = ppq;
        processor.processBlock (audio, midi);
        ppq += 512.0 / 48000.0 * 2.0;
        peak = std::max (peak, audio.getMagnitude (0, 512));
    }

    check (peak < 1.0e-4f, "automating the synth's gain to 0 through MPE Roll silences it");

    std::printf ("Slots survive saving and reloading\n");
    {
        juce::MemoryBlock state;
        processor.getStateInformation (state);

        MpeRollProcessor restored;
        restored.setRateAndBufferSizeDetails (48000.0, 512);
        restored.prepareToPlay (48000.0, 512);
        restored.setStateInformation (state.getData(), (int) state.getSize());

        for (int i = 0; i < 100 && ! restored.getSynth().isLoaded(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        check (restored.getParameters()[1]->getName (64) == "Tone", "the restored slots point at the same parameters");
        check (std::abs (restored.getParameters()[1]->getValue() - 0.75f) < 1.0e-3f, "and the synth's values come back");
        restored.releaseResources();
    }

    std::printf ("A slot whose parameter has gone stays empty\n");
    {
        auto saveXml = [&]
        {
            juce::MemoryBlock state;
            processor.getStateInformation (state);
            return juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        };

        // The same synth is loaded, so this restores into it in place.
        auto restoreXml = [&] (const juce::XmlElement& xml)
        {
            juce::MemoryBlock state;
            juce::AudioProcessor::copyXmlToBinary (xml, state);
            processor.setStateInformation (state.getData(), (int) state.getSize());
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        };

        auto firstSlotOf = [] (juce::XmlElement* xml)
        {
            auto* slots = xml != nullptr ? xml->getChildByName ("SynthParameters") : nullptr;
            return slots != nullptr ? slots->getChildByName ("Slot") : nullptr;
        };

        auto xml = saveXml();
        auto* first = firstSlotOf (xml.get());
        check (first != nullptr && first->getIntAttribute ("index", -1) == 0, "the project lists the slots");

        if (first != nullptr)
        {
            const juce::String goneId ("a-parameter-this-synth-no-longer-has");
            first->setAttribute ("id", goneId);
            restoreXml (*xml);

            check (params[0]->getName (64).contains ("unused"), "its slot isn't handed to another parameter");
            check (params[1]->getName (64) == "Tone", "the other slots keep their parameters");
            check (params[2]->getName (64) == "Gain", "a parameter without a slot gets a free one");

            auto again = saveXml();
            auto* kept = firstSlotOf (again.get());
            check (kept != nullptr && kept->getStringAttribute ("id") == goneId, "saving keeps the slot for its parameter");

            xml->getChildByName ("SynthParameters")->setAttribute ("synth", "another synth");
            restoreXml (*xml);
            check (params[0]->getName (64) == "Gain" && params[1]->getName (64) == "Tone"
                       && params[2]->getName (64).contains ("unused"),
                   "slots saved for a different synth start afresh");
        }
    }

    if (synthFile.hasFileExtension ("clap"))
        clapSynthChecks (check, processor, playHead, synthFile);

    std::printf ("A block longer than the host announced still plays\n");
    {
        params[0]->setValue (1.0f);
        playHead.playing = true;
        playHead.ppq = 0.0;

        juce::AudioBuffer<float> audio (2, 2048);     // prepared for 512
        juce::MidiBuffer midi;
        processor.processBlock (audio, midi);
        check (audio.getMagnitude (0, 0, 512) > 1.0e-3f && audio.getMagnitude (0, 1536, 512) > 1.0e-3f,
               "the synth sounds all the way through it");

        // The CLAP test synth reports where the transport said its last piece started.
        juce::DynamicLibrary hooks;

        if (synthFile.hasFileExtension ("clap") && hooks.open (synthFile.getFullPathName()))
        {
            using LastBeats = double (*)();

            if (auto lastBeats = (LastBeats) hooks.getFunction ("mperoll_test_synth_last_song_beats"))
            {
                const auto expected = 1536.0 / 48000.0 * playHead.bpm / 60.0;
                check (std::abs (lastBeats() - expected) < 1.0e-4, "each piece hears where in the song it starts");
            }
        }
    }

    silenceWatchChecks (check, synthFile);
    silenceAlertChecks (check, synthFile);

    synthReloadChecks (check, processor, playHead, synthFile);

    processor.releaseResources();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

//==============================================================================
/** Plays one held note through the CLAP test synth with CLAP note expressions on, then off.
    The test synth ignores MIDI, so it should only sound when MPE Roll sends it CLAP notes.
*/
int clapDialectTest (const juce::File& synthFile)
{
    Check check;
    FakePlayHead playHead;
    MpeRollProcessor processor;
    processor.setPlayHead (&playHead);
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    if (! loadSynth (processor, synthFile))
        return 1;

    MpeClip clip;
    clip.length = 4.0;
    clip.addNote (note (60, 0.0, 4.0, 0.8f));
    processor.getDocument().replaceClip (0, clip, "Test");

    auto peakWith = [&] (bool noteExpressions)
    {
        auto s = processor.getDocument().getSettings();
        s.output.clapNoteExpressions = noteExpressions;
        processor.getDocument().setSettings (s);

        std::printf ("Played with: %s\n", processor.getSynth().describeConnection (s.output).toRawUTF8());

        // Stop, so the note starts again under the new setting.
        playHead.playing = false;
        double ppq = 0.0;
        runBlock (processor, playHead, ppq);
        playHead.playing = true;

        float peak = 0.0f;

        for (int block = 0; block < 40; ++block)
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            playHead.ppq = ppq;
            processor.processBlock (audio, midi);
            ppq += 512.0 / 48000.0 * 2.0;

            if (block >= 4)
                peak = std::max (peak, audio.getMagnitude (0, 512));
        }

        return peak;
    };

    std::printf ("A CLAP synth is played with CLAP notes and note expressions\n");
    const auto withExpressions = peakWith (true);
    check (withExpressions > 0.01f, "with note expressions on, the synth plays");

    const auto withMidi = peakWith (false);
    check (withMidi < 1.0e-4f, "with them off it gets MPE MIDI instead (which this synth ignores)");

    const auto again = peakWith (true);
    check (again > 0.01f, "and switching back works without reloading the synth");

    processor.releaseResources();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

/** The VST3 note expression path, with a synth that maps pitch bend on MIDI channel 1 only and takes
    tuning and pressure as note expressions (tools/Vst3NoteExpressionTestSynth.cpp), and a JUCE-built
    one that maps every channel. The glides themselves are checked by render.
*/
int vst3DialectTest (const juce::File& noteExpressionSynth, const juce::File& midiSynth)
{
    Check check;
    FakePlayHead playHead;
    MpeRollProcessor processor;
    processor.setPlayHead (&playHead);
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    if (! loadSynth (processor, noteExpressionSynth))
        return 1;

    auto& synth = processor.getSynth();
    const auto& support = synth.getVst3Support();
    std::printf ("%s\n", support.describe().toRawUTF8());

    std::printf ("MPE Roll asks a VST3 synth what it takes\n");
    check (support.isVst3 && support.hasMidiMapping && support.channelsWithBend == 1 && ! support.ownBendOnMemberChannels,
           "it sees pitch bend mapped on one MIDI channel only, so MPE over MIDI would miss the notes");
    check (support.takesTuning() && support.typeIds[(size_t) Vst3Support::pressure] == 100000 && support.scaled[(size_t) Vst3Support::pressure],
           "and that the synth takes tuning, and pressure as its own type through its physical UI mapping");
    check (synth.playsNoteExpressions (processor.getDocument().getSettings().output)
               && synth.describeConnection (processor.getDocument().getSettings().output).contains ("VST3 notes"),
           "so it plays it with VST3 note expressions, and says so");

    auto setNoteExpressions = [&] (bool on)
    {
        auto s = processor.getDocument().getSettings();
        s.output.clapNoteExpressions = on;
        processor.getDocument().setSettings (s);
    };

    // A note held for the whole clip, at a steady pressure.
    auto play = [&] (float pressure, int blocks, bool fromStart = true)
    {
        if (fromStart)
        {
            MpeClip clip;
            clip.length = 8.0;
            auto held = note (60, 0.0, 8.0, 0.8f);
            held.lanes.pressure = { { 0.0, pressure } };
            clip.addNote (held);
            processor.getDocument().replaceClip (0, clip, "Test");

            playHead.playing = false;
            double ppq = 0.0;
            runBlock (processor, playHead, ppq);
            playHead.playing = true;
            playHead.ppq = 0.0;
        }

        float peak = 0.0f;

        for (int block = 0; block < blocks; ++block)
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi);
            playHead.ppq += 512.0 / 48000.0 * 2.0;

            if (block >= blocks / 2)
                peak = std::max (peak, audio.getMagnitude (0, 512));
        }

        return peak;
    };

    auto stopAndListen = [&]
    {
        playHead.playing = false;
        float peak = 0.0f;

        for (int block = 0; block < 20; ++block)
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi);

            if (block >= 10)
                peak = std::max (peak, audio.getMagnitude (0, 512));
        }

        playHead.playing = true;
        return peak;
    };

    std::printf ("Each note's pressure reaches it as the synth's own pressure type\n");
    const auto soft = play (0.0f, 20), hard = play (1.0f, 20);
    std::printf ("  peak %.4f at no pressure, %.4f at full\n", soft, hard);
    check (soft > 0.01f && hard > 3.0f * soft, "a note pressed fully is about four times louder, as the synth scales it");
    check (stopAndListen() < 1.0e-4f, "and it ends when playback stops");

    std::printf ("Switching note expressions off and on leaves no note behind\n");
    setNoteExpressions (false);
    check (! synth.playsNoteExpressions (processor.getDocument().getSettings().output)
               && synth.describeConnection (processor.getDocument().getSettings().output).contains ("can't reach it one by one"),
           "off, it's played with MPE over MIDI, and MPE Roll says the notes can't be reached one by one");
    check (play (1.0f, 20) > 0.01f, "where it still plays (all at the same pitch bend)");
    setNoteExpressions (true);
    play (1.0f, 20, false);     // the note that went as MIDI is held while note expressions come back on
    check (stopAndListen() < 1.0e-4f, "a note held while note expressions came on doesn't hang on");
    play (1.0f, 20);
    setNoteExpressions (false);
    play (1.0f, 20, false);
    check (stopAndListen() < 1.0e-4f, "nor one held while they went off");
    setNoteExpressions (true);

    std::printf ("The synth's pitch-bend parameter following the notes isn't a knob tweak to the host\n");
    {
        // Analog Lab and Kontakt map each channel's pitch bend to a parameter of their own ("channel1
        // pitch bend"...), which JUCE moves with every bend MPE Roll sends. Reported to FL as a change,
        // each bend would fill its undo history and record as automation: MPE Roll tells such a
        // parameter by the synth's MIDI mapping, not by its name.
        setNoteExpressions (false);     // over MIDI, a zone bend goes to the synth's pitch bend on channel 1
        juce::AudioProcessorParameter* bendSlot = nullptr;

        for (auto* p : processor.getParameters())
            if (p->getName (100) == "channel1 pitch bend")
                bendSlot = p;

        check (bendSlot != nullptr, "the synth's channel1 pitch bend is one of MPE Roll's parameters");

        struct HeardByHost : juce::AudioProcessorParameter::Listener
        {
            void parameterValueChanged (int, float) override        { ++changes; }
            void parameterGestureChanged (int, bool) override       {}
            int changes = 0;
        };

        HeardByHost heard;

        if (bendSlot != nullptr)
            bendSlot->addListener (&heard);

        MpeClip clip;
        clip.length = 8.0;
        clip.addNote (note (60, 0.0, 8.0, 0.8f));
        clip.zone.pitch = { { 0.0, 0.0f }, { 2.0, 1.0f } };     // the zone bends up a semitone over two beats
        processor.getDocument().replaceClip (0, clip, "Test");

        playHead.playing = false;
        double ppq = 0.0;
        runBlock (processor, playHead, ppq);
        playHead.playing = true;
        playHead.ppq = 0.0;
        play (0.0f, 40, false);

        const auto synthBend = synth.getInstance() != nullptr ? synth.getInstance()->getParameters()[0]->getValue() : 0.5f;
        std::printf ("  the synth's parameter is at %.3f; the host heard %d change(s)\n", (double) synthBend, heard.changes);
        check (std::abs (synthBend - 0.5f) > 0.05f, "the synth's own parameter moved with the bend");
        check (heard.changes == 0, "and the host heard nothing of it");

        if (bendSlot != nullptr)
            bendSlot->removeListener (&heard);

        stopAndListen();
        setNoteExpressions (true);
    }

    std::printf ("A zone bend that went as MIDI doesn't stay on the synth once note expressions are back\n");
    {
        // A held C4 with the zone bent up a semitone. Over MIDI the synth takes the zone's bend on
        // channel 1 (its global bend); with note expressions the bend is part of each note's tuning,
        // so the global bend has to go back to rest, or the note sounds a whole tone up. The synth
        // is loaded afresh with note expressions off, so it has only ever been played over MIDI:
        // MPE Roll's own reset at the switch goes into the translator (and is dropped there), so
        // only the switch itself puts the bend back.
        setNoteExpressions (false);

        if (! loadSynth (processor, noteExpressionSynth))
            return 1;

        MpeClip clip;
        clip.length = 8.0;
        clip.addNote (note (60, 0.0, 8.0, 0.8f));
        clip.zone.pitch = { { 0.0, 1.0f } };
        processor.getDocument().replaceClip (0, clip, "Test");

        playHead.playing = false;
        double ppq = 0.0;
        runBlock (processor, playHead, ppq);
        playHead.playing = true;
        playHead.ppq = 0.0;
        play (0.0f, 20, false);
        setNoteExpressions (true);

        // 16 blocks: long enough to tell a semitone from a whole tone apart at this pitch.
        juce::AudioBuffer<float> tail (1, 16 * 512);

        for (int block = 0; block < 40; ++block)
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi);
            playHead.ppq += 512.0 / 48000.0 * 2.0;

            if (block >= 24)
                tail.copyFrom (0, (block - 24) * 512, audio, 0, 0, 512);
        }

        const auto bentUp = goertzel (tail.getReadPointer (0), tail.getNumSamples(), juce::MidiMessage::getMidiNoteInHertz (61), 48000.0);
        const auto twice = goertzel (tail.getReadPointer (0), tail.getNumSamples(), juce::MidiMessage::getMidiNoteInHertz (62), 48000.0);
        std::printf ("  C#4 %.4f, D4 %.4f\n", bentUp, twice);
        check (bentUp > 0.005 && bentUp > 5.0 * twice, "the note sounds a semitone up (the zone's bend), not a whole tone");
        stopAndListen();
    }

    std::printf ("A VST3 synth that maps pitch bend on every channel keeps MPE over MIDI\n");

    if (! loadSynth (processor, midiSynth))
        return 1;

    std::printf ("%s\n", synth.getVst3Support().describe().toRawUTF8());
    check (synth.getVst3Support().ownBendOnMemberChannels && ! synth.getVst3Support().midiMissesNotes()
               && ! synth.playsNoteExpressions (processor.getDocument().getSettings().output),
           "it sees each channel's own pitch bend, and plays it with MPE over MIDI");

    processor.releaseResources();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

//==============================================================================

/** Drives a component with synthetic mouse gestures. */
void gesture (juce::Component& target, juce::Point<float> from, juce::Point<float> to,
              juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), int steps = 10)
{
    const auto now = juce::Time::getCurrentTime();
    auto event = [&] (juce::Point<float> pos, bool dragged)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &target, &target, now, from, now, 1, dragged);
    };

    target.mouseDown (event (from, false));

    for (int i = 1; i <= steps; ++i)
        target.mouseDrag (event (from + (to - from) * ((float) i / (float) steps), from != to));

    target.mouseUp (event (to, from != to));
}

// The scanner's part of the editor: the scan window, and what Load plugin file... says of FL's own plugins.
void scannerUiTest (Check& check)
{
    auto& mm = *juce::MessageManager::getInstance();
    ignoreXErrors();

    const auto scanWindows = []
    {
        int count = 0;
        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* window = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)))
                count += window->getName() == "MPE Roll: scan for plugins" ? 1 : 0;

        return count;
    };

    std::printf ("The scan window\n");
    {
        auto processor = std::make_unique<MpeRollProcessor>();
        processor->getSynth().showScanWindow();
        mm.runDispatchLoopUntil (100);
        check (scanWindows() == 1, "Scan for plugins... opens it");

        processor->getSynth().showScanWindow();
        mm.runDispatchLoopUntil (50);
        check (scanWindows() == 1, "and brings it to the front the second time, instead of opening another");

        // As FL deleting MPE Roll while it's open: the window scans into the synth's list and settings.
        processor.reset();
        mm.runDispatchLoopUntil (100);
        check (scanWindows() == 0, "it goes with MPE Roll, so it can't scan into a synth that's gone");
    }

    std::printf ("FL Studio's own plugins\n");
    {
        const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto harmor = temp.getChildFile ("Image-Line").getChildFile ("FL Studio 2024").getChildFile ("Plugins")
                                .getChildFile ("Fruity").getChildFile ("Generators").getChildFile ("Harmor").getChildFile ("Harmor_x64.dll");
        check (HostedSynth::isFlStudioNativePlugin (harmor), "a file under FL's Plugins\\Fruity folder is one");
        check (! HostedSynth::isFlStudioNativePlugin (temp.getChildFile ("VST3").getChildFile ("Surge XT.vst3")), "a synth elsewhere isn't");

        HostedSynth synth;
        juce::String error;
        bool answered = false;
        synth.loadFromFile (harmor, [&] (const juce::String& e) { error = e; answered = true; });

        for (int i = 0; i < 100 && ! answered; ++i)
            mm.runDispatchLoopUntil (50);

        check (answered && error.contains ("FL Studio's own") && error.contains ("Send to FL piano roll"),
               "Load plugin file... says only FL can load it, and points to Send to FL piano roll");
    }
}

/** The first label under a component whose text contains the given words. */
juce::Label* findLabelSaying (juce::Component& parent, const juce::String& words)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText().contains (words))
            return label;

        if (auto* found = findLabelSaying (*child, words))
            return found;
    }

    return nullptr;
}

/** The editor's UI: the glide tool dragged back from where it was pressed, the paintbrush pointer, and
    the settings call-out's Window line and lifetime. Has an editor of its own, so it can take the
    editor and the processor down with the settings open. */
void editorUiChecks (Check& check)
{
    auto& mm = *juce::MessageManager::getInstance();
    ignoreXErrors();
    juce::SharedResourcePointer<SkinManager> skins;
    const auto lookFile = juce::File::createTempFile (".settings");
    skins->useSettingsFile (lookFile);
    juce::Component::SafePointer<juce::CallOutBox> settingsBox;

    {
        MpeRollProcessor processor;
        auto& document = processor.getDocument();
        std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
        editor->setSize (1200, 760);
        mm.runDispatchLoopUntil (100);

        auto* grid = findChild<NoteGrid> (*editor);
        auto* bar = findChild<Toolbar> (*editor);
        std::printf ("Glide tool, dragging back\n");
        check (grid != nullptr && bar != nullptr, "the editor has a grid and a toolbar");

        if (grid != nullptr && bar != nullptr)
        {
            const auto& view = grid->getView();
            auto at = [&] (double beat, float pitch) { return juce::Point<float> ((float) view.beatToX (beat), view.pitchToY (pitch)); };
            auto clip = [&]() -> const MpeClip& { return document.getEditClip(); };

            gesture (*grid, at (2.03, 60.0f), at (2.03, 60.0f));
            check (clip().notes.size() == 1 && clip().notes[0].lanes.pitch.isEmpty(), "a new note has no glide");

            // Pressed on the grid at beat 3, a beat into the note, and dragged back to beat 2.5 and up 2 st.
            grid->keyPressed (juce::KeyPress ('g'));
            gesture (*grid, at (2.97, 60.0f), at (2.5, 62.0f));

            if (clip().notes.size() == 1)
            {
                const auto& scoop = clip().notes[0].lanes.pitch;
                const auto& points = scoop.getPoints();
                check (points.size() == 2
                           && std::abs (points[0].beat - 0.5) < 1.0e-9 && std::abs (points[0].value - 2.0f) < 1.0e-6f
                           && std::abs (points[1].beat - 1.0) < 1.0e-9 && std::abs (points[1].value) < 1.0e-6f,
                       "dragged back from where it was pressed, a snapped glide scoops straight into the note's pitch there");
                check (std::abs (scoop.valueAt (0.0, 0.0f) - 2.0f) < 1.0e-6f && std::abs (scoop.valueAt (0.25, 0.0f) - 2.0f) < 1.0e-6f,
                       "holding the pen's pitch from the note's start rather than rising to it");
            }

            // The same drawn freely, with Alt held: the scoop comes out the same way, in the pen's own shape.
            const auto doubleClick = [&] (juce::Point<float> where)
            {
                const auto now = juce::Time::getCurrentTime();
                grid->mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), where, juce::ModifierKeys(),
                                                          1.0f, 0.0f, 0.0f, 0.0f, 0.0f, grid, grid, now, where, now, 2, false));
            };
            const juce::ModifierKeys alt (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);

            doubleClick (at (2.5, 60.0f));
            check (clip().notes.size() == 1 && clip().notes[0].lanes.pitch.isEmpty(), "double-click clears the glide");
            gesture (*grid, at (2.97, 60.0f), at (2.5, 62.0f), alt);

            if (clip().notes.size() == 1)
            {
                const auto& scoop = clip().notes[0].lanes.pitch;
                const auto& points = scoop.getPoints();
                check (points.size() >= 2
                           && std::abs (points.front().beat - 0.5) < 1.0e-4 && std::abs (points.front().value - 2.0f) < 1.0e-3f
                           && std::abs (points.back().beat - 0.97) < 1.0e-4 && std::abs (points.back().value) < 1.0e-3f,
                       "drawn freely with Alt held, a glide dragged back scoops into the note's pitch where it was pressed");
                check (std::abs (scoop.valueAt (0.0, 0.0f) - 2.0f) < 1.0e-3f && std::abs (scoop.valueAt (0.25, 0.0f) - 2.0f) < 1.0e-3f,
                       "holding the pen's pitch from the note's start");
            }

            // Drawn forwards from inside the note, the note's own pitch is held up to where it was pressed.
            doubleClick (at (2.5, 60.0f));
            gesture (*grid, at (2.5, 60.4f), at (2.97, 62.0f), alt);

            if (clip().notes.size() == 1)
            {
                const auto& glide = clip().notes[0].lanes.pitch;
                check (! glide.isEmpty() && std::abs (glide.valueAt (0.0, 9.0f)) < 1.0e-3f && std::abs (glide.valueAt (0.5, 9.0f) - 0.4f) < 1.0e-3f
                           && std::abs (glide.valueAt (0.97, 0.0f) - 2.0f) < 1.0e-3f,
                       "drawn forwards from inside the note, it leaves the note's own pitch, held from the start");
            }

            // Pressed 0.4 st above the note's pitch and let go without moving, or slipping a pixel either
            // way as clicks do: the point under the pen would otherwise be the whole note's pitch.
            for (const int slip : { 0, 1, -1 })
            {
                doubleClick (at (2.5, 60.0f));
                gesture (*grid, at (2.5, 60.4f), at (2.5, 60.4f).translated ((float) slip, 0.0f), alt);

                if (clip().notes.size() == 1)
                    check (clip().notes[0].lanes.pitch.isEmpty(),
                           "an Alt-click on a note without a glide draws nothing, leaving the note at its own pitch"
                               + (slip == 0 ? juce::String() : " (slipping " + juce::String (slip) + " px)"));
            }

            std::printf ("Paint tool pointer\n");
            {
                grid->keyPressed (juce::KeyPress ('b'));
                const auto now = juce::Time::getCurrentTime();
                const juce::MouseEvent hover (juce::Desktop::getInstance().getMainMouseSource(), at (5.0, 60.0f), juce::ModifierKeys(),
                                              1.0f, 0.0f, 0.0f, 0.0f, 0.0f, grid, grid, now, at (5.0, 60.0f), now, 0, false);
                grid->mouseMove (hover);
                const auto brush = grid->getMouseCursor();
                check (brush != juce::MouseCursor::NormalCursor, "the Paint tool has a pointer of its own");

                skins->setPreset (1);
                mm.runDispatchLoopUntil (50);
                grid->mouseMove (hover);
                check (grid->getMouseCursor() != brush, "which is drawn again in a new skin's colours");
                grid->keyPressed (juce::KeyPress ('p'));
            }

            std::printf ("The settings call-out\n");
            editor->addToDesktop (0);
            editor->setVisible (true);
            mm.runDispatchLoopUntil (200);

            bar->settingsButton.triggerClick();
            mm.runDispatchLoopUntil (50);
            settingsBox = findChild<juce::CallOutBox> (*editor);
            check (settingsBox != nullptr, "the MPE settings button opens them in a call-out");

            if (auto* box = settingsBox.getComponent())
            {
                check (findLabelSaying (*box, "scale, drawn by") != nullptr,
                       "whose Window line says how the window is drawn from the moment it opens");

                auto* panel = findChild<SettingsPanel> (*box);
                auto* link = panel != nullptr ? findChild<juce::HyperlinkButton> (*panel) : nullptr;
                check (link != nullptr && link->isShowing() && panel->getLocalBounds().contains (link->getBounds()),
                       "a Support MPE Roll link is in view at the foot of the settings");

                // Its words fit, in this system's font: a link of the same height sized to them is no wider.
                if (link != nullptr)
                {
                    juce::HyperlinkButton sized (link->getButtonText(), {});
                    sized.setSize (100, link->getHeight());
                    sized.changeWidthToFitText();
                    check (link->getWidth() >= sized.getWidth(),
                           "with room for all its words (" + juce::String (link->getWidth()) + " px for "
                               + juce::String (sized.getWidth()) + ")");
                }

                // So a tester can tell which build their DAW loaded.
                auto* build = panel != nullptr ? findLabelSaying (*panel, getBuildName()) : nullptr;
                check (build != nullptr && build->isShowing() && panel->getLocalBounds().contains (build->getBounds())
                           && ! build->getBounds().intersects (link != nullptr ? link->getBounds() : juce::Rectangle<int>()),
                       "and beside it, which build this is: " + getBuildName());

                // It opens the donation details in MPE Roll itself, not a web page that may not be there.
                if (link != nullptr)
                {
                    check (! link->getURL().isWellFormed(), "and leads to no web address");
                    link->triggerClick();
                    mm.runDispatchLoopUntil (100);
                }

                juce::DialogWindow* support = nullptr;

                for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
                    if (auto* window = dynamic_cast<juce::DialogWindow*> (juce::Desktop::getInstance().getComponent (i)))
                        if (window->getName() == "Support MPE Roll")
                            support = window;

                auto* donatePanel = support != nullptr ? dynamic_cast<DonatePanel*> (support->getContentComponent()) : nullptr;
                check (donatePanel != nullptr, "but opens a Support MPE Roll window of its own");

                if (donatePanel != nullptr)
                {
                    check (donatePanel->getNumQrCodes() == 4, "with the four QR codes");
                    check (donatePanel->getAddress ("BTC") == "bc1qw07yj62hae5nflfz6pwt8yyr57377adkclsy3t"
                               && donatePanel->getAddress ("XMR").startsWith ("84L4Twd2DcNjHevR6qUzz"),
                           "and the addresses from docs/donate.json");

                    if (auto* copy = donatePanel->getCopyButton ("ETH"))
                    {
                        copy->triggerClick();       // posted, so let it through
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
                        check (juce::SystemClipboard::getTextFromClipboard() == "0x79Ff39eCD0D2520c5353e011ac1317C51dc2AC3A"
                                   && copy->getButtonText().startsWith ("Copied"),
                               "whose Copy button puts the address on the clipboard");
                    }

                    const auto work = WindowPlacement::workAreaNear (editor.get());
                    const auto outer = WindowPlacement::frameOf (*support).addedTo (support->getBounds());
                    check (work.contains (outer.getTopLeft()), "and which opens with its title bar on screen");
                }

                delete support;
            }

            // As FL closing the window and then the plugin with the settings open, while a skin change
            // (from another MPE Roll's settings) is still on its way: the call-out is deleted later, by
            // the message loop, and must no longer reach the processor.
            skins->setPreset (2);
            editor->removeFromDesktop();
        }

        processor.editorBeingDeleted (editor.get());
        editor.reset();
    }

    mm.runDispatchLoopUntil (100);
    check (settingsBox == nullptr, "closing the window and the plugin with the settings open is safe, and the call-out goes");

    skins->setPreset (0);
    skins->flush();
    mm.runDispatchLoopUntil (50);
    lookFile.deleteFile();
}

// What MPE Roll says about the MPE of synths it knows, matched on their names and makers.
void synthProfileChecks (Check& check)
{
    std::printf ("Known synths are recognised, with where their MPE switch is\n");

    const auto find = [] (const char* name, const char* maker, const char* version = "")
    {
        juce::PluginDescription d;
        d.name = name;
        d.manufacturerName = maker;
        d.version = version;
        return SynthProfile::find (d);
    };

    using Mpe = SynthProfile::Mpe;
    const auto serum = find ("Serum", "Xfer Records"), serum2 = find ("Serum2", "Xfer Records");
    check (serum.has_value() && serum->name == "Serum" && serum->mpe == Mpe::needsSwitch && serum->advice.contains ("MPE Enabled")
               && serum2.has_value() && serum2->name == "Serum 2" && ! find ("Serum FX", "Xfer Records").has_value(),
           "Serum and Serum 2, with their MPE Enabled switch, and not Serum FX");

    const auto analogLab = find ("Analog Lab V", "Arturia"), zenology = find ("ZENOLOGY", "Roland");
    check (analogLab.has_value() && analogLab->mpe == Mpe::none && analogLab->advice.contains ("Pigments")
               && analogLab->advice.contains ("Plain MIDI") && zenology.has_value() && zenology->mpe == Mpe::none,
           "Analog Lab and ZENOLOGY as having no MPE, with Plain MIDI to use instead");

    const auto omni2 = find ("Omnisphere", "Spectrasonics", "2.8.6f"), omni3 = find ("Omnisphere", "Spectrasonics", "3.0.1c");
    check (omni2.has_value() && omni2->mpe == Mpe::none && omni3.has_value() && omni3->mpe == Mpe::needsSwitch
               && omni3->advice.contains ("System page"),
           "Omnisphere 2 without MPE, and 3 with it on its System page");

    check (find ("Vital", "Vital Audio").has_value() && find ("Surge XT", "Surge Synth Team")->mpe == Mpe::followsSetup
               && ! find ("MPE Roll Test Synth", "Glowing Pancake").has_value(),
           "Vital and Surge XT, and nothing for a synth it doesn't know");

    // What the list says for any instrument of a maker goes by the instrument's own name; and a maker
    // matched exactly ("Waves") isn't one whose name merely starts the same.
    const auto newArturia = find ("Brand New V", "Arturia"), waves = find ("Some Synth", "Waves");
    const auto cherry = find ("CA2600", "Cherry Audio");
    check (newArturia.has_value() && newArturia->name == "Brand New V" && ! find ("Brand New V", "").has_value()
               && cherry.has_value() && cherry->name == "CA2600" && cherry->mpe == Mpe::none
               && waves.has_value() && waves->mpe == Mpe::none && ! find ("Some Synth", "Wavesfactory").has_value(),
           "a maker's whole range by the synth's own name, and exact makers");

    // From the audit (docs/MPE_SYNTHS.md): an MPE switch, one whose maker never mentions MPE, and
    // names that start the same.
    const auto icarus = find ("Icarus", "Tone2"), one = find ("FabFilter One", "FabFilter"), nexus = find ("Nexus", "reFX");
    const auto massive = find ("Massive", "Native Instruments"), massiveX = find ("Massive X", "Native Instruments");
    check (icarus.has_value() && icarus->mpe == Mpe::needsSwitch && icarus->advice.containsIgnoreCase ("force MPE")
               && one.has_value() && one->mpe == Mpe::none && ! one->certain && one->describeNone().startsWith ("most likely")
               && nexus.has_value() && nexus->name == "Nexus" && ! nexus->certain
               && massive.has_value() && massive->name == "Massive" && massiveX.has_value() && massiveX->name == "Massive X",
           "audited synths: Icarus's switch, 'most likely no MPE' for FabFilter One and Nexus, Massive vs Massive X");
}

int uiTest()
{
    Check check;
    MpeRollProcessor processor;
    processor.setRateAndBufferSizeDetails (48000.0, 512);
    processor.prepareToPlay (48000.0, 512);
    auto& document = processor.getDocument();

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditorAndMakeActive());
    editor->setSize (1200, 760);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    auto* grid = findChild<NoteGrid> (*editor);
    auto* lane = findChild<ExpressionLane> (*editor);

    if (grid == nullptr || lane == nullptr)
    {
        std::printf ("Couldn't find the grid and lane\n");
        return 1;
    }

    const auto& view = grid->getView();
    auto at = [&] (double beat, float pitch) { return juce::Point<float> ((float) view.beatToX (beat), view.pitchToY (pitch)); };
    auto clip = [&]() -> const MpeClip& { return document.getEditClip(); };
    const juce::ModifierKeys right (juce::ModifierKeys::rightButtonModifier);

    std::printf ("Grid editing\n");
    gesture (*grid, at (1.03, 60.0f), at (1.03, 60.0f));
    check (clip().notes.size() == 1 && clip().notes[0].key == 60 && std::abs (clip().notes[0].start - 1.0) < 1.0e-9,
           "click adds a note on the snapped grid");

    gesture (*grid, at (1.5, 60.0f), at (3.5, 62.0f));
    check (clip().notes.size() == 1 && clip().notes[0].key == 62 && std::abs (clip().notes[0].start - 3.0) < 1.0e-9,
           "dragging moves it in time and pitch");

    gesture (*grid, at (3.97, 62.0f), at (4.97, 62.0f));
    check (std::abs (clip().notes[0].length - 2.0) < 1.0e-9, "dragging the right edge resizes it");

    grid->keyPressed (juce::KeyPress ('g'));
    gesture (*grid, at (3.0, 62.0f), at (5.0, 65.0f), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 30);
    const auto& glide = clip().notes[0].lanes.pitch;
    check (! glide.isEmpty() && std::abs (glide.valueAt (2.0, 0.0f) - 3.0f) < 0.2f, "the glide tool draws pitch onto the note");

    // Off the grid and between semitones on purpose: the glide still lands on beat 4.25 and +3.
    grid->mouseDoubleClick (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at (3.5, 62.0f),
                                              juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, grid, grid,
                                              juce::Time::getCurrentTime(), at (3.5, 62.0f), juce::Time::getCurrentTime(), 2, false));
    gesture (*grid, at (3.6, 62.0f), at (4.37, 64.6f));
    {
        const auto& points = clip().notes[0].lanes.pitch.getPoints();
        bool onGrid = ! points.empty();

        for (const auto& p : points)
            onGrid = onGrid && std::abs (p.beat * 4.0 - std::round (p.beat * 4.0)) < 1.0e-9
                            && std::abs (p.value - std::round (p.value)) < 1.0e-6f;

        const auto& snapped = clip().notes[0].lanes.pitch;
        check (onGrid && std::abs (snapped.valueAt (0.5, 9.0f)) < 1.0e-6f && std::abs (snapped.valueAt (1.25, 0.0f) - 3.0f) < 1.0e-6f
                   && std::abs (snapped.valueAt (2.0, 0.0f) - 3.0f) < 1.0e-6f,
               "a snapped glide runs straight from the grid to a whole semitone on the grid");
    }

    gesture (*grid, at (3.0, 62.0f), at (4.9, 63.6f),
             juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier), 20);
    {
        bool between = false;

        for (const auto& p : clip().notes[0].lanes.pitch.getPoints())
            between = between || std::abs (p.value - std::round (p.value)) > 0.1f;

        check (between, "with Alt held, it's drawn freely");
    }

    auto* bar = findChild<Toolbar> (*editor);
    check (bar != nullptr && bar->glideSnapButton.getToggleState(), "Snap glide is on to start with");

    if (bar != nullptr)
    {
        bar->glideSnapButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (! view.snapGlide, "and the toolbar turns it off");
        bar->glideSnapButton.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }

    std::printf ("Paint tool\n");
    {
        // The last note placed is 2 beats long, so a drag from beat 8.1 to 13.9 paints at 8, 10 and 12.
        grid->keyPressed (juce::KeyPress ('b'));
        check (view.tool == ViewState::Tool::paint, "B picks the Paint tool");

        const auto before = clip().notes.size();
        const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        gesture (*grid, at (8.1, 72.0f), at (13.9, 72.0f), left, 12);

        std::vector<double> starts;

        for (const auto& n : clip().notes)
            if (n.key == 72)
                starts.push_back (n.start);

        check (clip().notes.size() == before + 3 && (starts == std::vector<double> { 8.0, 10.0, 12.0 }),
               "dragging paints snapped notes one after another");
        check (std::all_of (clip().notes.begin(), clip().notes.end(),
                            [] (const MpeNote& n) { return n.key != 72 || std::abs (n.length - 2.0) < 1.0e-9; }),
               "each as long as the last note placed");

        gesture (*grid, at (13.9, 72.0f), at (8.1, 72.0f), left, 12);
        check (clip().notes.size() == before + 3, "painting back over them adds nothing (and leaves nothing to undo)");

        document.getUndoManager().undo();
        check (clip().notes.size() == before, "one undo takes the whole stroke away");
    }

    grid->keyPressed (juce::KeyPress ('p'));

    std::printf ("Expression lane\n");
    {
        const auto plotTop = (float) ExpressionLane::headerHeight + 4.0f;
        const auto plotBottom = (float) lane->getHeight() - 4.0f;
        auto laneAt = [&] (double beat, float value)
        {
            return juce::Point<float> ((float) ViewState::keysWidth + (float) view.beatToX (beat),
                                       plotBottom - value * (plotBottom - plotTop));
        };

        gesture (*lane, laneAt (3.0, 0.2f), laneAt (5.0, 0.8f), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 40);
        const auto& pressure = clip().notes[0].lanes.pressure;
        check (! pressure.isEmpty(), "drawing in the pressure lane adds a curve");
        check (std::abs (pressure.valueAt (0.0, 0.0f) - 0.2f) < 0.03f && std::abs (pressure.valueAt (2.0, 0.0f) - 0.8f) < 0.03f,
               "the curve follows the pen");

        gesture (*lane, laneAt (3.5, 0.5f), laneAt (4.5, 0.5f), right);
        const auto erased = std::none_of (pressure.getPoints().begin(), pressure.getPoints().end(),
                                          [] (const CurvePoint& p) { return p.beat > 0.55 && p.beat < 1.45; });
        check (erased && ! pressure.isEmpty(), "right-dragging erases just the points under the pen");

        lane->setTarget (ExpressionLane::Target::noteVibrato);
        gesture (*lane, laneAt (3.0, 0.6f), laneAt (5.0, 0.6f), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 40);
        const auto& vibrato = clip().notes[0].lanes.vibrato;
        check (! vibrato.isEmpty() && std::abs (vibrato.valueAt (1.0, 0.0f) - 0.6f) < 0.03f, "the vibrato lane draws depth onto the note");

        if (auto* rate = findChild<juce::Slider> (*lane))
        {
            rate->setValue (7.0, juce::sendNotificationSync);
            check (std::abs (clip().vibratoRate - 7.0) < 1.0e-9, "its speed is set next to the lane menu");
            document.getUndoManager().undo();
            check (std::abs (clip().vibratoRate - defaultVibratoRate) < 1.0e-9, "and can be undone");
        }
        else
        {
            check (false, "the vibrato lane has a speed control");
        }

        lane->setTarget (ExpressionLane::Target::notePressure);
    }

    std::printf ("Selection, clipboard and undo\n");
    {
        grid->keyPressed (juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0));
        grid->keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0));
        check (clip().notes.size() == 2 && std::abs (clip().notes[1].start - 5.0) < 1.0e-9, "Ctrl+B duplicates after the selection (FL's key)");

        grid->keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        check (clip().notes[1].key == 63, "Up transposes the selection");

        gesture (*grid, at (3.5, 62.0f), at (3.5, 62.0f), right);
        check (clip().notes.size() == 1, "right-click deletes a note");

        document.getUndoManager().undo();
        check (clip().notes.size() == 2, "undo brings it back");
    }

    std::printf ("FL's tools and keys\n");
    {
        auto& mm = *juce::MessageManager::getInstance();
        ignoreXErrors();

        // A known clip: a C major chord two beats long at beat 8, and a lone note at beat 12 that
        // glides up two semitones.
        document.perform ("Test notes", [] (MpeClip& c)
        {
            c.notes.clear();

            for (int key : { 60, 64, 67 })
            {
                MpeNote n;
                n.key = key;
                n.start = 8.0;
                n.length = 2.0;
                c.addNote (n);
            }

            MpeNote lone;
            lone.key = 72;
            lone.start = 12.0;
            lone.length = 1.0;
            lone.lanes.pitch = ExpressionCurve ({ { 0.0, 0.0f }, { 1.0, 2.0f } });
            c.addNote (lone);
        });
        document.selection.clear();
        document.selectionChanged();

        // Notes are kept sorted by start, so they're found by key here.
        auto noteWithKey = [&] (int key) -> const MpeNote*
        {
            for (const auto& n : clip().notes)
                if (n.key == key)
                    return &n;

            return nullptr;
        };

        // FL's two quick quantizes: Shift+Q lands starts on the grid, Ctrl+Q ends as well.
        document.perform ("Off the grid", [] (MpeClip& c)
        {
            for (auto& n : c.notes)
                if (n.key == 60)
                {
                    n.start = 8.1;
                    n.length = 1.9;
                }
        });
        grid->keyPressed (juce::KeyPress ('q', juce::ModifierKeys::shiftModifier, 0));
        check (noteWithKey (60) != nullptr && std::abs (noteWithKey (60)->start - 8.0) < 1.0e-9
                   && std::abs (noteWithKey (60)->length - 1.9) < 1.0e-9,
               "Shift+Q quantizes starts, leaving lengths alone");
        editor->keyPressed (juce::KeyPress ('q', juce::ModifierKeys::commandModifier, 0));
        check (noteWithKey (60) != nullptr && std::abs (noteWithKey (60)->start - 8.0) < 1.0e-9
                   && std::abs (noteWithKey (60)->length - 2.0) < 1.0e-9,
               "Ctrl+Q quantizes ends too, as FL's quick quantize does");

        grid->keyPressed (juce::KeyPress ('c'));
        check (view.tool == ViewState::Tool::slice, "C picks the Slice tool, as in FL");
        grid->keyPressed (juce::KeyPress ('d'));
        check (view.tool == ViewState::Tool::erase, "D the Delete tool");
        grid->keyPressed (juce::KeyPress ('e'));
        check (view.tool == ViewState::Tool::select, "E the Select tool");
        grid->keyPressed (juce::KeyPress ('b'));
        check (view.tool == ViewState::Tool::paint, "B the Paint tool");
        grid->keyPressed (juce::KeyPress ('p'));
        check (view.tool == ViewState::Tool::edit, "P the Draw tool");

        check (bar != nullptr && bar->drawToolButton.getToggleState() && ! bar->sliceToolButton.getToggleState(),
               "and the toolbar's icon buttons follow");

        // Slice: a click cuts the lone note at the snapped beat (12.53 lands on 12.5), the glide
        // carrying on across the cut.
        grid->keyPressed (juce::KeyPress ('c'));
        gesture (*grid, at (12.53, 72.0f), at (12.53, 72.0f));
        {
            std::vector<const MpeNote*> pieces;

            for (const auto& n : clip().notes)
                if (n.key == 72)
                    pieces.push_back (&n);

            check (pieces.size() == 2 && std::abs (pieces[0]->length - 0.5) < 1.0e-9
                       && std::abs (pieces[1]->start - 12.5) < 1.0e-9 && std::abs (pieces[1]->length - 0.5) < 1.0e-9,
                   "a click with the Slice tool cuts the note in two at the snapped beat");
            check (pieces.size() == 2
                       && std::abs (pieces[0]->lanes.pitch.valueAt (0.5, 0.0f) - 1.0f) < 1.0e-5f
                       && std::abs (pieces[1]->lanes.pitch.valueAt (0.0, 0.0f) - 1.0f) < 1.0e-5f
                       && std::abs (pieces[1]->lanes.pitch.valueAt (0.5, 0.0f) - 2.0f) < 1.0e-5f,
                   "and the glide carries on across the cut");
        }

        gesture (*grid, at (9.02, 68.0f), at (9.02, 59.0f));
        check (clip().notes.size() == 8, "dragging a slice line down across the chord cuts every note it crosses");
        document.getUndoManager().undo();
        check (clip().notes.size() == 5, "as one undo step");

        grid->keyPressed (juce::KeyPress ('d'));
        gesture (*grid, at (12.2, 72.0f), at (12.2, 72.0f));
        check (clip().notes.size() == 4, "the Delete tool removes the note under a click");

        grid->keyPressed (juce::KeyPress ('e'));
        gesture (*grid, at (7.5, 69.0f), at (10.5, 58.0f));
        check (document.selection.size() == 3, "the Select tool's box selects the chord");
        gesture (*grid, at (12.7, 72.0f), at (12.7, 72.0f));
        check (document.selection.size() == 1 && clip().findNote (*document.selection.begin()) != nullptr
                   && clip().findNote (*document.selection.begin())->key == 72,
               "a click selects just that note");
        gesture (*grid, at (14.0, 50.0f), at (14.0, 50.0f));
        check (document.selection.empty(), "and a click on empty space selects nothing");

        grid->keyPressed (juce::KeyPress ('i', juce::ModifierKeys::shiftModifier, 0));
        check (document.selection.size() == 4, "Shift+I inverts the selection, as in FL");
        grid->keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::commandModifier, 0));
        check (noteWithKey (60) == nullptr && noteWithKey (76) != nullptr && noteWithKey (84) != nullptr,
               "Ctrl+Up transposes an octave, as in FL");
        document.getUndoManager().undo();
        grid->keyPressed (juce::KeyPress ('i', juce::ModifierKeys::shiftModifier, 0));
        check (document.selection.empty(), "and again puts it back");

        // The Tools' keys reach the editor. Nothing selected: every note is worked on, as in FL.
        editor->keyPressed (juce::KeyPress ('u', juce::ModifierKeys::commandModifier, 0));
        check (clip().notes.size() == 3 * 8 + 2, "Ctrl+U quick-chops every note at the grid when nothing is selected");
        document.getUndoManager().undo();
        check (clip().notes.size() == 4, "in one undo step");

        editor->keyPressed (juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0));
        check (std::all_of (clip().notes.begin(), clip().notes.end(),
                            [] (const MpeNote& n) { return n.key == 72 || std::abs (n.length - 4.5) < 1.0e-9; }),
               "Ctrl+L quick legato lengthens the chord to where the next note starts");
        document.getUndoManager().undo();

        // Alt+A opens the Arpeggiate dialog, which previews on the clip: Escape puts the notes
        // back and leaves nothing to undo, Return keeps the result as one undo step.
        editor->addToDesktop (0);
        editor->setVisible (true);
        mm.runDispatchLoopUntil (100);
        const auto undoBefore = document.getUndoManager().getUndoDescription();

        auto openDialog = [&]() -> NoteToolDialog*
        {
            editor->keyPressed (juce::KeyPress ('a', juce::ModifierKeys::altModifier, 0));
            mm.runDispatchLoopUntil (100);
            auto* box = findChild<juce::CallOutBox> (*editor);
            return box != nullptr ? findChild<NoteToolDialog> (*box) : nullptr;
        };

        auto* dialog = openDialog();
        check (dialog != nullptr, "Alt+A opens the Arpeggiate dialog in a call-out");
        check (clip().notes.size() == 10, "which previews the arpeggio on the clip: eight steps for the chord, two for the lone note");

        if (dialog != nullptr)
        {
            dialog->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            mm.runDispatchLoopUntil (100);
        }

        check (clip().notes.size() == 4 && document.getUndoManager().getUndoDescription() == undoBefore,
               "Escape puts the notes back and leaves nothing to undo");

        dialog = openDialog();

        if (dialog != nullptr)
        {
            dialog->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            mm.runDispatchLoopUntil (100);
        }

        check (clip().notes.size() == 10 && document.getUndoManager().getUndoDescription() == "Arpeggiate",
               "Return keeps the arpeggio as one undo step");
        document.getUndoManager().undo();
        check (clip().notes.size() == 4, "which undoes it whole");
        editor->removeFromDesktop();
        grid->keyPressed (juce::KeyPress ('p'));
    }

    std::printf ("Windows are placed on screen\n");
    {
        // A synth window taller than the screen, as the user saw: 1000 x 1400 on a 1920 x 1040 work
        // area under a 31-pixel title bar.
        const juce::Rectangle<int> work (0, 0, 1920, 1040), tall (0, 0, 1000, 1400);
        const juce::BorderSize<int> frame (31, 8, 8, 8);

        const auto fixed = WindowPlacement::place (tall, frame, work, false, true);
        check (fixed.getY() == 31 && fixed.getWidth() == 1000 && fixed.getHeight() == 1400 && fixed.getX() == 460,
               "a window too tall to fit keeps its size, with its title bar at the top of the screen");

        const auto shrunk = WindowPlacement::place (tall, frame, work, true, true);
        check (shrunk.getY() == 31 && shrunk.getBottom() == 1040 - 8 && shrunk.getWidth() == 1000,
               "one that may be resized is shrunk to fit");

        const auto second = WindowPlacement::place ({ 0, 0, 800, 600 }, frame, { 1920, 0, 1280, 984 }, false, true);
        check (second.getX() == 1920 + 240 && second.getY() == 31 + (984 - 31 - 8 - 600) / 2,
               "and a window is centred on the display it's put on, not the main one");

        const auto off = WindowPlacement::place ({ -300, -200, 800, 600 }, frame, work, false, false);
        check (off.getX() == 8 && off.getY() == 31, "one gone off the top left is brought back inside");
    }

    std::printf ("The editor's window\n");
    {
        auto& mm = *juce::MessageManager::getInstance();
        ignoreXErrors();

        auto openWindow = [&]
        {
            editor->addToDesktop (0);
            editor->setVisible (true);
            mm.runDispatchLoopUntil (200);
        };

        openWindow();
        check (WindowDrawing::describe (*editor).contains ("scale, drawn by"), "Settings can say how the window is drawn");
        check (! WindowDrawing::resync (*editor), "a window JUCE knows the size of is left alone");

        // As a host closing the plugin window and opening a new one.
        editor->removeFromDesktop();
        mm.runDispatchLoopUntil (100);
        openWindow();

        // Through the window, as the mouse reaches the editor: it lands where it was clicked.
        if (auto* peer = editor->getPeer())
        {
            const auto where = grid->getPosition().toFloat() + at (0.53, 67.0f);
            const auto source = juce::MouseInputSource::InputSourceType::mouse;
            const auto down = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
            const auto time = juce::Time::currentTimeMillis();
            peer->handleMouseEvent (source, where, {}, juce::MouseInputSource::defaultPressure, 0.0f, time);
            peer->handleMouseEvent (source, where, down, juce::MouseInputSource::defaultPressure, 0.0f, time + 10);
            peer->handleMouseEvent (source, where, {}, juce::MouseInputSource::defaultPressure, 0.0f, time + 20);
        }

        const auto landed = std::any_of (clip().notes.begin(), clip().notes.end(), [] (const MpeNote& n)
                                         { return n.key == 67 && std::abs (n.start - 0.5) < 1.0e-9; });
        check (landed, "after the window is reopened, a click adds its note under the mouse");

        if (WindowDrawing::isChoiceAvailable())
        {
            WindowDrawing::apply (*editor, WindowDrawing::Mode::software);
            mm.runDispatchLoopUntil (100);
            check (WindowDrawing::describe (*editor).contains ("Software Renderer"), "the window can be drawn in software");
            WindowDrawing::apply (*editor, WindowDrawing::Mode::gpu);
            mm.runDispatchLoopUntil (100);
            check (WindowDrawing::describe (*editor).contains ("Direct2D"), "and back with Direct2D");
        }

        auto settingsFile = juce::File::createTempFile (".settings");
        {
            juce::PropertiesFile settings (settingsFile, {});
            check (WindowDrawing::getMode (&settings) == WindowDrawing::Mode::automatic, "window drawing starts on Automatic");
            WindowDrawing::setMode (&settings, WindowDrawing::Mode::software);
            check (WindowDrawing::getMode (&settings) == WindowDrawing::Mode::software, "and remembers a choice");
        }
        settingsFile.deleteFile();

        editor->removeFromDesktop();
    }

    std::printf ("Skins\n");
    {
        auto& mm = *juce::MessageManager::getInstance();
        juce::SharedResourcePointer<SkinManager> skins;
        const auto lookFile = juce::File::createTempFile (".settings");
        skins->useSettingsFile (lookFile);

        const auto& presets = presetSkins();
        check (presets.size() == 10, "there are ten skins");

        skins->setPreset (5);
        mm.runDispatchLoopUntil (50);
        check (Theme::rowWhite == presets[5].colours[(size_t) SkinRole::gridWhiteRows], "picking a skin recolours the grid");
        check (editor->getLookAndFeel().findColour (juce::TextButton::buttonColourId) == presets[5].colours[(size_t) SkinRole::buttons],
               "and the editor's buttons");

        skins->setColour (SkinRole::notes, juce::Colours::red);
        check (Theme::note == juce::Colours::red && skins->hasCustomColours(), "a colour of your own goes on top of the skin");

        const auto pictureFile = juce::File::createTempFile (".png");
        writePng (testPicture(), pictureFile);
        check (skins->setBackgroundFile (pictureFile) && skins->getBackgroundFor ({ 0, 0, 200, 100 }).getWidth() == 200,
               "a picture can go behind the grid, scaled to cover it");
        skins->setBackgroundStrength (0.6f);

        const auto notAPicture = juce::File::createTempFile (".png");
        notAPicture.replaceWithText ("not a picture");
        check (! skins->setBackgroundFile (notAPicture) && skins->getBackgroundFile() == pictureFile
                   && skins->getBackgroundFor ({ 0, 0, 200, 100 }).isValid(),
               "a file that isn't a picture is turned down, and the picture showing stays");
        skins->flush();

        {
            SkinManager other;
            other.useSettingsFile (lookFile);
            check (other.getPreset() == 5 && other.getColour (SkinRole::notes) == juce::Colours::red
                       && other.getBackgroundFile() == pictureFile && std::abs (other.getBackgroundStrength() - 0.6f) < 1.0e-6f,
                   "the look is kept, for every MPE Roll on the computer");
        }

        skins->setPreset (5);
        check (! skins->hasCustomColours() && Theme::note == presets[5].colours[(size_t) SkinRole::notes],
               "picking a skin again starts from its own colours");

        skins->clearBackground();
        skins->setPreset (0);
        skins->flush();
        mm.runDispatchLoopUntil (50);

        for (const auto& f : { lookFile, pictureFile, notAPicture })
            f.deleteFile();
    }

    scannerUiTest (check);
    editorUiChecks (check);
    synthProfileChecks (check);

    processor.editorBeingDeleted (editor.get());
    editor.reset();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

// Plugin files are looked at in the scanner process: a crash or hang there doesn't reach MPE Roll.
// Stands in for MPE Roll Scanner when scantest runs the harness as one. It makes up a plugin named after
// the file without loading anything. For a file named "too old" it turns the job down without reading
// it, as a scanner from another version of MPE Roll does; for "never starts" it goes down before it
// gets to the file, for "crashes" while it's looking, and for "crashes on the way out" once it has
// written what it found, as a plugin does whose clean-up code crashes. In "won't start" it finds
// nothing, as JUCE does in a VST3 whose component won't initialise (an unauthorised plugin).
int standInScanner (const juce::File& jobFile)
{
    PluginScanner::Job job;

    if (! PluginScanner::Job::parse (jobFile.loadFileAsString(), job))
        return 1;

    juce::PluginDescription d;
    d.pluginFormatName = job.formatName;
    d.fileOrIdentifier = job.fileOrIdentifier;
    d.name = juce::File (d.fileOrIdentifier).getFileNameWithoutExtension();
    d.uniqueId = d.deprecatedUid = d.name.hashCode();

    if (d.name == "too old")
        std::_Exit (1);

    PluginScanner::readyMarkerFor (jobFile).create();

    if (d.name == "never starts")
        std::_Exit (3);

    PluginScanner::startedMarkerFor (jobFile).create();

    if (d.name == "crashes")
        std::_Exit (3);

    juce::XmlElement list ("LIST");

    if (d.name != "won't start")
        list.addChildElement (d.createXml().release());

    PluginScanner::resultFileFor (jobFile).replaceWithText (list.toString());

    if (d.name == "crashes on the way out")
        std::_Exit (3);

    return 0;
}

int scanTest (const juce::File& scanner, const juce::File& synthFile, const juce::File& crashing, const juce::File& hanging)
{
    Check check;
    PluginScanner::setExecutableForTesting (scanner);
    ignoreXErrors();     // the scan window and the message box about an out-of-date scanner

    const auto formatOf = [] (const juce::File& f)
    {
        return f.hasFileExtension (".clap") ? juce::String ("CLAP")
             : f.hasFileExtension (".vst3") ? juce::String ("VST3") : juce::String ("VST");
    };

    // Load plugin file... answers on the message thread once its check is done.
    const auto waitUntil = [] (const bool& flag, int ms)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + ms;

        while (! flag && juce::Time::getMillisecondCounterHiRes() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    };

    std::printf ("The scanner looks inside a plugin file\n");
    {
        juce::OwnedArray<juce::PluginDescription> found;
        const auto outcome = PluginScanner::findTypes (formatOf (synthFile), synthFile.getFullPathName(), found, 60000);
        check (outcome == PluginScanner::Outcome::found, "it reports what it found");
        check (found.size() == 1 && found[0]->name == "MPE Roll Test Synth", "which is the test synth");
    }

    std::printf ("A plugin that crashes or hangs only takes the scanner down\n");
    {
        juce::OwnedArray<juce::PluginDescription> found;
        const auto crashStart = juce::Time::getMillisecondCounterHiRes();
        check (PluginScanner::findTypes ("CLAP", crashing.getFullPathName(), found, 60000) == PluginScanner::Outcome::crashed,
               "a crash is reported as one");
        check (juce::Time::getMillisecondCounterHiRes() - crashStart < 5000, "at once");

        const auto start = juce::Time::getMillisecondCounterHiRes();
        check (PluginScanner::findTypes ("CLAP", hanging.getFullPathName(), found, 3000) == PluginScanner::Outcome::timedOut,
               "a hang is reported as one");
        check (juce::Time::getMillisecondCounterHiRes() - start < 15000, "once the time allowed is up");
        check (found.isEmpty(), "and neither lists a plugin");
    }

    std::printf ("A scan carries on past them\n");
    {
        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);
       #if MPEROLL_HOST_CLAP
        formats.addFormat (std::make_unique<ClapPluginFormat>());
       #endif

        const auto formatNamed = [&formats] (const juce::String& name) -> juce::AudioPluginFormat*
        {
            for (auto* format : formats.getFormats())
                if (format->getName() == name)
                    return format;

            return nullptr;
        };

        juce::KnownPluginList list;
        list.setCustomScanner (std::make_unique<PluginScanner::OutOfProcessScanner> (3000));
        juce::OwnedArray<juce::PluginDescription> found;
        auto* clap = formatNamed ("CLAP");
        auto* synthFormat = formatNamed (formatOf (synthFile));

        if (clap != nullptr && synthFormat != nullptr)
        {
            list.scanAndAddFile (crashing.getFullPathName(), false, found, *clap);
            list.scanAndAddFile (hanging.getFullPathName(), false, found, *clap);
            list.scanAndAddFile (synthFile.getFullPathName(), false, found, *synthFormat);
        }

        const auto& setAside = list.getBlacklistedFiles();
        check (setAside.contains (crashing.getFullPathName()) && setAside.contains (hanging.getFullPathName()),
               "the crashing and hanging files are set aside");
        check (list.getNumTypes() == 1 && list.getTypes()[0].name == "MPE Roll Test Synth",
               "and the synth after them is listed");
    }

    std::printf ("A file is only blamed for what happened while it was being looked at\n");
    {
        PluginScanner::setExecutableForTesting (juce::File::getSpecialLocation (juce::File::currentExecutableFile));
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto pathOf = [&folder] (const char* name) { return folder.getChildFile (juce::String (name) + ".vst3").getFullPathName(); };
        juce::OwnedArray<juce::PluginDescription> found;

        check (PluginScanner::findTypes ("VST3", pathOf ("crashes on the way out"), found, 10000) == PluginScanner::Outcome::found
                   && found.size() == 1,
               "a plugin that crashes after it has been looked at keeps what was found");
        check (PluginScanner::findTypes ("VST3", pathOf ("crashes"), found, 10000) == PluginScanner::Outcome::crashed,
               "one that crashes while it's looked at is reported");
        check (PluginScanner::findTypes ("VST3", pathOf ("never starts"), found, 10000) == PluginScanner::Outcome::unavailable,
               "a scanner that goes down before it gets to the file says nothing about the file");
        check (PluginScanner::findTypes ("VST3", pathOf ("too old"), found, 10000) == PluginScanner::Outcome::outdated,
               "one from another version of MPE Roll, which turns the job down, is reported as out of date");

        juce::KnownPluginList list;
        list.setCustomScanner (std::make_unique<PluginScanner::OutOfProcessScanner> (10000));
        juce::VST3PluginFormat format;      // only its name reaches the stand-in scanner
        list.scanAndAddFile (pathOf ("never starts"), false, found, format);
        check (list.getBlacklistedFiles().isEmpty(), "so a scan doesn't set that file aside");
        list.scanAndAddFile (pathOf ("too old"), false, found, format);
        check (list.getBlacklistedFiles().isEmpty(), "nor the file an out-of-date scanner couldn't look at");

        // One of FL's own plugins isn't looked at at all, and a box at the end of the scan says what it
        // is. A scan of its own, so nothing else from the scans above ends up in that box.
        juce::KnownPluginList endList;
        endList.setCustomScanner (std::make_unique<PluginScanner::OutOfProcessScanner> (10000));
        const auto fruity = folder.getChildFile ("Image-Line/FL Studio 2025/Plugins/Fruity/Generators/Harmor/Harmor_x64.dll");
        fruity.create();
        found.clear();
        endList.scanAndAddFile (fruity.getFullPathName(), false, found, format);
        check (found.isEmpty() && endList.getBlacklistedFiles().isEmpty() && endList.getNumTypes() == 0,
               "one of FL's own plugins is left out of a scan without being set aside");

        endList.scanAndAddFile (pathOf ("won't start"), false, found, format);
        check (found.isEmpty() && endList.getBlacklistedFiles().isEmpty() && endList.getNumTypes() == 0,
               "a VST3 that won't start is left out without being set aside, so it's scanned again next time");

        endList.scanFinished();
        const auto endBox = []() -> juce::AlertWindow*
        {
            auto& desktop = juce::Desktop::getInstance();

            for (int i = 0; i < desktop.getNumComponents(); ++i)
                if (auto* box = dynamic_cast<juce::AlertWindow*> (desktop.getComponent (i)))
                    if (box->getName() == "Some plugins weren't added")
                        return box;

            return nullptr;
        };

        for (int i = 0; i < 20 && endBox() == nullptr; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        const auto said = PluginScanner::describeFlStudioNativeFiles ({ fruity.getFileName() });
        check (endBox() != nullptr && said.contains ("Harmor_x64.dll") && said.contains ("Send to FL piano roll"),
               "and the scan ends with a box naming FL's plugin and where to find it instead");

        const auto refused = PluginScanner::describeRefusedToStart (juce::StringArray ("won't start.vst3"), true);
        check (refused.contains ("won't start.vst3") && refused.contains ("authorised") && refused.contains ("Roland Cloud Manager"),
               "and the plugin that didn't start, with what to check");

        if (auto* box = endBox())
            box->exitModalState (0);

        folder.getChildFile ("Image-Line").deleteRecursively();

        // Load plugin file... on it says the same.
        const juce::File wontStart (pathOf ("won't start"));
        wontStart.create();
        {
            HostedSynth synth;
            juce::String error;
            bool answered = false;
            synth.loadFromFile (wontStart, [&] (const juce::String& e) { error = e; answered = true; });
            waitUntil (answered, 30000);
            check (answered && error.contains ("didn't start") && error.contains ("authorised") && ! synth.isLoaded(),
                   "Load plugin file... on a plugin that won't start says it may not be authorised");
        }
        wontStart.deleteFile();

        // Load plugin file... only tries files that exist. Empty, so looking inside it here would find nothing.
        const juce::File tooOld (pathOf ("too old"));
        tooOld.create();
        {
            HostedSynth synth;
            juce::String error;
            bool answered = false;
            synth.loadFromFile (tooOld, [&] (const juce::String& e) { error = e; answered = true; });
            waitUntil (answered, 30000);
            check (answered && error.contains ("older than this MPE Roll") && ! synth.isLoaded(),
                   "Load plugin file... says the scanner is out of date, rather than looking inside the file here");
        }
        tooOld.deleteFile();
        PluginScanner::setExecutableForTesting (scanner);
    }

    std::printf ("Load plugin file... checks the file off the message thread, and refuses one that crashes\n");
    {
        HostedSynth synth;
        juce::String error = "(no answer)";
        bool answered = false, checked = false;
        synth.loadFromFile (crashing, [&] (const juce::String& e) { error = e; answered = true; }, [&checked] { checked = true; });
        check (! answered && synth.isLoading(), "the check runs on a thread of its own, so FL's window isn't held up");
        waitUntil (answered, 30000);
        check (error.contains ("crashed"), "the user is told it crashed");
        check (! synth.isLoaded() && ! synth.isLoading() && ! checked, "and nothing is loaded, or said to be loading");
    }

    std::printf ("A check that's still running stops when the synth goes, or another load starts\n");
    {
        bool answered = false;
        const auto start = juce::Time::getMillisecondCounterHiRes();

        {
            HostedSynth synth;
            synth.loadFromFile (hanging, [&answered] (const juce::String&) { answered = true; });
        }

        check (juce::Time::getMillisecondCounterHiRes() - start < 5000.0, "the synth is gone within moments of a check that's stuck in a plugin");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (! answered, "and the check doesn't answer afterwards");

        HostedSynth synth;
        bool first = false, second = false, checkedBeforeLoading = false;
        juce::String error;
        synth.loadFromFile (hanging, [&first] (const juce::String&) { first = true; });
        synth.loadFromFile (synthFile, [&] (const juce::String& e) { error = e; second = true; },
                            [&] { checkedBeforeLoading = ! second && ! synth.isLoaded(); });
        waitUntil (second, 30000);
        check (second && error.isEmpty() && synth.isLoaded(), "a second Load plugin file... loads its synth");
        check (checkedBeforeLoading, "saying when the check is over and the loading starts (the status goes from Checking to Loading)");
        check (! first, "and the first never answers");

        // A project giving the loaded synth its state (FL does on opening a project, or when the plugin
        // is bridged) while a check is stuck in a plugin: the check and its scanner stop then too, as
        // they do when another load starts. The scanner's job folder is there for as long as it runs.
        const auto scanFolders = []
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .findChildFiles (juce::File::findDirectories, false, "MPE Roll scan *").size();
        };

        const auto foldersBefore = scanFolders();
        bool third = false;
        synth.loadFromFile (hanging, [&third] (const juce::String&) { third = true; });
        const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;

        while (scanFolders() == foldersBefore && juce::Time::getMillisecondCounterHiRes() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        check (scanFolders() == foldersBefore + 1 && synth.isLoading(), "a check stuck in a plugin has its scanner running");

        if (auto saved = synth.saveState())
            synth.restoreState (*saved);

        // The folder goes with the check; a moment's grace for the file system to let it go.
        const auto stopEnd = juce::Time::getMillisecondCounterHiRes() + 2000.0;

        while (scanFolders() != foldersBefore && juce::Time::getMillisecondCounterHiRes() < stopEnd)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        check (scanFolders() == foldersBefore && ! synth.isLoading(), "a project giving the loaded synth its state stops the check, scanner and all");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (! third && synth.isLoaded(), "the check never answers, and the synth stays");
    }

    std::printf ("A scanner ends by itself when MPE Roll has gone, or its time is long up\n");
    {
        // Started by hand, as MPE Roll would, then left alone: how long until it ends, or -1 if it didn't.
        const auto runAlone = [&] (juce::int64 parentProcess, int timeoutMs) -> double
        {
            const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("MPE Roll scan watchdog " + juce::Uuid().toDashedString());
            folder.createDirectory();
            const auto jobFile = folder.getChildFile ("job.txt");

            PluginScanner::Job job;
            job.formatName = "CLAP";
            job.fileOrIdentifier = hanging.getFullPathName();
            job.parentProcessId = parentProcess;
            job.timeoutMs = timeoutMs;
            jobFile.replaceWithText (job.toText());

            juce::ChildProcess process;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            double took = -1.0;

            if (process.start (juce::StringArray { scanner.getFullPathName(), PluginScanner::scanArgument, jobFile.getFullPathName() }, 0))
            {
                if (process.waitForProcessToFinish (20000))
                    took = juce::Time::getMillisecondCounterHiRes() - start;
                else
                    process.kill();
            }

            folder.deleteRecursively();
            return took;
        };

        const auto noParent = runAlone (0x7fffffff, 60000);     // no process has this ID
        check (noParent >= 0.0 && noParent < 10000.0, "at once when the process that started it isn't there");

        const auto stuck = runAlone (PluginScanner::currentProcessId(), 1000);
        check (stuck >= 1000.0 && stuck < 15000.0, "and once the job's time and a margin are up while it's stuck in a plugin");
    }

    std::printf ("Files dropped on the scan window are scanned on the scan's thread\n");
    {
        HostedSynth synth;
        auto& known = synth.getKnownPlugins();

        for (const auto& type : known.getTypes())       // from an earlier run, maybe
            if (type.name == "MPE Roll Test Synth")
                known.removeType (type);

        synth.showScanWindow();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

        juce::PluginListComponent* list = nullptr;
        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* window = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)))
                if (window->getName() == "MPE Roll: scan for plugins")
                    list = dynamic_cast<juce::PluginListComponent*> (window->getContentComponent());

        check (list != nullptr, "the scan window shows the plugin list");

        if (list != nullptr)
        {
            juce::FileDragAndDropTarget& target = *list;
            target.filesDropped (juce::StringArray { synthFile.getFullPathName() }, 0, 0);
            check (list->isScanning(), "a drop starts a scan and comes back at once");

            const auto end = juce::Time::getMillisecondCounterHiRes() + 30000.0;

            while (list->isScanning() && juce::Time::getMillisecondCounterHiRes() < end)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

            bool listed = false;

            for (const auto& type : known.getTypes())
                listed = listed || type.name == "MPE Roll Test Synth";

            check (! list->isScanning() && listed, "and the synth is listed once the scan is done");
        }
    }

    PluginScanner::setExecutableForTesting ({});
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

// A load that fails says why: the system's reason for a file it can't load, and otherwise the step
// that failed when the scanner retraced the load (LoadDiagnosis).
int loadFailTest (const juce::File& scanner, const juce::File& vst3Synth, const juce::File& vst2Synth, const juce::File& flakySynth)
{
    Check check;
    PluginScanner::setExecutableForTesting (scanner);
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MPE Roll load failures");
    folder.deleteRecursively();
    folder.createDirectory();
    PluginLog::setFileForTesting (folder.getChildFile ("Plugin log.txt"));

    // The settings every synth here shares (where loading markers go), in a file of the test's own:
    // kept open by this one for as long as the test runs.
    HostedSynth keeper;
    keeper.useSettingsFile (folder.getChildFile ("MPE Roll.settings"));

    const auto waitFor = [] (bool& answered)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + 120000.0;

        while (! answered && juce::Time::getMillisecondCounterHiRes() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        return answered;
    };

    const auto load = [&check, &waitFor] (const juce::PluginDescription& description)
    {
        HostedSynth synth;
        juce::String error;
        bool answered = false;
        synth.load (description, [&] (const juce::String& e) { error = e; answered = true; });
        waitFor (answered);

        check (answered && ! synth.isLoaded() && ! synth.isLoading() && ! synth.isFindingOutWhy(),
               "  (" + description.name + " didn't load, and the answer came)");
        std::printf ("    %s\n", error.replace ("\n", "\n    ").toRawUTF8());
        return error;
    };

    /** Loads into a synth that stays, and answers with the error (or "no answer"). */
    const auto loadInto = [&waitFor] (HostedSynth& synth, const juce::PluginDescription& description)
    {
        juce::String error;
        bool answered = false;
        synth.load (description, [&] (const juce::String& e) { error = e; answered = true; });
        return waitFor (answered) ? error : juce::String ("no answer");
    };

    const auto describeOnly = [] (const juce::File& file)
    {
        juce::OwnedArray<juce::PluginDescription> found;

        if (file.hasFileExtension (".vst3") && file.exists())
        {
            juce::VST3PluginFormat vst3;
            vst3.findAllTypesForFile (found, file.getFullPathName());
        }
       #if JUCE_PLUGINHOST_VST
        else if (file.existsAsFile())
        {
            juce::VSTPluginFormat vst2;
            vst2.findAllTypesForFile (found, file.getFullPathName());
        }
       #endif

        return found.isEmpty() ? juce::PluginDescription() : *found[0];
    };

    std::printf ("A file the system can't load: its reason\n");
    {
        juce::PluginDescription d;
        d.pluginFormatName = "VST3";
        d.name = "Broken Synth";
        d.numOutputChannels = 2;
        const auto bundle = folder.getChildFile ("Broken Synth.vst3");

       #if JUCE_WINDOWS
        d.fileOrIdentifier = bundle.getFullPathName();
        bundle.replaceWithText ("not a plugin");
       #else
        // On Linux a VST3 is always a bundle, with the library inside.
        bundle.createDirectory();
        d.fileOrIdentifier = bundle.getFullPathName();
        const auto inside = LoadDiagnosis::findBinaries (d)[0];
        inside.getParentDirectory().createDirectory();
        inside.replaceWithText ("not a plugin");
       #endif

        const auto error = load (d);
       #if JUCE_WINDOWS
        check (error.contains ("Windows couldn't load Broken Synth.vst3") && error.contains ("isn't a 64-bit Windows plugin")
                   && error.contains ("Windows error 193"),
               "Windows' own reason, in words, with its error number");
        check (! error.contains ("%1"), "the file named where Windows' own message has a placeholder");
       #else
        check (error.contains ("Broken Synth.so couldn't be loaded:"), "the system's own reason");
       #endif
    }

    const auto realVst3 = describeOnly (vst3Synth);
    check (realVst3.name == "MPE Roll Test Synth", "the test synth's VST3 is found");

   #if JUCE_PLUGINHOST_VST
    std::printf ("A library that isn't a plugin, loaded as VST2: it says so\n");
    if (realVst3.name.isNotEmpty())
    {
        // The VST3 test synth's own library has no VST2 entry point.
       #if JUCE_WINDOWS
        const auto library = folder.getChildFile ("Not A Synth.dll");
       #else
        const auto library = folder.getChildFile ("Not A Synth.so");
       #endif
        LoadDiagnosis::findBinaries (realVst3)[0].copyFileTo (library);

        juce::PluginDescription d;
        d.pluginFormatName = "VST";
        d.name = "Not A Synth";
        d.fileOrIdentifier = library.getFullPathName();
        const auto error = load (d);
        check (error.contains ("has no VSTPluginMain or main function, so it isn't a VST2 plugin"), "no VST2 entry point");
    }
   #endif

    std::printf ("A VST3 that no longer holds the class a scan listed: the scanner finds which\n");
    if (realVst3.name.isNotEmpty())
    {
        auto changed = realVst3;
        changed.uniqueId ^= 0x5a5a;
        changed.deprecatedUid ^= 0x5a5a;
        const auto error = load (changed);
        check (error.contains ("\"MPE Roll Test Synth\" has another ID"), "the class with its name has another ID");
        check (error.contains ("Load plugin file...") || error.contains ("moduleinfo.json"), "and what to do");

        const auto log = PluginLog::getFile().loadFileAsString();
        check (log.contains ("Retracing the load in the scanner") && log.contains ("Calling its GetPluginFactory")
                   && log.contains ("No class is \"MPE Roll Test Synth\""),
               "the plugin log has each step the scanner took");
        std::printf ("    The plugin log so far:\n%s\n", ("      " + log.trimEnd().replace ("\n", "\n      ")).toRawUTF8());

        auto renamed = realVst3;
        renamed.name = "MPE Roll Old Synth";
        check (load (renamed).contains ("\"MPE Roll Test Synth\" has the ID, under another name"), "and a renamed class too");

        std::printf ("Load plugin file... on it, as the box says: the listing that no longer loads is replaced\n");
        HostedSynth synth;
        auto& known = synth.getKnownPlugins();
        known.addType (changed);

        bool answered = false;
        juce::String loadError;
        synth.loadFromFile (juce::File (realVst3.fileOrIdentifier), [&] (const juce::String& e) { loadError = e; answered = true; });
        const auto end = juce::Time::getMillisecondCounterHiRes() + 120000.0;

        while (! answered && juce::Time::getMillisecondCounterHiRes() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        juce::Array<juce::PluginDescription> listed;

        for (const auto& type : known.getTypes())
            if (type.pluginFormatName == "VST3" && type.fileOrIdentifier == realVst3.fileOrIdentifier)
                listed.add (type);

        check (answered && loadError.isEmpty() && synth.isLoaded(), "it loads");
        check (listed.size() == 1 && listed[0].isDuplicateOf (realVst3), "and the file is listed once, as it is now");
        synth.unload();
    }

    std::printf ("Plugins that load fine, retraced anyway: the plugin itself works\n");
    for (const auto& synthFile : { vst3Synth, vst2Synth })
    {
        const auto d = describeOnly (synthFile);

        if (d.name.isEmpty())
            continue;

        const auto diagnosis = PluginScanner::diagnose (d, 60000);
        const auto why = LoadDiagnosis::summarise (diagnosis.lines, false, false, d.name);
        check (diagnosis.outcome == PluginScanner::Outcome::found && why.contains ("loads fine in a separate process"),
               d.pluginFormatName + ": " + why.upToFirstOccurrenceOf (",", false, false));
    }

    std::printf ("A scanner from before diagnoses turns the job down\n");
    {
        // The harness stands in for a scanner that only knows scan jobs.
        PluginScanner::setExecutableForTesting (juce::File::getSpecialLocation (juce::File::currentExecutableFile));
        check (PluginScanner::diagnose (realVst3, 20000).outcome == PluginScanner::Outcome::outdated, "and is reported as out of date");

        if (realVst3.name.isNotEmpty())
        {
            auto changed = realVst3;
            changed.uniqueId ^= 0x5a5a;
            changed.deprecatedUid ^= 0x5a5a;
            const auto error = load (changed);
            check (error.contains ("couldn't find out why MPE Roll Test Synth didn't load") && error.contains ("older than this MPE Roll"),
                   "a failed load says the scanner couldn't find out why, and that it's out of date");
        }
    }

    PluginScanner::setExecutableForTesting (scanner);

   #if JUCE_PLUGINHOST_VST
    std::printf ("A plugin that won't start the first time in a process is loaded again once the scanner finds it works\n");
    if (flakySynth.existsAsFile())
    {
        // Described by hand: asking the file for its plugins would be its first start in this process.
        juce::PluginDescription d;
        d.pluginFormatName = "VST";
        d.name = "MPE Roll Flaky Test Synth";
        d.manufacturerName = "Glowing Pancake";
        d.fileOrIdentifier = flakySynth.getFullPathName();
        d.uniqueId = d.deprecatedUid = 0x466c6b79;    // 'Flky'
        d.numOutputChannels = 2;
        d.isInstrument = true;

        HostedSynth synth;
        const auto error = loadInto (synth, d);
        std::printf ("    %s\n", error.isEmpty() ? "loaded" : error.replace ("\n", "\n    ").toRawUTF8());
        check (error.isEmpty() && synth.isLoaded(), "it loads, on the second try");
        const auto log = PluginLog::tail (40);
        check (log.contains ("started in a separate process when MPE Roll called it directly"), "the scanner having started it directly");
        const bool retried = log.contains ("Loading it again, since it works in a separate process")
                          && log.contains ("Loading again MPE Roll Flaky Test Synth");
        check (retried, "and MPE Roll loaded it again by itself");

        if (! retried)
            std::printf ("%s\n", log.replace ("\n", "\n      ").toRawUTF8());

        synth.unload();
    }
    else
    {
        std::printf ("  (no flaky test synth given: not checked)\n");
    }
   #endif

    std::printf ("Another install of the loaded synth: the first is let go before the second loads\n");
    if (realVst3.name.isNotEmpty())
    {
        // A copy of the VST3 test synth in another folder, as a second install is.
        const juce::File bundle (realVst3.fileOrIdentifier);
        const auto secondFolder = folder.getChildFile ("second install");
        secondFolder.createDirectory();
        const auto copy = secondFolder.getChildFile (bundle.getFileName());
        const auto copied = bundle.isDirectory() ? bundle.copyDirectoryTo (copy) : bundle.copyFileTo (copy);
        const auto second = describeOnly (copy);
        check (copied && second.name == realVst3.name && second.fileOrIdentifier == copy.getFullPathName(),
               "a copy of the synth in another folder is found there");

        HostedSynth synth;
        check (loadInto (synth, realVst3).isEmpty() && synth.isLoaded(), "the first install loads");
        check (loadInto (synth, second).isEmpty() && synth.isLoaded() && synth.getDescription() != nullptr
                   && synth.getDescription()->fileOrIdentifier == copy.getFullPathName(),
               "and the second loads in its place");
        check (PluginLog::tail (30).contains ("first: another install of it is about to be loaded"), "the first let go before it");
        synth.unload();
    }

    std::printf ("A synth that took the program down while loading isn't loaded again by a project on its own\n");
    if (realVst3.name.isNotEmpty())
    {
        auto* settings = keeper.getUserSettings();
        check (settings != nullptr, "the test has settings of its own");

        const auto processId = CopySafety::getProcessId();
        const auto started = CopySafety::getProcessStartTime (processId);
        const auto identity = realVst3.name + "|" + realVst3.pluginFormatName + ":"
                            + juce::String (realVst3.uniqueId != 0 ? realVst3.uniqueId : realVst3.deprecatedUid) + "|" + realVst3.fileOrIdentifier;
        const auto markers = [settings]
        {
            int count = 0;

            for (const auto& key : settings->getAllProperties().getAllKeys())
                count += key.startsWith ("synthLoading.") ? 1 : 0;

            return count;
        };

        // The project names the synth.
        juce::XmlElement project ("HostedSynth");
        project.addChildElement (realVst3.createXml().release());

        const auto restoreInto = [&waitFor, &project] (HostedSynth& synth)
        {
            juce::String error;
            bool answered = false;
            synth.restoreState (project, [&] (const juce::String& e) { error = e; answered = true; });
            return waitFor (answered) ? error : juce::String ("no answer");
        };

        // What a run that closed while loading it leaves behind: its process ID has since gone to
        // another process (this one), which started later, so it isn't the one.
        settings->setValue ("synthLoading.earlier", "another run|" + juce::String (processId) + "@" + juce::String (started - 1) + "|" + identity);

        HostedSynth synth;
        auto error = restoreInto (synth);
        std::printf ("    %s\n", error.toRawUTF8());
        check (! synth.isLoaded() && error.contains ("closed while it was loading last time") && synth.getLoadProblem() == error,
               "the project opens without it, and says why");
        check (synth.getMissingSynthName() == realVst3.name, "the synth button can name it");
        check (! settings->containsKey ("synthLoading.earlier") && settings->getValue ("synthCrashedWhileLoading").contains (identity),
               "the marker is gone, and the crash remembered");

        // Picked by name, it loads, and the crash is forgotten.
        error = loadInto (synth, realVst3);
        check (error.isEmpty() && synth.isLoaded() && synth.getLoadProblem().isEmpty(), "Load synth loads it");
        check (! settings->getValue ("synthCrashedWhileLoading").contains (identity) && markers() == 0,
               "the crash is forgotten, and no marker is left once it has loaded");

        // Another program loading it right now (the MPE Roll app beside FL, say) isn't a crash.
        settings->setValue ("synthLoading.elsewhere", "another program|" + juce::String (processId) + "@" + juce::String (started) + "|" + identity);
        synth.unload();
        check (restoreInto (synth).isEmpty() && synth.isLoaded(), "a running program's marker doesn't keep a project from loading it");
        check (settings->containsKey ("synthLoading.elsewhere"), "and is left for it");
        settings->removeValue ("synthLoading.elsewhere");
        synth.unload();
    }

    PluginScanner::setExecutableForTesting ({});
    PluginLog::setFileForTesting ({});
    folder.deleteRecursively();
    std::printf ("%s: %d failure(s)\n", check.failures == 0 ? "PASS" : "FAIL", check.failures);
    return check.failures == 0 ? 0 : 1;
}

#if JUCE_WINDOWS
 #if JUCE_MSVC
__declspec (noinline)
 #endif
void crashOnPurpose (volatile int* target)
{
    *target = 42;
}

volatile bool keepRecursing = true;     // always true, but the compiler can't know that

 #if JUCE_MSVC
__declspec (noinline)
 #endif
int overflowOnPurpose (int depth)
{
    volatile char frame[4096];
    frame[0] = (char) depth;

    if (! keepRecursing)
        return 0;

    return overflowOnPurpose (depth + 1) + frame[0];
}
#endif

// Windows: the standalone app's crash report, checked by crashing on purpose. Never returns there.
int crashTest (const juce::File& reportFile, bool overflow)
{
   #if JUCE_WINDOWS
    static volatile int* volatile nowhere = nullptr;
    CrashReport::install (reportFile, false);
    CrashReport::setActivity ("running the crash test");

    if (overflow)
        overflowOnPurpose (0);
    else
        crashOnPurpose (nowhere);

    std::printf ("FAIL: it didn't crash\n");
    return 1;
   #else
    juce::ignoreUnused (reportFile, overflow);
    std::printf ("Crash reports are only made on Windows.\n");
    return 0;
   #endif
}

} // namespace

//==============================================================================
/** One copy per note, through the plain test synth: a synth without MPE, where every note on a
    channel shares its pitch bend. Copies give each note its own; the copies follow the first one's
    parameters and sound, sleep when quiet, play the same on several cores as on one, survive a
    project being saved and opened, and stop loading when memory runs low or when loading them
    crashed last time.
*/
struct CopyRig
{
    static constexpr double rate = 48000.0;
    static constexpr int block = 512;

    CopyRig()
    {
        processor.setPlayHead (&playHead);
        processor.setRateAndBufferSizeDetails (rate, block);
        processor.prepareToPlay (rate, block);
    }

    ~CopyRig()
    {
        processor.releaseResources();
    }

    HostedSynth& synth()                                { return processor.getSynth(); }
    HostedSynth::CopyStatus status()                    { return synth().getCopyStatus(); }

    static void pump (int ms)                           { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    void setOutput (const std::function<void (OutputSettings&)>& change)
    {
        auto s = processor.getDocument().getSettings();
        change (s.output);
        processor.getDocument().setSettings (s);
        pump (30);      // the document's change message reaches the processor, which tells the synth
    }

    /** Blocks with the transport stopped, in about real time: the message loop runs between them
        for as long as a block lasts, so timers (the copies' checks for a changed sound) fire as
        they would. */
    void idle (int milliseconds)
    {
        playHead.playing = false;
        const auto blockMs = juce::jmax (1, (int) (block / rate * 1000.0));
        const auto blocks = juce::jmax (1, milliseconds / blockMs);

        for (int i = 0; i < blocks; ++i)
        {
            juce::AudioBuffer<float> audio (2, block);
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi);
            pump (blockMs);
        }
    }

    /** Waits, playing silence, until the copies asked for are loaded (or loading has stopped
        short) and none is taking a sound. */
    bool settle (int timeoutMs = 30000)
    {
        auto loaded = std::make_shared<bool> (false);
        synth().whenCopiesLoaded ([loaded] { *loaded = true; });

        for (int waited = 0; waited < timeoutMs; waited += 50)
        {
            idle (50);

            if (*loaded && ! status().copyingSound)
            {
                idle (50);
                return true;
            }
        }

        return false;
    }

    /** Plays the clip in slot 1 from its start for this long and returns what came out. Given
        `during`, it's called before each block with the time, and the blocks come in about real
        time, the message loop running between them (timers fire as they would). */
    juce::AudioBuffer<float> play (const MpeClip& clip, double seconds, const std::function<void (double)>& during = {})
    {
        processor.getDocument().replaceClip (0, clip, "Test");
        pump (10);

        const auto total = (int) (seconds * rate);
        const auto blockMs = juce::jmax (1, (int) (block / rate * 1000.0));
        juce::AudioBuffer<float> output (2, total);
        output.clear();
        playHead.playing = true;

        for (int pos = 0; pos < total; pos += block)
        {
            if (during)
                during (pos / rate);

            const auto n = juce::jmin (block, total - pos);
            juce::AudioBuffer<float> audio (2, n);
            juce::MidiBuffer midi;
            playHead.ppq = pos / rate * playHead.bpm / 60.0;
            processor.processBlock (audio, midi);

            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom (ch, pos, audio, ch, 0, n);

            if (during)
                pump (blockMs);
        }

        playHead.playing = false;
        idle (20);
        return output;
    }

    double level (const juce::AudioBuffer<float>& audio, double atSeconds, double frequency) const
    {
        constexpr int window = 4096;
        const auto centre = (int) (atSeconds * rate);
        return goertzel (audio.getReadPointer (0, centre - window / 2), window, frequency, rate);
    }

    juce::MemoryBlock stateOf (int copy)
    {
        juce::MemoryBlock state;

        if (auto* plugin = synth().getCopy (copy))
            plugin->getStateInformation (state);

        return state;
    }

    /** True if every loaded copy has the first copy's state. */
    bool copiesMatchFirst()
    {
        const auto first = stateOf (0);

        for (int c = 1; c < OutputSettings::maxCopies; ++c)
        {
            if (synth().getCopy (c) == nullptr)
                continue;

            if (const auto state = stateOf (c); state != first)
            {
                size_t at = 0;

                while (at < juce::jmin (state.getSize(), first.getSize()) && state[at] == first[at])
                    ++at;

                std::printf ("  copy %d's state (%d bytes) differs from the first's (%d bytes) at byte %d\n", c + 1,
                             (int) state.getSize(), (int) first.getSize(), (int) at);
                std::printf ("%s\n", PluginLog::tail (6).toRawUTF8());
                return false;
            }
        }

        return true;
    }

    FakePlayHead playHead;
    MpeRollProcessor processor;
};

/** C4 glides up an octave between beats 0.5 and 1.5 while E4 holds (as in the render test). */
MpeClip glideClip()
{
    MpeClip clip;
    clip.length = 8.0;
    auto glide = note (60, 0.0, 2.0, 0.8f);
    glide.lanes.pitch = { { 0.0, 0.0f }, { 0.5, 0.0f }, { 1.5, 12.0f } };
    clip.addNote (glide);
    clip.addNote (note (64, 0.0, 2.0, 0.8f));
    return clip;
}

int copiesTest (const juce::File& synthFile)
{
    Check check;
    ignoreXErrors();     // the synth's window, opened and closed below

    // The settings every MPE Roll shares (where the loading markers go) in a file of the test's own.
    const auto settingsFile = juce::File::createTempFile ("settings");

    // Where the test synth notes any call made to it after it crashed (before it's loaded).
    const auto afterCrash = juce::File::createTempFile ("after-crash");
    afterCrash.deleteFile();
   #if JUCE_WINDOWS
    _putenv_s ("MPEROLL_TEST_AFTER_CRASH", afterCrash.getFullPathName().toRawUTF8());
   #else
    setenv ("MPEROLL_TEST_AFTER_CRASH", afterCrash.getFullPathName().toRawUTF8(), 1);
   #endif

    auto rig = std::make_unique<CopyRig>();
    rig->synth().useSettingsFile (settingsFile);

    rig->setOutput ([] (OutputSettings& o)
    {
        o.mode = OutputSettings::Mode::copies;
        o.copies = 4;
        o.masterBendRange = 12.0f;      // the copies are told it (RPN 0), and the synth follows
    });

    if (! loadSynth (rig->processor, synthFile))
        return 1;

    std::printf ("Hosting: %s\n", rig->synth().getName().toRawUTF8());
    check (rig->settle() && rig->status().loaded == 4, "four copies load, one at a time");

    // The synth's own bend range set to MPE Roll's, as its user would: a VST3 built with JUCE gets
    // MIDI controllers as parameter changes, in which an RPN's messages lose their order, so the
    // range MPE Roll sends (RPN 0) only reaches the VST2. Through FL automation, so every copy has it.
    for (auto* p : rig->processor.getParameters())
        if (p->getName (100) == "Bend range")
            p->setValue (p->getValueForText ("12"));

    rig->idle (100);
    std::printf ("  %s\n", rig->synth().describeConnection (rig->processor.getDocument().getSettings().output).toRawUTF8());

    const auto c4 = juce::MidiMessage::getMidiNoteInHertz (60);
    const auto e4 = juce::MidiMessage::getMidiNoteInHertz (64);
    const auto c5 = juce::MidiMessage::getMidiNoteInHertz (72);

    std::printf ("Each note glides on its own copy\n");
    juce::AudioBuffer<float> withCores;
    {
        withCores = rig->play (glideClip(), 1.0);
        const auto earlyC4 = rig->level (withCores, 0.125, c4), earlyE4 = rig->level (withCores, 0.125, e4);
        const auto lateC5 = rig->level (withCores, 0.925, c5), lateC4 = rig->level (withCores, 0.925, c4);
        const auto lateE4 = rig->level (withCores, 0.925, e4);
        std::printf ("  start: C4 %.4f E4 %.4f   end: C5 %.4f C4 %.4f E4 %.4f\n", earlyC4, earlyE4, lateC5, lateC4, lateE4);
        check (earlyC4 > 0.01 && earlyE4 > 0.01, "both notes start in tune");
        check (lateC5 > 10.0 * lateC4, "C4 glides up the octave (the copy took the 12-semitone bend range)");
        check (lateE4 > 0.5 * earlyE4, "E4 holds still while it does");
    }

    std::printf ("A glide past the Zone bend range is reported\n");
    {
        check (rig->processor.getRecentBendOverflow().isEmpty(), "the octave glide fitted the 12-semitone range");

        // The stream carries each note's bend scaled to the copies' range, so the octave stops at 2.
        rig->setOutput ([] (OutputSettings& o) { o.masterBendRange = 2.0f; });
        rig->play (glideClip(), 1.0);
        const auto over = rig->processor.getRecentBendOverflow();
        std::printf ("  reached %g semitones, stopped at %g\n", (double) over.asked, (double) over.range);
        check (std::abs (over.asked - 12.0f) < 0.01f && std::abs (over.range - 2.0f) < 0.01f,
               "the octave glide stopped at 2 semitones, and the processor says so");
        check (over.semitonesReached() == 12 && over.suggestedRange() == 12, "with 12 the range to set");

        const auto text = rig->synth().describeConnection (rig->processor.getDocument().getSettings().output, over);
        std::printf ("  %s\n", text.toRawUTF8());
        check (text.contains ("A glide just reached 12 semitones and stopped at 2: set both to 12."), "and MPE settings says what to set");

        rig->setOutput ([] (OutputSettings& o) { o.masterBendRange = 12.0f; });
        check (rig->processor.getRecentBendOverflow().isEmpty(), "a changed Zone bend range drops the report");
        rig->play (glideClip(), 1.0);
        check (rig->processor.getRecentBendOverflow().isEmpty(), "and the octave fits again");
    }

    std::printf ("The same on one core as on several\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.copiesUseCores = false; });
        check (rig->status().workers == 0, "no worker threads with Use several CPU cores off");
        rig->processor.prepareToPlay (CopyRig::rate, CopyRig::block);
        const auto alone = rig->play (glideClip(), 1.0);
        rig->setOutput ([] (OutputSettings& o) { o.copiesUseCores = true; });
        check (rig->status().workers > 0, "worker threads with it on");
        rig->processor.prepareToPlay (CopyRig::rate, CopyRig::block);
        const auto spread = rig->play (glideClip(), 1.0);

        float largest = 0.0f;

        for (int i = 0; i < alone.getNumSamples(); ++i)
            largest = juce::jmax (largest, std::abs (alone.getSample (0, i) - spread.getSample (0, i)));

        std::printf ("  largest difference %g\n", (double) largest);
        check (largest < 1.0e-6f, "the copies sound exactly the same spread over cores");
    }

    std::printf ("Without copies, the synth bends every note together\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::singleChannel; });
        check (rig->status().loaded == 0 && rig->synth().getCopy (1) == nullptr, "Plain MIDI lets the copies go");

        // With Plain MIDI the newest note's glide bends the channel: C4 starts just after E4.
        MpeClip clip;
        clip.length = 8.0;
        clip.addNote (note (64, 0.0, 2.0, 0.8f));
        auto glide = note (60, 0.05, 1.95, 0.8f);
        glide.lanes.pitch = { { 0.0, 0.0f }, { 0.45, 0.0f }, { 1.45, 12.0f } };
        clip.addNote (glide);
        const auto plain = rig->play (clip, 1.0);
        const auto earlyE4 = rig->level (plain, 0.125, e4), lateE4 = rig->level (plain, 0.925, e4);
        std::printf ("  E4 at the start %.4f, at the end %.4f\n", earlyE4, lateE4);
        check (lateE4 < 0.2 * earlyE4, "E4 is carried off by C4's glide (what copies are for)");

        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::copies; });
        check (rig->settle() && rig->status().loaded == 4, "back to one copy per note, four copies load again");
    }

    std::printf ("The copies follow the first one's parameters\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = false; });
        juce::AudioProcessorParameter* gain = nullptr;

        for (auto* p : rig->processor.getParameters())
            if (p->getName (100) == "Gain")
                gain = p;

        check (gain != nullptr, "the synth's Gain is one of MPE Roll's parameters");

        auto copiesHave = [&] (float value)
        {
            for (int c = 1; c < 4; ++c)
                if (auto* copy = rig->synth().getCopy (c); copy == nullptr || std::abs (copy->getParameters()[0]->getValue() - value) > 1.0e-4f)
                    return false;

            return true;
        };

        if (gain != nullptr)
        {
            gain->setValue (0.25f);     // as host automation does
            rig->idle (50);
            check (copiesHave (0.25f), "automating Gain reaches every copy");
        }

        rig->synth().getCopy (0)->getParameters()[0]->setValueNotifyingHost (0.5f);   // as the synth's own window does
        rig->idle (50);
        check (copiesHave (0.5f), "turning Gain in the first copy reaches every copy");

        rig->synth().getCopy (0)->getParameters()[0]->setValueNotifyingHost (1.0f);
        rig->idle (50);

        // A synth's own pitch-wheel parameter (Serum 1's "Pitch Bend") follows the notes the first
        // copy is sent, and the synth may report it from its window as the wheel it draws moves:
        // that's not handed on. Automating it from the host still reaches every copy.
        constexpr int pitchWheel = 5;
        juce::AudioProcessorParameter* pitchWheelSlot = nullptr;

        for (auto* p : rig->processor.getParameters())
            if (p->getName (100) == "Pitch Bend")
                pitchWheelSlot = p;

        auto copiesWheelAt = [&] (float value)
        {
            for (int c = 1; c < 4; ++c)
                if (auto* copy = rig->synth().getCopy (c); copy == nullptr || std::abs (copy->getParameters()[pitchWheel]->getValue() - value) > 1.0e-4f)
                    return false;

            return true;
        };

        // Nor is it told to the host, which could send it back as automation for every copy.
        struct HeardByHost : juce::AudioProcessorParameter::Listener
        {
            void parameterValueChanged (int, float) override        { ++changes; }
            void parameterGestureChanged (int, bool) override       {}
            int changes = 0;
        };

        HeardByHost heard;

        if (pitchWheelSlot != nullptr)
            pitchWheelSlot->addListener (&heard);

        rig->synth().getCopy (0)->getParameters()[pitchWheel]->setValueNotifyingHost (0.9f);
        rig->idle (50);
        check (copiesWheelAt (0.5f), "the first copy's own Pitch Bend parameter moving reaches no other copy");
        check (pitchWheelSlot != nullptr && heard.changes == 0, "nor the host, which could send it back as automation");

        if (pitchWheelSlot != nullptr)
            pitchWheelSlot->removeListener (&heard);

        check (pitchWheelSlot != nullptr, "the synth's Pitch Bend is one of MPE Roll's parameters");

        if (pitchWheelSlot != nullptr)
        {
            pitchWheelSlot->setValue (0.7f);
            rig->idle (50);
            check (copiesWheelAt (0.7f), "automating it from the host reaches every copy");
            pitchWheelSlot->setValue (0.5f);
            rig->idle (50);
        }

        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = true; });
    }

    std::printf ("The copies follow the first one's sound\n");
    {
        // A synth picking presets may report several changes in a row: the copies get the sound
        // once it has settled, 0.4 seconds after the last.
        rig->synth().getCopy (0)->setCurrentProgram (1);     // Square, and the synth says its program changed
        rig->idle (800);
        check (rig->settle() && rig->copiesMatchFirst(), "a program the synth reports reaches every copy");

        rig->synth().getCopy (0)->setCurrentProgram (3);     // Square (quiet): the synth doesn't say
        rig->idle (50);
        rig->synth().showEditor (nullptr);
        rig->idle (100);
        rig->synth().closeEditor();
        rig->idle (300);
        check (rig->settle() && rig->copiesMatchFirst(), "a change it doesn't report reaches them when its window closes");

        rig->synth().getCopy (0)->setCurrentProgram (0);
        rig->idle (800);
        rig->synth().copySoundToAllCopies();
        check (rig->settle() && rig->copiesMatchFirst(), "Give every copy copy 1's sound does");
    }

    std::printf ("Notes held while the copies are given a sound play on\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = false; });

        MpeClip chord;
        chord.length = 8.0;

        for (const auto key : { 60, 64, 67 })
            chord.addNote (note (key, 0.0, 3.0, 0.8f));     // 1.5 seconds at 120 bpm

        // A program the synth reports, partway through: the copies are given it 0.4 s later, those
        // holding the chord once it has ended.
        bool changed = false;
        const auto audio = rig->play (chord, 2.0, [&] (double t)
        {
            if (t >= 0.3 && ! changed)
            {
                changed = true;
                rig->synth().getCopy (0)->setCurrentProgram (2);     // Saw
            }
        });

        auto quietest = 1.0;

        for (const auto key : { 60, 64, 67 })
            quietest = juce::jmin (quietest, rig->level (audio, 1.3, juce::MidiMessage::getMidiNoteInHertz (key)));

        std::printf ("  quietest note of the chord at 1.3 s: %.4f\n", quietest);
        check (quietest > 0.01, "the chord sounds to its end");
        check (rig->settle() && rig->copiesMatchFirst(), "and the copies that held it have the new sound after it");

        // A knob turned with the synth's window open: the copies get it as it moves, and the look
        // taken every 1.5 s while the window is open finds they have the sound already.
        rig->synth().showEditor (nullptr);
        PluginLog::add ("Harness: turning Release");
        const auto turned = rig->play (chord, 3.5, [&] (double t)
        {
            if (t > 0.2 && t < 0.6)
                rig->synth().getCopy (0)->getParameters()[2]->setValueNotifyingHost ((float) t);
        });

        rig->synth().closeEditor();
        rig->idle (300);
        const auto since = PluginLog::tail (40).fromLastOccurrenceOf ("Harness: turning Release", false, false);
        std::printf ("  log since:%s\n", since.replace ("\n", "\n    ").toRawUTF8());
        check (since.contains ("had its sound already") && ! since.contains ("Gave"),
               "the copies, found to have the knob already, aren't given the whole sound again");
        check (rig->settle() && rig->copiesMatchFirst(), "and they match the first copy");
        juce::ignoreUnused (turned);

        rig->synth().getCopy (0)->setCurrentProgram (0);
        rig->synth().getCopy (0)->getParameters()[2]->setValueNotifyingHost (0.0f);
        rig->idle (800);
        check (rig->settle() && rig->copiesMatchFirst(), "back to the first program");
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = true; });
    }

    std::printf ("Notes started while the copies are given a sound play to their end\n");
    {
        // A run of notes a quarter of a second apart, each 0.4 s long, and a new program given to
        // every copy as it starts: each copy is given it in turn, between notes, and the copy about
        // to be given it is given no new note meanwhile.
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = false; });

        MpeClip run;
        run.length = 8.0;

        for (int i = 0; i < 6; ++i)
            run.addNote (note (60 + 2 * i, 0.5 * i, 0.8, 0.8f));

        bool asked = false;
        const auto audio = rig->play (run, 2.2, [&] (double t)
        {
            if (t >= 0.2 && ! asked)
            {
                asked = true;
                rig->synth().getCopy (0)->setCurrentProgram (2);     // Saw
                rig->synth().copySoundToAllCopies();
            }
        });

        auto quietest = 1.0;

        for (int i = 0; i < 6; ++i)
            quietest = juce::jmin (quietest, rig->level (audio, 0.25 * i + 0.3, juce::MidiMessage::getMidiNoteInHertz (60 + 2 * i)));

        std::printf ("  quietest note near its end: %.4f\n", quietest);
        check (quietest > 0.01, "every note sounds to its end");
        check (rig->settle() && rig->copiesMatchFirst(), "and every copy has the new program after");

        rig->synth().getCopy (0)->setCurrentProgram (0);
        rig->synth().copySoundToAllCopies();
        check (rig->settle() && rig->copiesMatchFirst(), "back to the first program");
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = true; });
    }

    std::printf ("Quiet copies sleep, and a note wakes one\n");
    {
        rig->idle (1600);
        auto s = rig->status();
        std::printf ("  after the notes: %d asleep of %d\n", s.asleep, s.loaded);
        check (s.asleep == 4, "a second after the last note, every copy sleeps");

        MpeClip single;
        single.length = 4.0;
        single.addNote (note (69, 0.0, 1.0, 0.8f));
        const auto audio = rig->play (single, 0.4);
        const auto a4 = rig->level (audio, 0.25, juce::MidiMessage::getMidiNoteInHertz (69));
        const auto off = rig->level (audio, 0.25, juce::MidiMessage::getMidiNoteInHertz (69) * 1.06);
        std::printf ("  A4 %.4f, a semitone off %.4f\n", a4, off);
        check (a4 > 0.01 && a4 > 10.0 * off, "a note wakes a copy and plays in tune");
    }

    std::printf ("Keys to every copy\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.everyCopyKeys = parseKeySet ("C3"); o.copiesSleep = false; });   // key 36
        MpeClip one, every;
        one.length = every.length = 4.0;
        one.addNote (note (38, 0.0, 1.0, 0.8f));
        every.addNote (note (36, 0.0, 1.0, 0.8f));
        const auto oneAudio = rig->play (one, 0.4);
        rig->idle (300);
        const auto everyAudio = rig->play (every, 0.4);
        const auto single = rig->level (oneAudio, 0.25, juce::MidiMessage::getMidiNoteInHertz (38));
        const auto all = rig->level (everyAudio, 0.25, juce::MidiMessage::getMidiNoteInHertz (36));
        std::printf ("  a key of its own %.4f, a key to every copy %.4f\n", single, all);
        check (all > 3.5 * single && all < 4.5 * single, "a key in the every-copy set sounds in all four copies");
        rig->setOutput ([] (OutputSettings& o) { o.everyCopyKeys.reset(); o.copiesSleep = true; });
    }

    std::printf ("More copies, fewer copies\n");
    {
        rig->setOutput ([] (OutputSettings& o) { o.copies = 2; });
        check (rig->settle() && rig->status().loaded == 2 && rig->synth().getCopy (2) == nullptr, "down to two copies");
        rig->setOutput ([] (OutputSettings& o) { o.copies = 6; });
        rig->synth().getCopy (0)->setCurrentProgram (1);     // Square, picked while they load
        check (rig->settle() && rig->status().loaded == 6, "up to six copies");
        rig->idle (800);
        check (rig->settle() && rig->copiesMatchFirst(), "the new copies have the first one's sound, picked while they loaded");

        rig->synth().getCopy (0)->setCurrentProgram (0);
        rig->synth().copySoundToAllCopies();
        rig->settle();
    }

    std::printf ("Loading stops when memory runs low\n");
    {
        CopySafety::pretendAvailableMemoryMB (500);
        rig->setOutput ([] (OutputSettings& o) { o.copies = 8; });
        rig->settle();
        auto s = rig->status();
        std::printf ("  %s\n", s.problem.toRawUTF8());
        check (s.loaded == 6 && s.problem.contains ("memory"), "no more copies with 500 MB free, and it says why");

        CopySafety::pretendAvailableMemoryMB (-2);
        rig->synth().reloadCopies();
        check (rig->settle() && rig->status().loaded == 8 && rig->status().problem.isEmpty(), "Load the copies again loads the rest");
    }

    std::printf ("A crash while loading copies isn't repeated\n");
    {
        const auto* d = rig->synth().getDescription();
        const auto identity = d->pluginFormatName + ":" + juce::String (d->uniqueId != 0 ? d->uniqueId : d->deprecatedUid) + ":" + d->name;

        // Another program loading copies right now (the MPE Roll app beside FL, say) isn't a crash.
        const auto processId = CopySafety::getProcessId();
        const auto started = CopySafety::getProcessStartTime (processId);
        check (started != 0, "a process's start time can be told");
        rig->synth().getUserSettings()->setValue ("copiesLoading.elsewhere",
                                                  "another program|" + juce::String (processId) + "@" + juce::String (started) + "|" + identity);
        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::singleChannel; });
        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::copies; o.copies = 4; });
        check (rig->settle() && rig->status().loaded == 4 && rig->status().problem.isEmpty(),
               "a running program's marker doesn't stop copies loading");
        check (rig->synth().getUserSettings()->containsKey ("copiesLoading.elsewhere"), "and is left for it");
        rig->synth().getUserSettings()->removeValue ("copiesLoading.elsewhere");

        // What a run that closed while loading a copy leaves behind. Its process ID has since been
        // given to another (this one): that process started later, so it isn't the one.
        rig->synth().getUserSettings()->setValue ("copiesLoading.earlier", "another run|" + juce::String (processId) + "@"
                                                                               + juce::String (started - 1) + "|" + identity);
        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::singleChannel; });
        rig->setOutput ([] (OutputSettings& o) { o.mode = OutputSettings::Mode::copies; o.copies = 4; });
        rig->settle();
        auto s = rig->status();
        std::printf ("  %s\n", s.problem.toRawUTF8());
        check (s.loaded == 1 && s.problem.contains ("closed while loading copies"), "no copies are loaded on their own, and it says why");

        rig->synth().reloadCopies();
        check (rig->settle() && rig->status().loaded == 4, "Load the copies again loads them");
        check (! rig->synth().getUserSettings()->containsKey ("copiesLoading.earlier"), "and the marker is gone");
    }

    // A copy that crashes, or throws a C++ exception the synth doesn't catch, in the first piece
    // of a block longer than prepared, with a knob of the first copy just turned (so a change is
    // on its way to it): it's switched off, the rest play on, and nothing calls it again, not the
    // later pieces, nor the host's own timers passing the change on.
    auto copyGoesWrong = [&] (int parameter, const char* how)
    {
        std::printf ("A copy that %s is switched off, and the rest play on\n", how);
        afterCrash.deleteFile();
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = false; });

        rig->synth().getCopy (0)->getParameters()[2]->setValueNotifyingHost (0.3f);
        rig->synth().getCopy (2)->getParameters()[parameter]->setValue (1.0f);     // the synth's Crash or Throw
        {
            juce::AudioBuffer<float> longBlock (2, CopyRig::block * 3);
            juce::MidiBuffer midi;
            rig->processor.processBlock (longBlock, midi);
        }

        rig->idle (600);
        auto s = rig->status();
        std::printf ("  %s\n", s.problem.toRawUTF8());
        check (s.crashed == 1 && s.problem.contains ("crashed"), "the copy is switched off, and it says so");

        const auto calledAfter = afterCrash.loadFileAsString().trim();
        check (calledAfter.isEmpty(), "and is never called again" + (calledAfter.isEmpty() ? juce::String() : ": " + calledAfter));

        const auto audio = rig->play (glideClip(), 1.0);
        check (rig->level (audio, 0.125, e4) > 0.01, "the other copies still play");

        rig->synth().reloadCopies();
        check (rig->settle() && rig->status().loaded == 4 && rig->status().crashed == 0, "Load the copies again replaces it");
        check (afterCrash.loadFileAsString().trim().isEmpty(), "and the one that went wrong is still never called");
        rig->synth().getCopy (0)->getParameters()[2]->setValueNotifyingHost (0.0f);
        rig->setOutput ([] (OutputSettings& o) { o.copiesSleep = true; });
    };

    copyGoesWrong (4, "throws");

    if (CopySafety::canCatchCrashes())
        copyGoesWrong (3, "crashes");
    else
        std::printf ("(A crashing copy can only be caught on Windows: not checked here.)\n");


    std::printf ("An export plays its own copies\n");
    {
        const auto output = rig->processor.getDocument().getSettings().output;
        HostedSynth exportSynth;
        exportSynth.setCopySettings (output);
        bool loaded = false;
        exportSynth.restoreState (*rig->synth().saveState(), [&] (const juce::String&) { exportSynth.whenCopiesLoaded ([&] { loaded = true; }); });

        for (int i = 0; i < 600 && ! loaded; ++i)
            CopyRig::pump (50);

        check (loaded && exportSynth.getCopyStatus().loaded == 4, "the export's synth loads four copies of its own");

        AudioExport::Options options;
        options.sampleRate = CopyRig::rate;
        options.bpm = 120.0;
        options.maxTailSeconds = 0.5;
        AudioExport::prepareForRender (exportSynth, options);
        const auto audio = AudioExport::render (glideClip(), output, exportSynth, options);
        const auto lateC5 = rig->level (audio, 0.925, c5), lateE4 = rig->level (audio, 0.925, e4), earlyE4 = rig->level (audio, 0.125, e4);
        std::printf ("  end: C5 %.4f E4 %.4f (E4 at the start %.4f)\n", lateC5, lateE4, earlyE4);
        check (lateC5 > 0.01 && lateE4 > 0.5 * earlyE4, "in the export too, C4 glides while E4 holds");

        // Through the export itself: its window shows while the copies load, and an export that
        // can't load them all says so rather than playing notes together.
        auto exportOnce = [&] (const juce::File& target)
        {
            AudioExport::Options exportOptions;
            exportOptions.sampleRate = CopyRig::rate;
            exportOptions.bpm = 120.0;
            exportOptions.maxTailSeconds = 0.5;
            bool finished = false;
            juce::String result;
            AudioExport::exportClip (rig->processor, glideClip(), target, exportOptions,
                                     [&] (const juce::String& e) { result = e; finished = true; });

            for (int i = 0; i < 1200 && ! finished; ++i)
                CopyRig::pump (50);

            return finished ? result : juce::String ("(didn't finish)");
        };

        const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MPE Roll copies export.wav");
        wav.deleteFile();
        const auto fine = exportOnce (wav);
        check (fine.isEmpty() && wav.existsAsFile(), "an export with copies finishes: " + fine);
        wav.deleteFile();

        CopySafety::pretendAvailableMemoryMB (500);
        const auto shortOfMemory = exportOnce (wav);
        CopySafety::pretendAvailableMemoryMB (-2);
        std::printf ("  %s\n", shortOfMemory.toRawUTF8());
        check (shortOfMemory.contains ("The export needs 4 copies") && ! wav.existsAsFile(),
               "an export that can't load every copy says so, and writes nothing");
    }

    std::printf ("The audio thread plays on while the copies come and go\n");
    {
        // As in FL: blocks on a thread of their own, a chord looping, while the message thread
        // changes the copies under them (more, fewer, threads on and off, sleep, new sounds).
        rig->setOutput ([] (OutputSettings& o) { o.copies = 4; o.copiesSleep = false; });
        rig->settle();

        MpeClip loop;
        loop.length = 2.0;
        loop.looping = true;

        for (const auto key : { 60, 64, 67, 71 })
            loop.addNote (note (key, 0.0, 1.5, 0.8f));

        rig->processor.getDocument().replaceClip (0, loop, "Stress");
        CopyRig::pump (20);

        std::atomic<bool> stop { false };
        std::atomic<int> blocks { 0 };
        rig->playHead.playing = true;

        std::thread audio ([&]
        {
            juce::AudioBuffer<float> buffer (2, CopyRig::block);
            juce::MidiBuffer midi;

            for (juce::int64 pos = 0; ! stop.load(); pos += CopyRig::block)
            {
                buffer.clear();
                midi.clear();
                rig->playHead.ppq = (double) pos / CopyRig::rate * rig->playHead.bpm / 60.0;
                rig->processor.processBlock (buffer, midi);
                ++blocks;
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        });

        for (int round = 0; round < 8; ++round)
        {
            rig->setOutput ([round] (OutputSettings& o)
            {
                o.copies = round % 2 == 0 ? 8 : 3;
                o.copiesUseCores = round % 3 != 0;
                o.copiesSleep = round % 2 == 1;
            });

            CopyRig::pump (250);
            rig->synth().copySoundToAllCopies();
            CopyRig::pump (150);
            rig->synth().getCopy (0)->setCurrentProgram (round % 3);
            CopyRig::pump (450);
        }

        rig->synth().reloadCopies();
        CopyRig::pump (1500);
        stop = true;
        audio.join();
        rig->playHead.playing = false;

        std::printf ("  %d blocks played meanwhile\n", blocks.load());
        check (blocks.load() > 200, "the audio thread played throughout, never held up for long");
        check (rig->settle() && rig->status().loaded == 3 && rig->copiesMatchFirst(), "and the copies end up loaded, with the first one's sound");
        rig->setOutput ([] (OutputSettings& o) { o.copies = 4; o.copiesSleep = true; o.copiesUseCores = true; });
        rig->settle();
    }

    std::printf ("A project keeps its copies\n");
    {
        rig->synth().getCopy (0)->setCurrentProgram (2);     // Saw
        rig->idle (100);
        rig->settle();
        juce::MemoryBlock project;
        rig->processor.getStateInformation (project);
        rig.reset();

        auto reopened = std::make_unique<CopyRig>();
        reopened->synth().useSettingsFile (settingsFile);     // not the real MPE Roll settings
        reopened->processor.setStateInformation (project.getData(), (int) project.getSize());

        for (int i = 0; i < 200 && reopened->synth().getCopy (0) == nullptr; ++i)
            CopyRig::pump (50);

        check (reopened->settle() && reopened->status().loaded == 4, "opening the project loads the four copies");
        check (reopened->copiesMatchFirst(), "with the project's sound in every one");
    }

    settingsFile.deleteFile();
    afterCrash.deleteFile();
    std::printf ("%s\n", check.failures == 0 ? "PASS" : "FAIL");
    return check.failures == 0 ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::StringArray args (argv + 1, argc - 1);
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (args.size() >= 2 && args[0] == PluginScanner::scanArgument)
        return standInScanner (cwd.getChildFile (args[1]));

    if (args.size() >= 2 && args[0] == "snapshot")
        return snapshot (cwd.getChildFile (args[1]),
                         args.size() >= 4 ? args[2].getIntValue() : 1200,
                         args.size() >= 4 ? args[3].getIntValue() : 760);

    if (args.size() >= 2 && args[0] == "settings")
        return settingsSnapshot (cwd.getChildFile (args[1]), args.size() >= 3 ? args[2] : juce::String());

    if (args.size() >= 2 && args[0] == "skins")
        return skinsSnapshots (cwd.getChildFile (args[1]));

    if (args.size() >= 2 && args[0] == "donate")
    {
        // The Support MPE Roll window's content, at twice the size, for its QR codes to be read back.
        MpeLookAndFeel lookAndFeel;
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        bool ok = false;
        {
            DonatePanel panel;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            ok = writePng (panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f), cwd.getChildFile (args[1]));
        }
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        std::printf ("%s %s\n", ok ? "Wrote" : "Failed to write", cwd.getChildFile (args[1]).getFullPathName().toRawUTF8());
        return ok ? 0 : 1;
    }

    if (args.size() >= 3 && args[0] == "export")
    {
        ignoreXErrors();     // the progress window
        return exportTest (cwd.getChildFile (args[1]), cwd.getChildFile (args[2]));
    }

    if (args.size() >= 3 && args[0] == "render")
        return render (cwd.getChildFile (args[1]), cwd.getChildFile (args[2]));

    if (args.size() >= 1 && args[0] == "selftest")
        return selfTest();

    if (args.size() >= 1 && args[0] == "uitest")
        return uiTest();

    if (args.size() >= 2 && args[0] == "params")
        return parameterTest (cwd.getChildFile (args[1]));

    if (args.size() >= 2 && args[0] == "clapdialect")
        return clapDialectTest (cwd.getChildFile (args[1]));

    if (args.size() >= 3 && args[0] == "vst3dialect")
        return vst3DialectTest (cwd.getChildFile (args[1]), cwd.getChildFile (args[2]));

    if (args.size() >= 2 && args[0] == "copies")
        return copiesTest (cwd.getChildFile (args[1]));

    if (args.size() >= 2 && args[0] == "crashtest")
        return crashTest (cwd.getChildFile (args[1]), args.size() >= 3 && args[2] == "overflow");

    if (args.size() >= 5 && args[0] == "scantest")
        return scanTest (cwd.getChildFile (args[1]), cwd.getChildFile (args[2]), cwd.getChildFile (args[3]),
                         cwd.getChildFile (args[4]));

    if (args.size() >= 4 && args[0] == "loadfailtest")
        return loadFailTest (cwd.getChildFile (args[1]), cwd.getChildFile (args[2]), cwd.getChildFile (args[3]),
                             args.size() >= 5 ? cwd.getChildFile (args[4]) : juce::File());

    std::printf ("Usage:\n  MpeRollHarness snapshot <out.png> [width height]\n"
                 "  MpeRollHarness settings <out.png>\n"
                 "  MpeRollHarness skins <folder>\n"
                 "  MpeRollHarness donate <out.png>\n"
                 "  MpeRollHarness export <synth> <folder>\n"
                 "  MpeRollHarness render <synth.vst3> <out.wav>\n"
                 "  MpeRollHarness selftest\n"
                 "  MpeRollHarness uitest\n"
                 "  MpeRollHarness params <synth.vst3>\n"
                 "  MpeRollHarness clapdialect <MPE Roll Test Synth.clap>\n"
                 "  MpeRollHarness vst3dialect <MPE Roll Note Expression Test Synth.vst3> <MPE Roll Test Synth.vst3>\n"
                 "  MpeRollHarness copies <MPE Roll Plain Test Synth>\n"
                 "  MpeRollHarness crashtest <report.txt> [overflow]\n"
                 "  MpeRollHarness scantest <MPE Roll Scanner> <synth> <crashing.clap> <hanging.clap>\n"
                 "  MpeRollHarness loadfailtest <MPE Roll Scanner> <synth.vst3> <VST2 synth>\n");
    return 2;
}
