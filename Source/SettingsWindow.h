#pragma once
#include <JuceHeader.h>
#include "LooperEngine.h"
#include "MidiPresets.h"
#include "FileBrowser.h"
#include "OutputBuses.h"

//==============================================================================
// One line of the mapping table: what a pad, the REC toggle or a mixer control responds to. Edits go straight to the
// engine; a timer keeps the row in step with MIDI learn, which happens on the MIDI thread.
class MappingRow : public juce::Component
{
public:
    MappingRow (LooperEngine& e, int targetId, const juce::String& name) : engine (e), target (targetId)
    {
        nameLabel.setText (name, juce::dontSendNotification);
        addAndMakeVisible (nameLabel);

        typeBox.addItemList ({ "None", "Note", "CC" }, 1);      // ids = MidiBinding::Kind + 1
        typeBox.setItemEnabled (2, ! LooperEngine::isContinuous (target));    // a volume / pan knob needs a CC
        typeBox.onChange = [this] { commit(); };
        addAndMakeVisible (typeBox);

        numberSlider.setSliderStyle (juce::Slider::IncDecButtons);
        numberSlider.setRange (0.0, 127.0, 1.0);
        numberSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 24);
        numberSlider.onValueChange = [this] { commit(); };
        addAndMakeVisible (numberSlider);

        channelBox.addItem ("Any ch", 1);                       // ids = channel + 1
        for (int ch = 1; ch <= 16; ++ch)
            channelBox.addItem ("Ch " + juce::String (ch), ch + 1);
        channelBox.setTooltip ("MIDI channel this control is matched on. Learn picks up the channel your controller sends.");
        channelBox.onChange = [this] { commit(); };
        addAndMakeVisible (channelBox);

        detailLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (detailLabel);

        learnButton.setTooltip (LooperEngine::isContinuous (target) ? "Click, then move a knob or fader on your controller"
                                                                    : "Click, then press a pad, key or button on your controller");
        learnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc62828));
        learnButton.onClick = [this]
        {
            if (engine.getLearnTarget() == target) engine.cancelMidiLearn();
            else                                   engine.startMidiLearn (target);
            refresh();
        };
        addAndMakeVisible (learnButton);

        show (engine.getBinding (target));
    }

    void refresh()
    {
        const auto b = engine.getBinding (target);
        if (b != shown || engine.getPerformanceChannel() != shownPerformanceChannel)
            show (b);

        const bool learning = engine.getLearnTarget() == target;
        learnButton.setToggleState (learning, juce::dontSendNotification);
        learnButton.setButtonText (learning ? (LooperEngine::isContinuous (target) ? "Move..." : "Press...") : "Learn");
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (0, 3);
        nameLabel.setBounds (r.removeFromLeft (110));
        typeBox.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (8);
        numberSlider.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        channelBox.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (8);
        learnButton.setBounds (r.removeFromRight (80));
        detailLabel.setBounds (r);
    }

private:
    void show (MidiBinding b)
    {
        shown = b;
        shownPerformanceChannel = engine.getPerformanceChannel();

        typeBox.setSelectedId ((int) b.kind + 1, juce::dontSendNotification);
        numberSlider.setValue (b.number, juce::dontSendNotification);
        numberSlider.setEnabled (b.kind != MidiBinding::Kind::none);
        channelBox.setSelectedId (b.channel + 1, juce::dontSendNotification);
        channelBox.setEnabled (b.kind != MidiBinding::Kind::none);

        // A control on the performance channel never reaches the pads; say so rather than look broken.
        const bool unreachable = b.kind != MidiBinding::Kind::none && b.channel != 0 && b.channel == shownPerformanceChannel;
        detailLabel.setColour (juce::Label::textColourId, unreachable ? juce::Colour (0xffef5350) : juce::Colours::white.withAlpha (0.6f));
        detailLabel.setText (unreachable ? "Perf. channel"
                             : b.kind == MidiBinding::Kind::note ? juce::MidiMessage::getMidiNoteName (b.number, true, true, 3)
                                                                 : juce::String(),
                             juce::dontSendNotification);
    }

    void commit()
    {
        const auto kind = (MidiBinding::Kind) juce::jlimit (0, 2, typeBox.getSelectedId() - 1);
        const MidiBinding b { kind, (int) numberSlider.getValue(), juce::jlimit (0, 16, channelBox.getSelectedId() - 1) };

        engine.setBinding (target, b);
        show (b);
    }

    LooperEngine& engine;
    const int target;
    MidiBinding shown;
    int shownPerformanceChannel = 0;

    juce::Label nameLabel, detailLabel;
    juce::ComboBox typeBox, channelBox;
    juce::Slider numberSlider;
    juce::TextButton learnButton { "Learn" };
};

