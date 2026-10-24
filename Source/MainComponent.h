#pragma once
#include <JuceHeader.h>
#include <regex>
#include "LooperEngine.h"
#include "LoopStretch.h"
#include "PadGrid.h"
#include "MidiPresets.h"
#include "SettingsWindow.h"
#include "PluginManagerWindow.h"
#include "EffectsWindow.h"
#include "Session.h"
#include "OutputBuses.h"

#if JUCE_LINUX && JUCE_JACK
namespace juce { extern StringArray jackOutputPortNames; }   // cmake/PatchJuce.cmake
#endif
#include "FileBrowser.h"
#include "MixerPanel.h"
#include "Panes.h"

//==============================================================================
// The Browser tab of the left pane: the sample browser and the auto-preview box.
class BrowserPane : public juce::Component
{
public:
    BrowserPane (SampleBrowser& b, juce::ToggleButton& autoPreview) : browser (b), autoPreviewButton (autoPreview)
    {
        addAndMakeVisible (browser);
        addAndMakeVisible (autoPreviewButton);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        autoPreviewButton.setBounds (r.removeFromBottom (26));
        r.removeFromBottom (4);
        browser.setBounds (r);
    }

private:
    SampleBrowser& browser;
    juce::ToggleButton& autoPreviewButton;
};