//==============================================================================
// MIDI mapping page: a row per pad, the REC toggle and every mixer control, and named presets for controllers /
// layouts.
class MidiMappingPanel : public juce::Component,
                         private juce::Timer
{
public:
    MidiMappingPanel (LooperEngine& e, MidiPresets& p) : engine (e), presets (p)
    {
        presetLabel.setText ("Preset", juce::dontSendNotification);
        addAndMakeVisible (presetLabel);

        presetBox.setTextWhenNothingSelected ("(none)");
        presetBox.setTextWhenNoChoicesAvailable ("(no presets saved)");
        presetBox.onChange = [this] { loadPreset (presetBox.getText()); };
        addAndMakeVisible (presetBox);

        saveButton.setTooltip ("Overwrite the selected preset with the current mapping");
        saveButton.onClick = [this]
        {
            if (presets.currentName.isEmpty()) saveAs();
            else                               presets.save (presets.currentName, MidiMapping::fromEngine (engine));
        };
        saveAsButton.onClick = [this] { saveAs(); };
        deleteButton.onClick = [this] { deleteSelected(); };

        for (auto* b : { &saveButton, &saveAsButton, &deleteButton })
            addAndMakeVisible (b);

        perfLabel.setText ("Performance channel", juce::dontSendNotification);
        addAndMakeVisible (perfLabel);

        perfBox.addItem ("None", 1);                            // ids = channel + 1
        for (int ch = 1; ch <= 16; ++ch)
            perfBox.addItem ("Ch " + juce::String (ch), ch + 1);
        perfBox.setTooltip ("MIDI on this channel plays the active pad's instrument, and the pad and REC controls "
                            "ignore it. Saved with the preset.");
        perfBox.onChange = [this] { engine.setPerformanceChannel (perfBox.getSelectedId() - 1); };
        perfBox.setSelectedId (engine.getPerformanceChannel() + 1, juce::dontSendNotification);
        addAndMakeVisible (perfBox);

        hintLabel.setText ("Type a note or CC number, or click Learn and press a control on your controller. "
                           "CCs act as buttons (pressed at 64 and above), except for mixer volume and pan, which follow a knob.",
                           juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);

        rows.add (new MappingRow (engine, LooperEngine::recControl, "REC toggle"));
        rows.add (new MappingRow (engine, LooperEngine::recLockControl, "Rec Lock toggle"));
        rows.add (new MappingRow (engine, LooperEngine::playControl, "Play / Stop"));
        for (int i = 0; i < LooperEngine::numPads; ++i)
            rows.add (new MappingRow (engine, i, "Pad " + juce::String (i + 1)));

        rows.add (new MappingRow (engine, LooperEngine::muteAllControl, "Mute all"));
        rows.add (new MappingRow (engine, LooperEngine::unmuteAllControl, "Unmute all"));

        for (int i = 0; i < LooperEngine::numPads; ++i)
            for (auto [c, name] : { std::pair { MixerControl::volume, "volume" }, std::pair { MixerControl::pan, "pan" },
                                    std::pair { MixerControl::mute, "mute" }, std::pair { MixerControl::solo, "solo" } })
                rows.add (new MappingRow (engine, LooperEngine::mixerTarget (i, c), "Pad " + juce::String (i + 1) + " " + name));

        for (auto* r : rows)
            rowHolder.addAndMakeVisible (r);

        viewport.setViewedComponent (&rowHolder, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        fillLabel.setText ("Pads 1-16 from note", juce::dontSendNotification);
        addAndMakeVisible (fillLabel);

        fillSlider.setSliderStyle (juce::Slider::IncDecButtons);
        fillSlider.setRange (0.0, 127.0 - (LooperEngine::numPads - 1), 1.0);
        fillSlider.setValue (36.0, juce::dontSendNotification);
        fillSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 24);
        addAndMakeVisible (fillSlider);

        fillButton.setTooltip ("Map the pads to consecutive notes (36 = C1 is the usual drum-pad layout)");
        fillButton.onClick = [this]
        {
            // Each pad keeps its channel.
            const int first = (int) fillSlider.getValue();
            for (int i = 0; i < LooperEngine::numPads; ++i)
                engine.setBinding (i, MidiBinding::note (first + i, engine.getBinding (i).channel));
        };
        addAndMakeVisible (fillButton);

        refreshPresetList();
        startTimerHz (10);
    }

    ~MidiMappingPanel() override { engine.cancelMidiLearn(); }

    void refreshPresetList()
    {
        presetBox.clear (juce::dontSendNotification);

        const auto names = presets.list();
        presetBox.addItemList (names, 1);
        presetBox.setSelectedItemIndex (names.indexOf (presets.currentName), juce::dontSendNotification);
        deleteButton.setEnabled (presets.currentName.isNotEmpty());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        auto top = r.removeFromTop (28);
        presetLabel.setBounds (top.removeFromLeft (55));
        deleteButton.setBounds (top.removeFromRight (70));
        top.removeFromRight (6);
        saveAsButton.setBounds (top.removeFromRight (85));
        top.removeFromRight (6);
        saveButton.setBounds (top.removeFromRight (60));
        top.removeFromRight (10);
        presetBox.setBounds (top);

        r.removeFromTop (6);
        auto perf = r.removeFromTop (28);
        perfLabel.setBounds (perf.removeFromLeft (140));
        perfBox.setBounds (perf.removeFromLeft (100));

        r.removeFromTop (6);
        hintLabel.setBounds (r.removeFromTop (40));

        auto bottom = r.removeFromBottom (30);
        fillLabel.setBounds (bottom.removeFromLeft (140));
        fillSlider.setBounds (bottom.removeFromLeft (110));
        bottom.removeFromLeft (8);
        fillButton.setBounds (bottom.removeFromLeft (60));
        r.removeFromBottom (8);

        viewport.setBounds (r);
        const int rowH = 32;
        rowHolder.setSize (r.getWidth() - viewport.getScrollBarThickness(), rowH * rows.size());

        for (int i = 0; i < rows.size(); ++i)
            rows[i]->setBounds (0, i * rowH, rowHolder.getWidth(), rowH);
    }

private:
    void timerCallback() override
    {
        for (auto* r : rows)
            r->refresh();

        // A preset load changes it behind our back.
        perfBox.setSelectedId (engine.getPerformanceChannel() + 1, juce::dontSendNotification);
    }

    void loadPreset (const juce::String& name)
    {
        MidiMapping m;
        if (name.isEmpty() || ! presets.load (name, m))
            return;

        engine.cancelMidiLearn();
        m.applyTo (engine);
        deleteButton.setEnabled (true);
    }

    void saveAs()
    {
        auto* w = new juce::AlertWindow ("Save MIDI preset",
                                         "Name this mapping (e.g. after the controller or layout):",
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", presets.currentName);
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<MidiMappingPanel> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            if (safe->presets.save (w->getTextEditorContents ("name"), MidiMapping::fromEngine (safe->engine)).isNotEmpty())
                safe->refreshPresetList();
        }), true);
    }

    void deleteSelected()
    {
        const auto name = presets.currentName;
        if (name.isEmpty())
            return;

        juce::Component::SafePointer<MidiMappingPanel> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (
                                          juce::MessageBoxIconType::QuestionIcon, "Delete preset",
                                          "Delete the MIDI preset \"" + name + "\"? The current mapping stays as it is.",
                                          "Delete", "Cancel", this),
                                      [safe, name] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            safe->presets.remove (name);
            safe->refreshPresetList();
        });
    }

    LooperEngine& engine;
    MidiPresets& presets;

    juce::Label presetLabel, hintLabel, fillLabel, perfLabel;
    juce::ComboBox presetBox, perfBox;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save As..." }, deleteButton { "Delete" }, fillButton { "Fill" };
    juce::Slider fillSlider;

    juce::Viewport viewport;
    juce::Component rowHolder;
    juce::OwnedArray<MappingRow> rows;      // declared after rowHolder so they are removed from it first
};

//==============================================================================
// Per-pad outputs (Settings > Audio, JACK only; see OutputBuses.h), kept in ~/.config/Indigo/Outputs.xml.
struct OutputSettings
{
    static juce::File file() { return MidiPresets::folder().getChildFile ("Outputs.xml"); }

    static bool perPad()                { return load()->getBoolAttribute ("perPad", false); }
    static bool preFader()              { return load()->getBoolAttribute ("preFader", false); }
    static bool setPerPad (bool on)     { return set ("perPad", on); }
    static bool setPreFader (bool on)   { return set ("preFader", on); }

private:
    static std::unique_ptr<juce::XmlElement> load()
    {
        auto xml = juce::XmlDocument::parse (file());
        if (xml == nullptr || ! xml->hasTagName ("Outputs"))
            xml = std::make_unique<juce::XmlElement> ("Outputs");
        return xml;
    }

    static bool set (const char* name, bool value)
    {
        auto xml = load();
        xml->setAttribute (name, value);
        return MidiPresets::folder().createDirectory() && xml->writeTo (file());
    }
};

//==============================================================================
// Audio page: JUCE's device selector (driver, device, sample rate, buffer size, channels), and per-pad outputs under
// JACK. Changes apply at once; MainComponent hears about them from the device manager and adapts (see audioDeviceChanged).
class AudioSettingsPanel : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit AudioSettingsPanel (juce::AudioDeviceManager& dm)
        : deviceManager (dm),
          selector (dm,
                    0, 2,           // inputs: none (no recording) up to one stereo pair
                    2, 2,           // outputs: one stereo pair
                    false, false,   // MIDI inputs are all enabled automatically; no MIDI output
                    true,           // show channels as stereo pairs
                    false)          // show the advanced options (sample rate, buffer size) directly
    {
        addAndMakeVisible (selector);

        perPadButton.setTooltip ("Adds an output pair for each pad (pad_01_L / pad_01_R ...) to patch into a DAW, so a "
                                 "performance can be recorded with the pads on separate tracks. The pads still play on "
                                 "main as well. Changing this reopens the audio device.");
        perPadButton.setToggleState (OutputBuses::perPad.load(), juce::dontSendNotification);
        perPadButton.onClick = [this] { setPerPad (perPadButton.getToggleState()); };
        addChildComponent (perPadButton);

        tapCombo.addItem ("Post-fader", 1);
        tapCombo.addItem ("Pre-fader", 2);
        tapCombo.setTooltip ("Post-fader: each pad as you hear it, after its effects, fader, pan and mute / solo.\n"
                             "Pre-fader: after its effects only, for raw tracks to mix afterwards.");
        tapCombo.setSelectedId (OutputBuses::padsPreFader.load() ? 2 : 1, juce::dontSendNotification);
        tapCombo.onChange = [this]
        {
            const bool pre = tapCombo.getSelectedId() == 2;
            OutputBuses::padsPreFader.store (pre);
            OutputSettings::setPreFader (pre);
        };
        addChildComponent (tapCombo);

        hintLabel.setText ("Changing the sample rate restarts the clock and cancels any recording in progress. "
                           "The Rec latency slider is reset to what the new device reports.",
                           juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);

        deviceManager.addChangeListener (this);
        showOutputOptions();
    }

    ~AudioSettingsPanel() override { deviceManager.removeChangeListener (this); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        hintLabel.setBounds (r.removeFromBottom (40));

        auto row = r.removeFromBottom (28);
        perPadButton.setBounds (row.removeFromLeft (160));
        tapCombo.setBounds (row.removeFromLeft (130).reduced (0, 2));
        r.removeFromBottom (8);

        selector.setBounds (r);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { showOutputOptions(); }

    // Per-pad outputs are JACK ports, so they're only offered with a JACK device.
    void showOutputOptions()
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        const bool jack = dev != nullptr && dev->getTypeName() == "JACK";
        perPadButton.setVisible (jack);
        tapCombo.setVisible (jack);
        tapCombo.setEnabled (perPadButton.getToggleState());
    }

    // The ports are made when the device is created, so it is closed and opened again with the new set.
    void setPerPad (bool on)
    {
        OutputSettings::setPerPad (on);
        OutputBuses::perPad.store (on);
        OutputBuses::setJackPortNames();

        deviceManager.closeAudioDevice();
        deviceManager.restartLastAudioDevice();
        showOutputOptions();
    }

    juce::AudioDeviceManager& deviceManager;
    juce::AudioDeviceSelectorComponent selector;
    juce::ToggleButton perPadButton { "Per-pad outputs" };
    juce::ComboBox tapCombo;
    juce::Label hintLabel;
};