//==============================================================================
// Row of dots, one per beat in the bar; the current beat is lit (downbeat in orange).
class BeatIndicator : public juce::Component
{
public:
    void setBeat (double beatInBar)
    {
        const int b = (int) beatInBar;
        if (b != current) { current = b; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const int n = LooperEngine::beatsPerBar;
        const float d = (float) juce::jmin (getHeight() - 8, getWidth() / n - 6);

        for (int i = 0; i < n; ++i)
        {
            auto dot = juce::Rectangle<float> (d, d).withCentre (
                { ((float) i + 0.5f) * (float) getWidth() / (float) n, (float) getHeight() * 0.5f });

            g.setColour (i == current ? (i == 0 ? juce::Colour (0xffe8a33d) : juce::Colours::white)
                                      : juce::Colour (0xff3a3a3a));
            g.fillEllipse (dot);
        }
    }

private:
    int current = -1;
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      private juce::MenuBarModel,
                      private juce::FileBrowserListener,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent()
    {
        formatManager.registerBasicFormats();

        // Menu bar
        menuBar.setModel (this);
        addAndMakeVisible (menuBar);

        // MIDI mapping from the last session (first run: the engine's default, pads on notes 36-51)
        MidiMapping mapping;
        if (midiPresets.loadCurrent (mapping))
            mapping.applyTo (engine);

        // File explorer
        tree.setDragAndDropDescription (PadComponent::treeDragDescription);
        tree.addListener (this);

        // Preview: double-click a file to hear it, or tick the box to hear each file as it is selected.
        previewThread.startThread();
        autoPreviewButton.setTooltip ("Play each audio file as it is selected in the browser (double-click always plays)");
        autoPreviewButton.onClick = [this]
        {
            if (autoPreviewButton.getToggleState()) previewFile (tree.getSelectedFile());
            else                                    stopPreview();
        };
        addAndMakeVisible (grid);

        // Side panes: the browser on the left, the mixer along the bottom; both tabbed (for more views later), hideable
        // and resizable, as they were left last time.
        leftPane.addTab ("Browser", browserPane);
        bottomPane.addTab ("Mixer", mixerPanel);
        leftPane.setCurrentTab (layout.leftTab);
        bottomPane.setCurrentTab (layout.bottomTab);
        leftPane.setShown (layout.leftShown);
        bottomPane.setShown (layout.bottomShown);

        for (auto* pane : { &leftPane, &bottomPane })
        {
            pane->onShownChanged = [this] { resized(); saveLayout(); };
            addAndMakeVisible (pane);
        }

        leftDivider.onDragStart   = [this] { dragStartSize = leftWidth(); };
        leftDivider.onDrag        = [this] (int d) { layout.leftWidth = leftWidth (dragStartSize + d); resized(); };
        bottomDivider.onDragStart = [this] { dragStartSize = bottomHeight(); };
        bottomDivider.onDrag      = [this] (int d) { layout.bottomHeight = bottomHeight (dragStartSize - d); resized(); };
        addAndMakeVisible (leftDivider);
        addAndMakeVisible (bottomDivider);

        // Transport controls
        tempoSlider.setRange (40.0, 240.0, 0.1);
        tempoSlider.setValue (120.0, juce::dontSendNotification);
        tempoSlider.setTextValueSuffix (" BPM");
        tempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 24);
        tempoSlider.onValueChange = [this]
        {
            engine.setTempo (tempoSlider.getValue());

            // Loops are re-stretched once the slider has settled (see timerCallback).
            tempoChangedAt = juce::Time::getMillisecondCounter();
            tempoDirty = true;
        };
        addAndMakeVisible (tempoSlider);

        quantizeBox.addItemList ({ "Off", "1 bar", "1 beat", "1/8 note", "1/16 note" }, 1);
        quantizeBox.setSelectedId (2, juce::dontSendNotification);   // matches the engine default (bar)
        quantizeBox.onChange = [this] { engine.setQuantize ((Quantize) (quantizeBox.getSelectedId() - 1)); };
        addAndMakeVisible (quantizeBox);

        quantizeLabel.setText ("Quantize", juce::dontSendNotification);
        quantizeLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (quantizeLabel);

        metroButton.onClick = [this] { engine.setMetronome (metroButton.getToggleState()); };
        addAndMakeVisible (metroButton);
        addAndMakeVisible (beats);

        // Recording controls
        recButton.setClickingTogglesState (true);
        recButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc62828));
        recButton.setTooltip ("Record mode: tap an empty pad to record a take, tap a playing loop to overdub");
        recButton.onClick = [this] { engine.setRecordMode (recButton.getToggleState()); };
        addAndMakeVisible (recButton);

        monitorButton.setTooltip ("Hear the input through the app. Use headphones: with speakers and a mic it will feed back.");
        monitorButton.onClick = [this] { engine.setMonitor (monitorButton.getToggleState()); };
        addAndMakeVisible (monitorButton);

        monitorFxButton.setTooltip ("Effects on the input, before the active pad's effects (they're recorded)");
        monitorFxButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2e6b8a));
        monitorFxButton.onClick = [this] { showEffects (LooperEngine::monitorTarget); };
        addAndMakeVisible (monitorFxButton);

        latencyLabel.setText ("Rec latency", juce::dontSendNotification);
        latencyLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (latencyLabel);

        latencySlider.setRange (0.0, 500.0, 1.0);
        latencySlider.setTextValueSuffix (" ms");
        latencySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        latencySlider.setTooltip ("Round-trip audio latency. Raise it if recordings land late against the beat, lower it if early.");
        latencySlider.onValueChange = [this] { applyLatency(); };
        addAndMakeVisible (latencySlider);

        for (int i = 0; i < PadGrid::cols * PadGrid::rows; ++i)
        {
            auto& p = grid.pad (i);
            p.onPressed      = [this] (int pad) { engine.triggerPad (pad); };
            p.onReleased     = [this] (int pad) { engine.releasePad (pad); };
            p.onFileAssigned = [this] (int pad, const juce::File& f) { assignFile (pad, f); };
            p.onCleared      = [this] (int pad) { clearPad (pad); };
            p.onEffects      = [this] (int pad) { showEffects (pad); };

            p.onModeChanged = [this] (int pad, PadMode m)
            {
                setPadMode (pad, m);
                saveCurrentTemplate();
            };

            p.onChokeChanged = [this] (int pad, int group)
            {
                setChokeGroup (pad, group);
                saveCurrentTemplate();
            };

            p.onLengthChanged = [this] (int pad, int numBeats)
            {
                auto& s = sources[(size_t) pad];
                s.beats = numBeats > 0 ? numBeats : guessBeats (s);
                grid.pad (pad).setLengthBeats (s.beats);

                if (s.mode == PadMode::loop)
                    sendToEngine (pad, true);
            };
        }

        // Audio + MIDI (the device chosen in Settings > Audio last time, else the system default)
        // Under JACK: Indigo's own named outputs (see OutputBuses.h), set before any JACK device is created.
       #if JUCE_LINUX && JUCE_JACK
        juce::jackOutputPortNames = OutputBuses::portNames();
       #endif
        auto savedDevice = juce::XmlDocument::parse (audioStateFile());
        deviceManager.initialise (2, 2, savedDevice.get(), true);
        // The engine must be the mixer's first input: only that one sees the live buffer with the
        // audio input in it (the engine copies the input out before clearing it).
        mixer.addInputSource (&engine, false);
        mixer.addInputSource (&previewBus, false);
        player.setSource (&mixer);
        deviceManager.addAudioCallback (&player);

        // Every MIDI input is enabled, including ones plugged in while Indigo is running
        enableAllMidiInputs();
        midiDeviceListConnection = juce::MidiDeviceListConnection::make ([this] { enableAllMidiInputs(); });
        deviceManager.addMidiInputDeviceCallback ({}, &engine);

        audioDeviceChanged();
        deviceManager.addChangeListener (this);
        padEffects.addChangeListener (this);

        // Pad modes from the last run
        PadTemplate padModes;
        if (PadTemplate::load (PadTemplate::currentFile(), padModes))
            applyTemplate (padModes);

        // The window as it was last time, else as tall as fits (pads, transport and mixer need about 900).
        const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const int maxHeight = screen != nullptr ? screen->userArea.getHeight() - 60 : 900;
        setSize (layout.windowWidth > 0 ? layout.windowWidth : 1100,
                 layout.windowHeight > 0 ? layout.windowHeight : juce::jmin (900, maxHeight));
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        saveLayout();
        midiPresets.saveCurrent (MidiMapping::fromEngine (engine));
        saveAudioState();
        saveCurrentTemplate();
        deviceManager.removeChangeListener (this);
        padEffects.removeChangeListener (this);
        settingsWindow = nullptr;
        pluginWindow = nullptr;
        for (auto& w : effectsWindows)
            w = nullptr;
        menuBar.setModel (nullptr);
        stopTimer();
        stretchPool.removeAllJobs (true, 10000);
        midiDeviceListConnection.reset();
        deviceManager.removeMidiInputDeviceCallback ({}, &engine);
        deviceManager.removeAudioCallback (&player);
        player.setSource (nullptr);
        stopPreview();
        mixer.removeAllInputs();
        tree.removeListener (this);
    }

    // Enabling an already-enabled input is a no-op, so this just picks up new devices
    void enableAllMidiInputs()
    {
        for (auto& dev : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled (dev.identifier, true);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff121212)); }

    void resized() override
    {
        auto r = getLocalBounds();
        menuBar.setBounds (r.removeFromTop (menuHeight));

        bottomPane.setBounds (r.removeFromBottom (bottomPane.isShown() ? bottomHeight() : SidePane::collapsedSize));
        bottomDivider.setVisible (bottomPane.isShown());
        if (bottomPane.isShown())
            bottomDivider.setBounds (r.removeFromBottom (dividerSize));

        leftPane.setBounds (r.removeFromLeft (leftPane.isShown() ? leftWidth() : SidePane::collapsedSize));
        leftDivider.setVisible (leftPane.isShown());
        if (leftPane.isShown())
            leftDivider.setBounds (r.removeFromLeft (dividerSize));

        auto bar = r.removeFromTop (44).reduced (10, 8);
        beats.setBounds (bar.removeFromRight (110));
        bar.removeFromRight (10);
        metroButton.setBounds (bar.removeFromRight (110));
        quantizeBox.setBounds (bar.removeFromRight (110));
        quantizeLabel.setBounds (bar.removeFromRight (70));
        tempoSlider.setBounds (bar);

        auto bar2 = r.removeFromTop (36).reduced (10, 4);
        recButton.setBounds (bar2.removeFromLeft (80));
        bar2.removeFromLeft (8);
        monitorButton.setBounds (bar2.removeFromLeft (90));
        monitorFxButton.setBounds (bar2.removeFromLeft (36));
        bar2.removeFromLeft (8);
        latencyLabel.setBounds (bar2.removeFromLeft (80));
        latencySlider.setBounds (bar2);

        grid.setBounds (r.reduced (10));
    }