//==============================================================================
// MIDI sync with a DAW (Settings > Sync), kept in ~/.config/Indigo/Sync.xml.
struct SyncSettings
{
    static juce::File file() { return MidiPresets::folder().getChildFile ("Sync.xml"); }

    static bool clockOn()                       { return load()->getBoolAttribute ("clock", false); }
    static bool mtcOn()                         { return load()->getBoolAttribute ("mtc", false); }
    static int mtcRate()                        { return juce::jlimit (0, 3, load()->getIntAttribute ("mtcRate", Mtc::fps30)); }
    static double offsetMs()                    { return load()->getDoubleAttribute ("clockOffsetMs", 0.0); }
    static bool setClockOn (bool on)            { return set ("clock", on); }
    static bool setMtcOn (bool on)              { return set ("mtc", on); }
    static bool setMtcRate (int rate)           { return set ("mtcRate", rate); }
    static bool setOffsetMs (double ms)         { return set ("clockOffsetMs", ms); }

    // Puts the saved choices into effect: the port is open while either is on. False if it couldn't be created.
    static bool apply (LooperEngine& engine)
    {
        const bool clock = clockOn(), mtc = mtcOn();
        auto& out = engine.getSyncOutput();

        out.setOpen (clock || mtc);
        engine.setClockOn (clock);
        engine.setMtcRate (mtc ? mtcRate() : -1);
        return out.isOpen() == (clock || mtc);
    }

private:
    static std::unique_ptr<juce::XmlElement> load()
    {
        auto xml = juce::XmlDocument::parse (file());
        if (xml == nullptr || ! xml->hasTagName ("Sync"))
            xml = std::make_unique<juce::XmlElement> ("Sync");
        return xml;
    }

    template <typename Value>
    static bool set (const char* name, Value value)
    {
        auto xml = load();
        xml->setAttribute (name, value);
        return MidiPresets::folder().createDirectory() && xml->writeTo (file());
    }
};

//==============================================================================
// Sync page: MIDI clock and MTC out on Indigo's own MIDI port, and how much to move them so the DAW lines up.
class SyncSettingsPanel : public juce::Component
{
public:
    explicit SyncSettingsPanel (LooperEngine& e) : engine (e)
    {
        clockButton.setTooltip ("Creates a MIDI output named \"Indigo\". Connect it to your DAW and set the DAW to follow "
                                "external MIDI clock.");
        clockButton.setToggleState (SyncSettings::clockOn(), juce::dontSendNotification);
        clockButton.onClick = [this] { outputsChanged(); };
        addAndMakeVisible (clockButton);

        mtcButton.setTooltip ("Creates a MIDI output named \"Indigo\". Connect it to your DAW and set the DAW to follow "
                              "external MTC at the same frame rate.");
        mtcButton.setToggleState (SyncSettings::mtcOn(), juce::dontSendNotification);
        mtcButton.onClick = [this] { outputsChanged(); };
        addAndMakeVisible (mtcButton);

        mtcRateBox.addItem ("24 fps", Mtc::fps24 + 1);
        mtcRateBox.addItem ("25 fps", Mtc::fps25 + 1);
        mtcRateBox.addItem ("29.97 fps drop", Mtc::fps2997Drop + 1);
        mtcRateBox.addItem ("30 fps", Mtc::fps30 + 1);
        mtcRateBox.setSelectedId (SyncSettings::mtcRate() + 1, juce::dontSendNotification);
        mtcRateBox.onChange = [this] { outputsChanged(); };
        addAndMakeVisible (mtcRateBox);

        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe57373));
        addAndMakeVisible (statusLabel);

        offsetLabel.setText ("Sync offset", juce::dontSendNotification);
        addAndMakeVisible (offsetLabel);

        offsetSlider.setRange (-100.0, 100.0, 1.0);
        offsetSlider.setTextValueSuffix (" ms");
        offsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        offsetSlider.setDoubleClickReturnValue (true, 0.0);
        offsetSlider.setTooltip ("Positive sends the sync later. If the DAW records the pads late against its grid, "
                                 "raise it; early, lower it.");
        offsetSlider.setValue (SyncSettings::offsetMs(), juce::dontSendNotification);
        offsetSlider.onValueChange = [this]
        {
            engine.setSyncOffsetMs (offsetSlider.getValue());
            SyncSettings::setOffsetMs (offsetSlider.getValue());
        };
        addAndMakeVisible (offsetSlider);

        hintLabel.setText ("MIDI clock sends Start when the transport starts (always from bar 1), 24 clocks per beat while "
                           "it plays, and Stop when it stops. MTC runs from 00:00:00:00 on bar 1 and counts real time, so "
                           "tempo changes don't move it. Both are timed for when you hear the audio "
                           "(the output half of Rec latency), plus the offset.",
                           juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        auto row = r.removeFromTop (28);
        clockButton.setBounds (row.removeFromLeft (160));
        statusLabel.setBounds (row);

        r.removeFromTop (6);
        row = r.removeFromTop (28);
        mtcButton.setBounds (row.removeFromLeft (160));
        mtcRateBox.setBounds (row.removeFromLeft (160));

        r.removeFromTop (6);
        row = r.removeFromTop (28);
        offsetLabel.setBounds (row.removeFromLeft (110));
        offsetSlider.setBounds (row.removeFromLeft (360));

        r.removeFromTop (10);
        hintLabel.setBounds (r.removeFromTop (80));
    }