private:
    // Pane sizes as laid out: what the user dragged them to, kept within what the window allows (the pads keep at
    // least 300 x 200, the mixer is never too short for its faders).
    static constexpr int dividerSize = 5, menuHeight = 24, transportHeight = 80;

    int bottomHeight (int wanted = -1) const
    {
        const int most = getHeight() - menuHeight - transportHeight - 200 - dividerSize;
        return juce::jlimit (240, juce::jmax (240, most), wanted < 0 ? layout.bottomHeight : wanted);
    }

    int leftWidth (int wanted = -1) const
    {
        const int most = getWidth() - 300 - dividerSize;
        return juce::jlimit (200, juce::jmax (200, most), wanted < 0 ? layout.leftWidth : wanted);
    }

    // What the user assigned to a pad, kept on the message thread at the device sample rate.
    // The engine only ever sees a copy (original, or a loop stretched to the current tempo).
    struct PadSource
    {
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        juce::String name;              // shown on the pad
        juce::File file;                // file the audio came from; empty for takes and overdubbed loops
        bool unsaved = false;           // audio that exists only in memory and isn't in a saved session yet
        bool normalised = false;        // `audio` is `file` normalised (Normalise on load)
        PadMode mode = PadMode::oneShot;
        int choke = 0;             // choke group, 0 = none
        double nameBpm = 0.0;      // tempo parsed from the file name ("..._95bpm_..."), 0 if none
        int beats = 4;             // loop length in beats
        int generation = 0;        // bumped on every request so stale stretch results are dropped
        bool mergeInFlight = false;     // an overdub merge is being built; re-stretches wait for it
        bool restretchPending = false;  // a re-stretch was deferred or refused; resent from timerCallback
    };

    // Loop period the engine plays `numBeats` at. Uses the engine's tempo rather than the slider: the
    // engine ignores tempo changes while recording, and loops must match the clock they run against.
    double loopIdeal (int numBeats) const
    {
        return (double) numBeats * 60.0 / engine.getTempo() * engine.getSampleRate();
    }

    // True while a take or overdub on the pad depends on the clip the engine is playing.
    bool padRecordingBusy (int pad) const
    {
        const auto st = engine.getPadStatus (pad);
        return sources[(size_t) pad].mergeInFlight
            || st == PadStatus::recQueued || st == PadStatus::recording
            || st == PadStatus::overdubQueued || st == PadStatus::overdubbing;
    }

    //==== File browser preview ====
    void selectionChanged() override
    {
        if (autoPreviewButton.getToggleState())
            previewFile (tree.getSelectedFile());
    }

    void fileClicked (const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked (const juce::File& f) override  { previewFile (f); }
    void browserRootChanged (const juce::File&) override   {}

    // Plays the file from the start (stopping any previous preview); anything that isn't a readable
    // audio file, such as a folder, just stops it. Reading happens on previewThread, at the file's own rate.
    void previewFile (const juce::File& f)
    {
        stopPreview();

        if (! f.existsAsFile())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (f));
        if (reader == nullptr)
            return;

        const double fileRate = reader->sampleRate;
        previewReader = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
        preview.setSource (previewReader.get(), 32768, &previewThread, fileRate, 2);
        preview.start();
    }

    void stopPreview()
    {
        preview.stop();
        preview.setSource (nullptr);     // detach before the reader it was playing is destroyed
        previewReader.reset();
    }

    //==== Menu bar ====
    enum MenuIds
    {
        menuExit = 1, menuSettings, menuPlugins,
        menuNewSession, menuOpenSession, menuSaveSession, menuSaveSessionAs,
        menuLoadTemplate, menuSaveTemplate, menuSaveTemplateAs,
        menuShowLeftPane, menuShowBottomPane
    };

    juce::StringArray getMenuBarNames() override { return { "File", "Edit", "View" }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;

        if (index == 0)
        {
            m.addItem (menuNewSession,     "New Session");
            m.addItem (menuOpenSession,    "Open Session...");
            m.addItem (menuSaveSession,    "Save Session");
            m.addItem (menuSaveSessionAs,  "Save Session As...");
            m.addSeparator();
            m.addItem (menuLoadTemplate,   "Load Template...");
            m.addItem (menuSaveTemplate,   "Save Template" + (templateFile == juce::File() ? juce::String()
                                                              : " (" + templateFile.getFileNameWithoutExtension() + ")"),
                       templateFile != juce::File());
            m.addItem (menuSaveTemplateAs, "Save Template As...");
            m.addSeparator();
            m.addItem (menuExit, "Exit");
        }
        else if (index == 1)
        {
            m.addItem (menuSettings, "Settings...");
            m.addItem (menuPlugins,  "Plugins...");
        }
        else
        {
            m.addItem (menuShowLeftPane,   "Browser pane (left)", true, leftPane.isShown());
            m.addItem (menuShowBottomPane, "Mixer pane (bottom)", true, bottomPane.isShown());
        }
        return m;
    }

    void menuItemSelected (int id, int) override
    {
        switch (id)
        {
            case menuExit:           juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case menuSettings:       showSettings(); break;
            case menuPlugins:        showPlugins(); break;
            case menuNewSession:     confirmDiscardThen ([this] { newSession(); }); break;
            case menuOpenSession:    confirmDiscardThen ([this] { chooseSessionToOpen(); }); break;
            case menuSaveSession:    saveSession (false); break;
            case menuSaveSessionAs:  saveSession (true); break;
            case menuLoadTemplate:   chooseTemplateToLoad(); break;
            case menuSaveTemplate:   saveTemplate (false); break;
            case menuSaveTemplateAs: saveTemplate (true); break;
            case menuShowLeftPane:   leftPane.setShown (! leftPane.isShown()); break;
            case menuShowBottomPane: bottomPane.setShown (! bottomPane.isShown()); break;
            default: break;
        }
    }

public:
    // Runs `then` straight away, unless some pads hold recordings that are in no saved session: then asks
    // whether to save them first (Save / Discard / Cancel). Used before quitting and loading a session.
    void confirmDiscardThen (std::function<void()> then)
    {
        const bool anyUnsaved = std::any_of (sources.begin(), sources.end(),
                                             [] (const PadSource& s) { return s.audio != nullptr && s.unsaved; });
        if (! anyUnsaved)
        {
            then();
            return;
        }

        if (discardPromptOpen)
            return;

        discardPromptOpen = true;
        juce::Component::SafePointer<MainComponent> safe (this);

        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Unsaved recordings")
                                          .withMessage ("Some pads hold recordings or overdubs that aren't saved in a session. "
                                                        "Save the session first?")
                                          .withButton ("Save...")
                                          .withButton ("Discard")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [safe, then] (int result)     // 1 = Save, 2 = Discard, 0 = Cancel
        {
            if (safe == nullptr)
                return;

            safe->discardPromptOpen = false;

            if (result == 1)      safe->saveSession (false, then);
            else if (result == 2) then();
        });
    }