private:
    void outputsChanged()
    {
        SyncSettings::setClockOn (clockButton.getToggleState());
        SyncSettings::setMtcOn (mtcButton.getToggleState());
        SyncSettings::setMtcRate (mtcRateBox.getSelectedId() - 1);

        const bool ok = SyncSettings::apply (engine);
        statusLabel.setText (ok ? "" : "Couldn't create the MIDI port", juce::dontSendNotification);

        if (! ok)
        {
            clockButton.setToggleState (false, juce::dontSendNotification);
            mtcButton.setToggleState (false, juce::dontSendNotification);
            SyncSettings::setClockOn (false);
            SyncSettings::setMtcOn (false);
            SyncSettings::apply (engine);
        }
    }

    LooperEngine& engine;
    juce::ToggleButton clockButton { "Send MIDI clock" }, mtcButton { "Send MTC" };
    juce::ComboBox mtcRateBox;
    juce::Label statusLabel, offsetLabel, hintLabel;
    juce::Slider offsetSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
};

//==============================================================================
// Browser page: the folder the sample browser opens in at startup (and goes to on Home).
class BrowserSettingsPanel : public juce::Component
{
public:
    explicit BrowserSettingsPanel (std::function<juce::File()> currentFolderFn) : currentFolder (std::move (currentFolderFn))
    {
        startLabel.setText ("Starting folder", juce::dontSendNotification);
        addAndMakeVisible (startLabel);

        pathEditor.setTooltip ("Type a path (~ works) and press Enter, or use Browse...");
        pathEditor.onReturnKey = [this] { commitTyped(); };
        pathEditor.onFocusLost = [this] { commitTyped(); };
        pathEditor.onEscapeKey = [this] { showSaved(); };
        addAndMakeVisible (pathEditor);

        browseButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Starting folder", BrowserSettings::startFolder());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc) { set (fc.getResult()); });
        };
        currentButton.setTooltip ("Use the folder the browser is showing now");
        currentButton.onClick = [this] { set (currentFolder()); };
        resetButton.setTooltip ("Back to " + BrowserSettings::defaultFolder().getFullPathName());
        resetButton.onClick = [this] { set (BrowserSettings::defaultFolder()); };

        for (auto* b : { &browseButton, &currentButton, &resetButton })
            addAndMakeVisible (b);

        reopenButton.setTooltip ("Start in the folder the browser was showing when Indigo last closed. "
                                 "Home still goes to the starting folder.");
        reopenButton.onClick = [this] { BrowserSettings::setReopenLastFolder (reopenButton.getToggleState()); };
        addAndMakeVisible (reopenButton);

        hintLabel.setText ("Home goes back here, and the browser opens here when Indigo starts unless the box above is ticked. You can also "
                           "right-click a folder in the browser and choose \"Set as starting folder\".",
                           juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);

        showSaved();
    }

    void visibilityChanged() override { if (isVisible()) showSaved(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        auto row = r.removeFromTop (28);
        startLabel.setBounds (row.removeFromLeft (110));
        pathEditor.setBounds (row);

        r.removeFromTop (6);
        auto buttons = r.removeFromTop (28).withTrimmedLeft (110);
        browseButton.setBounds (buttons.removeFromLeft (90));
        buttons.removeFromLeft (6);
        currentButton.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (6);
        resetButton.setBounds (buttons.removeFromLeft (70));

        r.removeFromTop (6);
        reopenButton.setBounds (r.removeFromTop (28).withTrimmedLeft (110));

        r.removeFromTop (10);
        hintLabel.setBounds (r.removeFromTop (40));
    }

private:
    void showSaved()
    {
        pathEditor.setText (BrowserSettings::startFolder().getFullPathName(), juce::dontSendNotification);
        reopenButton.setToggleState (BrowserSettings::reopensLastFolder(), juce::dontSendNotification);
    }

    void set (const juce::File& dir)
    {
        if (dir.isDirectory())
            BrowserSettings::setStartFolder (dir);
        showSaved();
    }

    void commitTyped()
    {
        const auto text = pathEditor.getText().trim();
        if (juce::File::isAbsolutePath (text))
            set (juce::File (text));
        else
            showSaved();
    }

    std::function<juce::File()> currentFolder;
    juce::Label startLabel, hintLabel;
    juce::TextEditor pathEditor;
    juce::ToggleButton reopenButton { "Reopen the last folder I was browsing instead" };
    juce::TextButton browseButton { "Browse..." }, currentButton { "Use current folder" }, resetButton { "Reset" };
    std::unique_ptr<juce::FileChooser> chooser;
};

//==============================================================================
// Settings window (Edit > Settings). Closing hides it; onClosed lets the owner persist the settings.
class SettingsWindow : public juce::DocumentWindow
{
public:
    SettingsWindow (LooperEngine& e, MidiPresets& p, juce::AudioDeviceManager& dm, std::function<juce::File()> browserFolder)
        : DocumentWindow ("Settings", juce::Colour (0xff1b1b1b), DocumentWindow::closeButton), engine (e)
    {
        auto* tabs = new juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop);
        tabs->addTab ("MIDI Mapping", juce::Colour (0xff1b1b1b), new MidiMappingPanel (e, p), true);
        tabs->addTab ("Audio", juce::Colour (0xff1b1b1b), new AudioSettingsPanel (dm), true);
        tabs->addTab ("Sync", juce::Colour (0xff1b1b1b), new SyncSettingsPanel (e), true);
        tabs->addTab ("Browser", juce::Colour (0xff1b1b1b), new BrowserSettingsPanel (std::move (browserFolder)), true);
        tabs->setSize (700, 700);

        setUsingNativeTitleBar (true);
        setContentOwned (tabs, true);
        setResizable (true, false);
        setResizeLimits (640, 400, 1200, 1400);
    }

    std::function<void()> onClosed;

    // Re-reads the preset list (it may have changed on disk) and brings the window up.
    void open (juce::Component* centreOn)
    {
        if (auto* tabs = dynamic_cast<juce::TabbedComponent*> (getContentComponent()))
            if (auto* panel = dynamic_cast<MidiMappingPanel*> (tabs->getTabContentComponent (0)))
                panel->refreshPresetList();

        if (! isVisible())
            centreAroundComponent (centreOn, getWidth(), getHeight());

        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        engine.cancelMidiLearn();
        setVisible (false);
        if (onClosed) onClosed();
    }

private:
    LooperEngine& engine;
};