private:
    //==== Pad modes and templates ====
    void setPadMode (int pad, PadMode m)
    {
        engine.setPadMode (pad, m);
        sources[(size_t) pad].mode = m;
        grid.pad (pad).setMode (m);
        sendToEngine (pad, false);      // loops get the stretched version, others the original
    }

    void setChokeGroup (int pad, int group)
    {
        engine.setChokeGroup (pad, group);
        sources[(size_t) pad].choke = group;
        grid.pad (pad).setChokeGroup (group);
    }

    PadTemplate currentTemplate() const
    {
        PadTemplate t;
        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            t.modes[(size_t) i]  = sources[(size_t) i].mode;
            t.chokes[(size_t) i] = sources[(size_t) i].choke;
        }
        t.effects = padEffects.toXml();
        return t;
    }

    void applyTemplate (const PadTemplate& t)
    {
        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            if (t.modes[(size_t) i] != sources[(size_t) i].mode)
                setPadMode (i, t.modes[(size_t) i]);

            setChokeGroup (i, t.chokes[(size_t) i]);
        }

        reportMissingEffects (padEffects.fromXml (t.effects.get()));
    }

    void reportMissingEffects (const juce::StringArray& missing)
    {
        if (! missing.isEmpty())
            showError ("Effects", "These plugins couldn't be loaded, so they were removed:\n\n" + missing.joinIntoString ("\n"));
    }

    void saveCurrentTemplate() const { currentTemplate().save (PadTemplate::currentFile()); }

    void chooseTemplateToLoad()
    {
        PadTemplate::folder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> ("Load template",
                                                           templateFile != juce::File() ? templateFile : PadTemplate::folder(),
                                                           juce::String ("*") + PadTemplate::fileExtension);
        juce::Component::SafePointer<MainComponent> safe (this);

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [safe] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe == nullptr || f == juce::File())
                return;

            PadTemplate t;
            if (! PadTemplate::load (f, t))
            {
                safe->showError ("Load template", "Couldn't read " + f.getFullPathName());
                return;
            }

            safe->applyTemplate (t);
            safe->templateFile = f;
            safe->saveCurrentTemplate();
        });
    }

    void saveTemplate (bool saveAs)
    {
        if (! saveAs && templateFile != juce::File())
        {
            if (! currentTemplate().save (templateFile))
                showError ("Save template", "Couldn't write " + templateFile.getFullPathName());
            return;
        }

        PadTemplate::folder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> ("Save template as",
                                                           templateFile != juce::File() ? templateFile
                                                               : PadTemplate::folder().getChildFile (juce::String ("Untitled") + PadTemplate::fileExtension),
                                                           juce::String ("*") + PadTemplate::fileExtension);
        juce::Component::SafePointer<MainComponent> safe (this);

        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [safe] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (safe == nullptr || f == juce::File())
                return;

            f = f.withFileExtension (PadTemplate::fileExtension);

            if (safe->currentTemplate().save (f)) safe->templateFile = f;
            else                                  safe->showError ("Save template", "Couldn't write " + f.getFullPathName());
        });
    }

    //==== Sessions ====
    void newSession()
    {
        for (int i = 0; i < LooperEngine::numPads; ++i)
            clearPad (i);

        engine.resetMixer();

        sessionFile = juce::File();
        updateTitle();
    }

    void chooseSessionToOpen()
    {
        Session::folder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> ("Open session",
                                                           sessionFile != juce::File() ? sessionFile : Session::folder(),
                                                           juce::String ("*") + Session::fileExtension);
        juce::Component::SafePointer<MainComponent> safe (this);

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [safe] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe != nullptr && f != juce::File())
                safe->loadSession (f);
        });
    }

    void loadSession (const juce::File& file)
    {
        Session session;
        auto xml = juce::XmlDocument::parse (file);

        if (xml == nullptr || ! Session::fromXml (*xml, file, session))
        {
            showError ("Open session", "Couldn't read " + file.getFullPathName());
            return;
        }

        for (int i = 0; i < LooperEngine::numPads; ++i)
            clearPad (i);

        // Loops are stretched to the old tempo first; the tempo change re-stretches them (timerCallback).
        tempoSlider.setValue (session.bpm, juce::sendNotificationSync);
        quantizeBox.setSelectedId (session.quantize + 1, juce::sendNotificationSync);

        const auto audioFolder = Session::audioFolderFor (file);
        juce::StringArray missing;

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            const auto& p = session.pads[(size_t) i];
            auto& s = sources[(size_t) i];

            if (p.mode != s.mode)
                setPadMode (i, p.mode);

            setChokeGroup (i, p.choke);

            engine.setFader (i, p.fader);
            engine.setPan (i, p.pan);
            engine.setMute (i, p.mute);
            engine.setSolo (i, p.solo);
        }

        reportMissingEffects (padEffects.fromXml (session.effects.get()));

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            const auto& p = session.pads[(size_t) i];
            auto& s = sources[(size_t) i];

            if (p.file == juce::File())
                continue;

            auto audio = loadAudio (p.file, engine.getSampleRate());
            if (audio == nullptr)
            {
                missing.add ("Pad " + juce::String (i + 1) + ": " + p.file.getFullPathName());
                continue;
            }

            // Audio the session wrote itself counts as a take: the next save writes it again (to a new
            // location on Save As) instead of pointing back at this session's folder.
            const bool ownAudio = p.file.isAChildOf (audioFolder);

            // A file that was normalised when it was loaded is normalised again (the session points at the original).
            s.normalised = p.normalised && ! ownAudio;
            s.audio   = s.normalised ? Mixer::normalised (*audio, engine.getSampleRate()) : std::move (audio);
            s.name    = p.name.isNotEmpty() ? p.name : p.file.getFileNameWithoutExtension();
            s.file    = ownAudio ? juce::File() : p.file;
            s.unsaved = false;
            s.nameBpm = ownAudio ? 0.0 : bpmFromName (p.file.getFileNameWithoutExtension());
            s.beats   = p.beats;

            grid.pad (i).setClipName (s.name);
            grid.pad (i).setLengthBeats (s.beats);
            sendToEngine (i, false);
        }

        sessionFile = file;
        updateTitle();
        saveCurrentTemplate();

        if (! missing.isEmpty())
            showError ("Open session", "These sounds couldn't be found or read, so their pads were left empty:\n\n"
                                           + missing.joinIntoString ("\n"));
    }

    // Saves to the current session file (or asks for one), then runs `then` if the save worked.
    void saveSession (bool saveAs, std::function<void()> then = {})
    {
        if (! saveAs && sessionFile != juce::File())
        {
            if (writeSession (sessionFile) && then)
                then();
            return;
        }

        Session::folder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> ("Save session as",
                                                           sessionFile != juce::File() ? sessionFile
                                                               : Session::folder().getChildFile (juce::String ("Untitled") + Session::fileExtension),
                                                           juce::String ("*") + Session::fileExtension);
        juce::Component::SafePointer<MainComponent> safe (this);

        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [safe, then] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (safe == nullptr || f == juce::File())
                return;

            f = f.withFileExtension (Session::fileExtension);

            if (safe->writeSession (f))
            {
                safe->sessionFile = f;
                safe->updateTitle();
                if (then) then();
            }
        });
    }

    bool writeSession (const juce::File& file)
    {
        const auto audioFolder = Session::audioFolderFor (file);
        Session session;
        session.bpm = tempoSlider.getValue();
        session.quantize = quantizeBox.getSelectedId() - 1;
        session.effects = padEffects.toXml();

        juce::Array<juce::File> keep;       // files in the audio folder the session refers to

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            const auto& s = sources[(size_t) i];
            auto& p = session.pads[(size_t) i];
            p.mode  = s.mode;
            p.choke = s.choke;
            p.beats = s.beats;
            p.fader = engine.getFader (i);
            p.pan   = engine.getPan (i);
            p.mute  = engine.isMuted (i);
            p.solo  = engine.isSoloed (i);

            if (s.audio == nullptr)
                continue;

            p.name = s.name;

            if (s.file != juce::File())
            {
                p.file = s.file;
                p.normalised = s.normalised;
                keep.add (s.file);
                continue;
            }

            // A take or overdubbed loop: write it out at the rate it's held at.
            p.file = audioFolder.getChildFile ("Pad " + juce::String (i + 1).paddedLeft ('0', 2) + ".wav");

            if (! audioFolder.createDirectory() || ! writeWav (p.file, *s.audio, sourceRate))
            {
                showError ("Save session", "Couldn't write " + p.file.getFullPathName());
                return false;
            }
            keep.add (p.file);
        }

        if (! session.toXml (file)->writeTo (file))
        {
            showError ("Save session", "Couldn't write " + file.getFullPathName());
            return false;
        }

        // Drop takes left over from pads that have since been cleared or replaced.
        for (auto& f : audioFolder.findChildFiles (juce::File::findFiles, false, "Pad *.wav"))
            if (! keep.contains (f))
                f.deleteFile();

        if (audioFolder.isDirectory() && audioFolder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            audioFolder.deleteFile();

        for (auto& s : sources)
            s.unsaved = false;

        return true;
    }

    // 32-bit float, so overdubs that went over full scale aren't clipped.
    static bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        file.deleteFile();
        auto stream = file.createOutputStream();
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate,
                                                                              (unsigned int) audio.getNumChannels(), 32, {}, 0));
        if (writer == nullptr)
            return false;

        stream.release();       // the writer owns it now
        return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    void updateTitle()
    {
        if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
            window->setName (sessionFile == juce::File() ? juce::String ("Indigo")
                                                         : "Indigo - " + sessionFile.getFileNameWithoutExtension());
    }

    void showError (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message, {}, this);
    }

    void saveLayout()
    {
        layout.leftShown = leftPane.isShown();
        layout.bottomShown = bottomPane.isShown();
        layout.leftTab = leftPane.getCurrentTab();
        layout.bottomTab = bottomPane.getCurrentTab();
        layout.windowWidth = getWidth();
        layout.windowHeight = getHeight();
        layout.save();
    }

    void showSettings()
    {
        if (settingsWindow == nullptr)
        {
            settingsWindow = std::make_unique<SettingsWindow> (engine, midiPresets, deviceManager,
                                                              [this] { return browser.getCurrentFolder(); });
            settingsWindow->onClosed = [this]
            {
                midiPresets.saveCurrent (MidiMapping::fromEngine (engine));
                saveAudioState();
            };
        }
        settingsWindow->open (this);
    }

    void showPlugins()
    {
        if (pluginWindow == nullptr)
            pluginWindow = std::make_unique<PluginManagerWindow> (plugins);

        pluginWindow->open (this);
    }

    void showEffects (int target)
    {
        auto& w = effectsWindows[(size_t) target];
        if (w == nullptr)
            w = std::make_unique<EffectsWindow> (padEffects, engine, target);

        w->open (this);
    }

    // Pad FX / instrument tags, the monitor FX button, REC (instruments record without an input), and the
    // effects saved for the next run.
    void effectsChanged()
    {
        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            grid.pad (i).setHasEffects (padEffects.hasEffects (i));
            const auto* inst = padEffects.getInstrument (i);
            grid.pad (i).setInstrumentName (inst != nullptr ? inst->description.name : juce::String());
        }

        monitorFxButton.setToggleState (padEffects.hasEffects (LooperEngine::monitorTarget), juce::dontSendNotification);
        updateRecordControls();
        saveCurrentTemplate();
    }

    // REC needs an audio input, or an instrument pad to record; Monitor needs an input.
    void updateRecordControls()
    {
        const bool haveInput = engine.getNumInputChannels() > 0;
        const bool canRecord = haveInput || padEffects.hasAnyInstrument();

        recButton.setEnabled (canRecord);
        recButton.setButtonText (canRecord ? "REC" : "No input");
        monitorButton.setEnabled (haveInput);

        if (! canRecord)
            engine.setRecordMode (false);

        if (! haveInput)
        {
            engine.setMonitor (false);
            monitorButton.setToggleState (false, juce::dontSendNotification);
        }
    }

    //==== Audio device ====
    static juce::File audioStateFile() { return MidiPresets::folder().getChildFile ("AudioDevice.xml"); }

    void saveAudioState()
    {
        // Null until the user has changed something in Settings > Audio; keep using the default until then.
        if (auto xml = deviceManager.createStateXml())
            if (audioStateFile().getParentDirectory().createDirectory())
                xml->writeTo (audioStateFile());
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &padEffects) effectsChanged();
        else                       audioDeviceChanged();
    }

    // Brings everything that depends on the audio device in line with it: the engine's input channel
    // count, the record / monitor controls, the latency guess, and (on a rate change) the pads' audio.
    void audioDeviceChanged()
    {
        auto* dev = deviceManager.getCurrentAudioDevice();

        // JACK devices have the separate metronome / preview outputs.
        OutputBuses::separate.store (dev != nullptr && dev->getTypeName() == "JACK" && ! dev->getOutputChannelNames().isEmpty());

        // The device manager also broadcasts for things like MIDI input changes; ignore those. Under JACK the
        // active inputs follow the patchbay connections, so rewiring lands here as a channel change only.
        const auto key = dev == nullptr ? juce::String()
                                        : dev->getTypeName() + "|" + dev->getName()
                                              + "|" + juce::String (dev->getCurrentSampleRate())
                                              + "|" + juce::String (dev->getCurrentBufferSizeSamples());
        const auto channels = dev == nullptr ? juce::String()
                                             : dev->getActiveInputChannels().toString (2)
                                                   + "|" + dev->getActiveOutputChannels().toString (2);
        const bool deviceChanged = key != deviceKey || ! deviceKnown;

        if (! deviceChanged && channels == deviceChannels)
            return;

        deviceKey = key;
        deviceChannels = channels;
        deviceKnown = true;

        const int numIn = dev != nullptr ? dev->getActiveInputChannels().countNumberOfSetBits() : 0;
        engine.setNumInputChannels (numIn);

        updateRecordControls();

        if (dev == nullptr || ! deviceChanged)
            return;     // a channel change keeps the latency the user tuned (and the rate)

        // Starting latency guess from what the device reports.
        // (Drivers often under-report; use the latency slider to fine-tune by ear.)
        const double sr = dev->getCurrentSampleRate();
        const int reported = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
        outputLatencyShare = reported > 0 ? (double) dev->getOutputLatencyInSamples() / reported : 0.5;
        latencySlider.setValue (sr > 0.0 ? 1000.0 * reported / sr : 0.0, juce::dontSendNotification);
        applyLatency();

        if (sr > 0.0 && std::abs (sr - sourceRate) >= 1.0)
            sampleRateChanged (sourceRate, sr);
    }

    // The pads' audio is kept at the device rate. The engine has already restarted its clock and
    // dropped any take or overdub (prepareToPlay); resample every pad and send it again.
    void sampleRateChanged (double oldRate, double newRate)
    {
        // Takes finished before the switch arrive at the old rate: take them in first so they get converted too.
        engine.pollEvents ([this] (const LooperEngine::Event& e) { handleEngineEvent (e); });
        sourceRate = newRate;

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            auto& s = sources[(size_t) i];
            if (s.audio == nullptr)
                continue;

            s.audio = std::make_shared<const juce::AudioBuffer<float>> (resample (*s.audio, oldRate / newRate));
            s.mergeInFlight = false;
            sendToEngine (i, false);    // bumps the generation, so stretches / merges made at the old rate are dropped
        }
    }

    // Instruments are played from what you hear, so only the output half of the round trip applies to them;
    // it keeps the device's own split of the reported latency, scaled to what the slider is tuned to.
    void applyLatency()
    {
        const double samples = latencySlider.getValue() * 0.001 * engine.getSampleRate();
        engine.setRecordLatency ((int) std::lround (samples));
        engine.setInstrumentLatency ((int) std::lround (samples * outputLatencyShare));
    }

    // Keeps a couple of cleared record buffers queued for the audio thread (one per tick: allocating is slow).
    void topUpRecordBuffers()
    {
        if (engine.getNumRecordBuffers() >= 2)
            return;

        auto buf = std::make_unique<Clip>();
        buf->audio.setSize (2, (int) (LooperEngine::maxRecordSeconds * engine.getSampleRate()));
        buf->audio.clear();
        engine.supplyRecordBuffer (std::move (buf));
    }

    void timerCallback() override
    {
        engine.pollEvents ([this] (const LooperEngine::Event& e) { handleEngineEvent (e); });
        topUpRecordBuffers();

        const bool anySolo = engine.isAnySoloed();

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            grid.pad (i).setStatus (engine.getPadStatus (i));
            grid.pad (i).setActive (i == engine.getActivePad());
            grid.pad (i).setSilenced (engine.isMuted (i) || (anySolo && ! engine.isSoloed (i)));
        }

        if (bottomPane.isShown())
        {
            for (int i = 0; i < LooperEngine::numPads; ++i)
            {
                const auto* inst = padEffects.getInstrument (i);
                mixerPanel.strip (i).setPadName (sources[(size_t) i].name.isNotEmpty() ? sources[(size_t) i].name
                                                 : inst != nullptr ? inst->description.name : juce::String());
            }

            mixerPanel.refresh();
        }

        beats.setBeat (engine.getBeatInBar());

        // REC mode can be toggled from the controller, so follow the engine (only when it changes,
        // otherwise a click would flicker back before the engine has applied it).
        if (engine.isRecordMode() != lastRecordMode)
        {
            lastRecordMode = engine.isRecordMode();
            recButton.setToggleState (lastRecordMode, juce::dontSendNotification);
            grid.setRecordMode (lastRecordMode);
        }

        // The tempo is locked while a take / overdub is in progress or being merged.
        const bool busy = engine.isRecordingBusy();
        tempoSlider.setEnabled (! busy);
        if (! busy && ! tempoDirty && std::abs (tempoSlider.getValue() - engine.getTempo()) > 0.05)
            tempoSlider.setValue (engine.getTempo(), juce::dontSendNotification);

        // Re-stretch loops 250 ms after the last tempo change.
        if (tempoDirty && juce::Time::getMillisecondCounter() - tempoChangedAt > 250)
        {
            tempoDirty = false;

            for (int i = 0; i < LooperEngine::numPads; ++i)
                if (sources[(size_t) i].mode == PadMode::loop)
                    sendToEngine (i, true);
        }

        // Resend re-stretches that had to wait for a recording or overdub on the pad.
        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            auto& s = sources[(size_t) i];

            if (s.restretchPending && s.mode != PadMode::loop)
                s.restretchPending = false;
            else if (s.restretchPending && ! padRecordingBusy (i))
                sendToEngine (i, true);
        }
    }

    //==== Results from the audio thread ====
    void handleEngineEvent (const LooperEngine::Event& e)
    {
        using Type = LooperEngine::Event::Type;

        if (e.type == Type::fxStale)        // e.pad may be the monitor, so before touching sources
        {
            padEffects.resend (e.pad);
            return;
        }
        auto& s = sources[(size_t) e.pad];
        const double sr = engine.getSampleRate();

        if (e.type == Type::swapRejected)
        {
            s.restretchPending = true;
            return;
        }

        if (e.type == Type::recorded)
        {
            // The take is already looping from its record buffer. Keep an exact-length copy as the pad's
            // source (so it can be re-stretched when the tempo changes) and swap it in for the big buffer.
            auto audio = std::make_shared<juce::AudioBuffer<float>> (2, e.length);
            for (int ch = 0; ch < 2; ++ch)
                audio->copyFrom (ch, 0, e.clip->audio, ch, 0, e.length);

            s.audio   = audio;
            s.name    = "Take " + juce::String (++takeCount);
            s.file    = juce::File();
            s.normalised = false;
            s.unsaved = true;
            s.mode    = PadMode::loop;
            s.nameBpm = 0.0;
            s.beats   = e.beats;
            s.restretchPending = false;     // the take was made at the engine's current tempo
            ++s.generation;

            auto& padUi = grid.pad (e.pad);
            padUi.setClipName (s.name);
            padUi.setMode (PadMode::loop);
            padUi.setLengthBeats (e.beats);

            auto clip = std::make_unique<Clip>();
            clip->audio = *audio;
            clip->idealLength = e.ideal;
            engine.assignClip (e.pad, std::move (clip), true);
        }
        else if (e.type == Type::overdubbed)
        {
            if (s.audio == nullptr)
                return;

            // The pad no longer matches the file it came from; a session save has to write it out.
            s.file = juce::File();
            s.normalised = false;
            s.unsaved = true;

            // Merge on a worker thread: the pad's source (stretched to the current tempo, exactly as the
            // engine is playing it) plus the recorded layer. The engine keeps playing the layer alongside
            // the old clip until the merged clip lands, so there is no gap or double-up.
            auto layer = std::make_shared<juce::AudioBuffer<float>> (2, e.length);
            for (int ch = 0; ch < 2; ++ch)
                layer->copyFrom (ch, 0, e.clip->audio, ch, 0, e.length);

            const int pad = e.pad;
            const int outLen = e.length;
            const double ideal = e.ideal;
            const int gen = ++s.generation;
            auto source = s.audio;
            s.mergeInFlight = true;
            juce::Component::SafePointer<MainComponent> safe (this);

            stretchPool.addJob ([=]
            {
                juce::AudioBuffer<float> base = source->getNumSamples() == outLen
                                                    ? juce::AudioBuffer<float> (*source)
                                                    : stretchLoopToLength (*source, outLen, sr);

                auto merged = std::make_shared<juce::AudioBuffer<float>> (2, outLen);
                merged->clear();
                const int n = juce::jmin (outLen, base.getNumSamples());

                for (int ch = 0; ch < 2; ++ch)
                {
                    if (base.getNumChannels() > 0)
                        merged->copyFrom (ch, 0, base, juce::jmin (ch, base.getNumChannels() - 1), 0, n);

                    merged->addFrom (ch, 0, *layer, ch, 0, outLen);
                }

                juce::MessageManager::callAsync ([safe, pad, gen, merged, ideal, outLen]
                {
                    if (safe == nullptr || safe->sources[(size_t) pad].generation != gen)
                        return;     // superseded (e.g. a new file was dropped on the pad)

                    auto& src = safe->sources[(size_t) pad];
                    src.audio = merged;
                    src.mergeInFlight = false;

                    auto clip = std::make_unique<Clip>();
                    clip->audio = *merged;
                    clip->idealLength = ideal;
                    safe->engine.assignClip (pad, std::move (clip), true, true);

                    // The merge has the length the overdub was recorded at; catch up if the tempo moved since.
                    if (juce::jmax (1, (int) std::ceil (safe->loopIdeal (src.beats))) != outLen)
                        src.restretchPending = true;
                });
            });
        }
    }

    void assignFile (int pad, const juce::File& file)
    {
        auto audio = loadAudio (file, engine.getSampleRate());
        if (audio == nullptr)
            return;

        auto& s = sources[(size_t) pad];
        s.normalised = MixerSettings::normaliseOnLoad();
        s.audio   = s.normalised ? Mixer::normalised (*audio, engine.getSampleRate()) : std::move (audio);
        s.name    = file.getFileNameWithoutExtension();
        s.file    = file;
        s.unsaved = false;
        s.nameBpm = bpmFromName (s.name);
        s.beats   = guessBeats (s);

        grid.pad (pad).setClipName (s.name);
        grid.pad (pad).setLengthBeats (s.beats);
        sendToEngine (pad, false);
    }

    void clearPad (int pad)
    {
        // Deliver anything the engine has already finished first, so a take that just landed
        // can't refill the pad after it has been cleared.
        engine.pollEvents ([this] (const LooperEngine::Event& e) { handleEngineEvent (e); });

        auto& s = sources[(size_t) pad];
        s.audio   = nullptr;
        s.name    = {};
        s.file    = juce::File();
        s.normalised = false;
        s.unsaved = false;
        s.nameBpm = 0.0;
        s.beats   = 4;
        s.mergeInFlight = false;
        s.restretchPending = false;
        ++s.generation;                 // drops any stretch or merge still in flight

        grid.pad (pad).setClipName ({});
        grid.pad (pad).setLengthBeats (0);
        engine.clearPad (pad);
    }

    // Sends the pad's sound to the engine: the original for one-shot/gate pads, or, for loop
    // pads, a copy time-stretched (pitch preserved) to fill s.beats beats at the current tempo.
    // keepPhase = the loop may already be playing; swap without stopping it.
    void sendToEngine (int pad, bool keepPhase)
    {
        auto& s = sources[(size_t) pad];
        if (s.audio == nullptr)
            return;

        // A loop swap now would be refused by the engine or would cut short an overdub's merge
        // (bumping the generation drops it), so hold it until the pad is free.
        if (keepPhase && padRecordingBusy (pad))
        {
            s.restretchPending = true;
            return;
        }

        s.restretchPending = false;
        if (! keepPhase)
            s.mergeInFlight = false;    // the engine discards the overdub on a swap that restarts the pad

        const int gen = ++s.generation;

        if (s.mode != PadMode::loop)
        {
            auto clip = std::make_unique<Clip>();
            clip->audio = *s.audio;
            engine.assignClip (pad, std::move (clip), false);
            return;
        }

        const double sr    = engine.getSampleRate();
        const double ideal = loopIdeal (s.beats);
        const int outLen   = juce::jmax (1, (int) std::ceil (ideal));
        auto source = s.audio;
        juce::Component::SafePointer<MainComponent> safe (this);

        stretchPool.addJob ([=]
        {
            auto stretched = std::make_shared<Clip>();
            // A source that already has exactly the right length (a recording at this tempo) needs no stretching.
            stretched->audio = source->getNumSamples() == outLen ? juce::AudioBuffer<float> (*source)
                                                                  : stretchLoopToLength (*source, outLen, sr);
            stretched->idealLength = ideal;

            juce::MessageManager::callAsync ([safe, pad, gen, keepPhase, stretched]
            {
                if (safe == nullptr || safe->sources[(size_t) pad].generation != gen)
                    return;     // superseded by a newer request

                auto clip = std::make_unique<Clip>();
                clip->audio = std::move (stretched->audio);
                clip->idealLength = stretched->idealLength;
                safe->engine.assignClip (pad, std::move (clip), keepPhase);
            });
        });
    }

    // Best guess at how many beats a clip spans.
    int guessBeats (const PadSource& s) const
    {
        if (s.audio == nullptr)
            return 4;

        const double seconds = (double) s.audio->getNumSamples() / engine.getSampleRate();

        // A tempo in the file name is the most reliable hint.
        if (s.nameBpm > 0.0)
            return juce::jmax (1, (int) std::lround (seconds * s.nameBpm / 60.0));

        // Otherwise: the power-of-two length whose implied tempo is closest to the current tempo.
        const double beatsAtProjectTempo = seconds * tempoSlider.getValue() / 60.0;
        int best = 1;
        double bestErr = 1.0e9;

        for (int b : PadComponent::lengthChoices)
        {
            const double err = std::abs (std::log (beatsAtProjectTempo / (double) b));
            if (err < bestErr) { bestErr = err; best = b; }
        }
        return best;
    }

    static double bpmFromName (const juce::String& name)
    {
        static const std::regex re (R"((\d{2,3})\s*[-_ ]?bpm|bpm\s*[-_ ]?(\d{2,3}))", std::regex::icase);
        std::smatch m;
        const auto text = name.toStdString();

        if (std::regex_search (text, m, re))
        {
            const double bpm = std::stod (m[1].matched ? m[1].str() : m[2].str());
            if (bpm >= 40.0 && bpm <= 300.0)
                return bpm;
        }
        return 0.0;
    }

    // Reads and resamples to the device rate. Runs on the message thread (fine for short samples;
    // move to a background thread for long files).
    std::shared_ptr<const juce::AudioBuffer<float>> loadAudio (const juce::File& file, double targetRate)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        constexpr juce::int64 maxSeconds = 120;

        if (reader == nullptr || reader->lengthInSamples <= 0
            || reader->lengthInSamples > (juce::int64) (reader->sampleRate * maxSeconds))
            return nullptr;

        const int numCh = (int) juce::jmin (2u, reader->numChannels);
        const int len   = (int) reader->lengthInSamples;

        juce::AudioBuffer<float> raw (numCh, len);
        reader->read (&raw, 0, len, 0, true, numCh > 1);

        if (std::abs (reader->sampleRate - targetRate) < 1.0)
            return std::make_shared<const juce::AudioBuffer<float>> (std::move (raw));

        return std::make_shared<const juce::AudioBuffer<float>> (resample (raw, reader->sampleRate / targetRate));
    }

    // ratio = source rate / target rate
    static juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double ratio)
    {
        const int numCh  = in.getNumChannels();
        const int outLen = juce::jmax (1, (int) std::ceil ((double) in.getNumSamples() / ratio));
        juce::AudioBuffer<float> out (numCh, outLen);

        for (int ch = 0; ch < numCh; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, in.getReadPointer (ch), out.getWritePointer (ch), outLen);
        }
        return out;
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    juce::MidiDeviceListConnection midiDeviceListConnection;   // MIDI hot-plug
    LooperEngine engine;

    juce::MixerAudioSource mixer;                       // engine + file preview
    juce::AudioTransportSource preview;
    OutputBuses::Source previewBus { preview, OutputBuses::preview };   // the preview, on its own outputs under JACK
    std::unique_ptr<juce::AudioFormatReaderSource> previewReader;
    juce::TimeSliceThread previewThread { "Preview reader" };

    juce::AudioFormatManager formatManager;
    SampleBrowser browser;
    juce::FileTreeComponent& tree = browser.getTree();
    juce::ToggleButton autoPreviewButton { "Auto-preview selected file" };

    PadGrid grid;
    juce::Slider tempoSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ComboBox quantizeBox;
    juce::Label quantizeLabel;
    juce::ToggleButton metroButton { "Metronome" };
    BeatIndicator beats;

    juce::TextButton recButton { "REC" };
    double outputLatencyShare = 0.5;     // output latency / round trip, as the device reports them
    juce::ToggleButton monitorButton { "Monitor" };
    juce::TextButton monitorFxButton { "FX" };
    juce::Label latencyLabel;
    juce::Slider latencySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    bool lastRecordMode = false;
    int takeCount = 0;

    std::array<PadSource, LooperEngine::numPads> sources;
    juce::uint32 tempoChangedAt = 0;
    bool tempoDirty = false;

    juce::File sessionFile, templateFile;   // last opened / saved (empty = none yet)
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool discardPromptOpen = false;

    double sourceRate = 44100.0;        // rate the pads' audio is held at (follows the device)
    juce::String deviceKey;             // device / rate / buffer size audioDeviceChanged last adapted to
    juce::String deviceChannels;        // ...and the active input / output channels
    bool deviceKnown = false;

    juce::MenuBarComponent menuBar;
    MidiPresets midiPresets;
    std::unique_ptr<SettingsWindow> settingsWindow;
    PluginLibrary plugins;
    std::unique_ptr<PluginManagerWindow> pluginWindow;     // after `plugins`, so it goes first
    PadEffects padEffects { engine, plugins };
    std::array<std::unique_ptr<EffectsWindow>, PadEffects::numTargets> effectsWindows;

    // Panes (declared after what they show, so they let go of it first)
    LayoutSettings layout = LayoutSettings::load();
    BrowserPane browserPane { browser, autoPreviewButton };
    MixerPanel mixerPanel { engine };
    SidePane leftPane { true, "browser" }, bottomPane { false, "mixer" };
    PaneDivider leftDivider { true }, bottomDivider { false };
    int dragStartSize = 0;

    juce::ThreadPool stretchPool { 2 };    // declared last so it is destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
