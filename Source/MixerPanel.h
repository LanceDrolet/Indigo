#pragma once
#include <JuceHeader.h>
#include <array>
#include "LooperEngine.h"
#include "MidiPresets.h"

//==============================================================================
// The mixer's own setting (Normalise on load), kept in ~/.config/Indigo/Mixer.xml. Levels, pan, mute and solo are
// saved in sessions instead.
struct MixerSettings
{
    static juce::File file() { return MidiPresets::folder().getChildFile ("Mixer.xml"); }

    static bool normaliseOnLoad()
    {
        auto xml = juce::XmlDocument::parse (file());
        return xml != nullptr && xml->getBoolAttribute ("normaliseOnLoad", false);
    }

    static bool setNormaliseOnLoad (bool on)
    {
        auto xml = juce::XmlDocument::parse (file());
        if (xml == nullptr || ! xml->hasTagName ("Mixer"))
            xml = std::make_unique<juce::XmlElement> ("Mixer");

        xml->setAttribute ("normaliseOnLoad", on);
        return MidiPresets::folder().createDirectory() && xml->writeTo (file());
    }
};

//==============================================================================
// A JUCE control that can be mapped to MIDI from a right-click (MidiLearnMenu). A right-click never moves or clicks
// the control itself; while it is learning it gets a red outline.
template <class Base>
class Learnable : public Base
{
public:
    using Base::Base;

    std::function<void()> onRightClick;

    void setLearning (bool on)
    {
        if (on != learning)
        {
            learning = on;
            this->repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())       Base::mouseDown (e);
        else if (onRightClick)            onRightClick();
    }

    void mouseDrag (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) Base::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override     { if (! e.mods.isPopupMenu()) Base::mouseUp (e); }

    void paintOverChildren (juce::Graphics& g) override
    {
        Base::paintOverChildren (g);

        if (learning)
        {
            g.setColour (juce::Colour (0xffe53935));
            g.drawRoundedRectangle (this->getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
        }
    }

private:
    bool learning = false;
};

// The right-click menu of a mixer control: what it's mapped to, MIDI Learn, and clearing the mapping.
inline void showMidiLearnMenu (LooperEngine& engine, int target, juce::Component& control)
{
    const auto binding = engine.getBinding (target);
    const bool learning = engine.getLearnTarget() == target;

    juce::PopupMenu m;
    m.addSectionHeader ("MIDI: " + (binding.kind == MidiBinding::Kind::none ? juce::String ("not mapped") : binding.describe()));
    m.addItem (1, learning ? "Cancel MIDI Learn"
                           : LooperEngine::isContinuous (target) ? "MIDI Learn (then move a knob or fader)"
                                                                 : "MIDI Learn (then press a button or pad)");
    m.addItem (2, "Clear MIDI mapping", binding.kind != MidiBinding::Kind::none);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&control), [&engine, target, learning] (int result)
    {
        if (result == 1)
        {
            if (learning) engine.cancelMidiLearn();
            else          engine.startMidiLearn (target);
        }
        else if (result == 2)
        {
            engine.setBinding (target, {});
        }
    });
}

//==============================================================================
// A stereo peak meter, -60 .. +6 dB. Falls back at about 40 dB a second.
class LevelMeter : public juce::Component
{
public:
    void push (float left, float right)
    {
        bool changed = false;

        for (auto [level, peak] : { std::pair { &levels[0], left }, std::pair { &levels[1], right } })
        {
            const float next = juce::jmax (peak, *level * 0.86f);
            const float shown = next < 0.001f ? 0.0f : next;
            changed = changed || std::abs (shown - *level) > 1.0e-4f;
            *level = shown;
        }

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float w = (r.getWidth() - 1.0f) * 0.5f;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = juce::Rectangle<float> (r.getX() + (float) ch * (w + 1.0f), r.getY(), w, r.getHeight());
            g.setColour (juce::Colour (0xff0d0d0d));
            g.fillRect (bar);

            const float db = juce::Decibels::gainToDecibels (levels[(size_t) ch], -100.0f);
            const float frac = juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
            if (frac <= 0.0f)
                continue;

            auto lit = bar.withTop (bar.getBottom() - bar.getHeight() * frac);
            g.setColour (db > 0.0f ? juce::Colour (0xffe53935) : db > -6.0f ? juce::Colour (0xffffd54f) : juce::Colour (0xff66bb6a));
            g.fillRect (lit);
        }

        // 0 dB tick
        const float zero = r.getBottom() - r.getHeight() * (0.0f - minDb) / (maxDb - minDb);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawHorizontalLine ((int) zero, r.getX(), r.getRight());
    }

private:
    static constexpr float minDb = -60.0f, maxDb = 6.0f;
    std::array<float, 2> levels {};
};

//==============================================================================
// One pad's channel strip: name, pan knob, fader with meter and level readout, mute and solo.
class MixerStrip : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    MixerStrip (LooperEngine& e, int padIndex) : engine (e), pad (padIndex)
    {
        numberLabel.setText (juce::String (pad + 1), juce::dontSendNotification);
        numberLabel.setJustificationType (juce::Justification::centred);
        numberLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (11.0f));
        nameLabel.setMinimumHorizontalScale (0.6f);
        nameLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
        nameLabel.setBorderSize ({});

        for (auto* l : { &numberLabel, &nameLabel })
        {
            l->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (l);
        }

        pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.0);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setPopupDisplayEnabled (true, true, nullptr);
        pan.textFromValueFunction = [] (double v) { return "Pan " + Mixer::panText ((float) v); };
        pan.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3d3d3d));
        pan.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff4fc3f7));
        pan.onValueChange = [this] { engine.setPan (pad, (float) pan.getValue()); };
        pan.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::mixerTarget (pad, MixerControl::pan), pan); };
        addAndMakeVisible (pan);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (0.0, 1.0, 0.0);
        fader.setDoubleClickReturnValue (true, Mixer::unityFader());
        fader.setColour (juce::Slider::trackColourId, fader.findColour (juce::Slider::backgroundColourId).brighter(0.52f));
        fader.onValueChange = [this] { engine.setFader (pad, (float) fader.getValue()); };
        fader.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::mixerTarget (pad, MixerControl::volume), fader); };
        addAndMakeVisible (fader);

        addAndMakeVisible (meter);

        levelLabel.setJustificationType (juce::Justification::centred);
        levelLabel.setFont (juce::FontOptions (11.0f));
        levelLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
        levelLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (levelLabel);

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            addAndMakeVisible (b);
        }

        mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb300));
        solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff66bb6a));
        mute.onClick = [this] { engine.setMute (pad, mute.getToggleState()); };
        solo.onClick = [this] { engine.setSolo (pad, solo.getToggleState()); };
        mute.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::mixerTarget (pad, MixerControl::mute), mute); };
        solo.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::mixerTarget (pad, MixerControl::solo), solo); };

        setPadName ({});
        refresh();
    }

    void setPadName (const juce::String& name)
    {
        if (name == shownName && nameLabel.getText().isNotEmpty())
            return;

        shownName = name;
        nameLabel.setText (name.isNotEmpty() ? name : juce::String ("-"), juce::dontSendNotification);
        setTooltip ("Pad " + juce::String (pad + 1) + (name.isNotEmpty() ? ": " + name : juce::String()));
    }

    // Timer: follows the engine (MIDI moves things too), the meter, and MIDI learn.
    void refresh()
    {
        auto follow = [] (juce::Slider& s, float v)
        {
            if (! s.isMouseButtonDown() && std::abs (s.getValue() - (double) v) > 1.0e-4)
                s.setValue (v, juce::dontSendNotification);
        };

        follow (fader, engine.getFader (pad));
        follow (pan, engine.getPan (pad));
        mute.setToggleState (engine.isMuted (pad), juce::dontSendNotification);
        solo.setToggleState (engine.isSoloed (pad), juce::dontSendNotification);

        const auto level = Mixer::faderText ((float) fader.getValue()) + " dB";
        if (level != levelLabel.getText())
            levelLabel.setText (level, juce::dontSendNotification);

        meter.push (engine.takePeak (pad, 0), engine.takePeak (pad, 1));

        const int learning = engine.getLearnTarget();
        fader.setLearning (learning == LooperEngine::mixerTarget (pad, MixerControl::volume));
        pan.setLearning (learning == LooperEngine::mixerTarget (pad, MixerControl::pan));
        mute.setLearning (learning == LooperEngine::mixerTarget (pad, MixerControl::mute));
        solo.setLearning (learning == LooperEngine::mixerTarget (pad, MixerControl::solo));

        updateTooltip (fader, MixerControl::volume, "Level (double-click: 0 dB)");
        updateTooltip (pan, MixerControl::pan, "Pan (double-click: centre)");
        updateTooltip (mute, MixerControl::mute, "Mute (the pad keeps playing silently)");
        updateTooltip (solo, MixerControl::solo, "Solo (more than one pad can be soloed)");
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff1e1e1e));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f, 0.0f), 4.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 4);
        numberLabel.setBounds (r.removeFromTop (16));
        nameLabel.setBounds (r.removeFromTop (14));

        auto buttons = r.removeFromBottom (20);
        const int half = (buttons.getWidth() - 2) / 2;
        mute.setBounds (buttons.removeFromLeft (half));
        solo.setBounds (buttons.removeFromRight (half));
        r.removeFromBottom (3);

        levelLabel.setBounds (r.removeFromBottom (14));
        pan.setBounds (r.removeFromTop (juce::jmin (36, r.getWidth())));
        r.removeFromTop (2);

        meter.setBounds (r.removeFromRight (9).reduced (0, 6));
        fader.setBounds (r);
    }

private:
    template <class Control>
    void updateTooltip (Control& c, MixerControl control, const juce::String& what)
    {
        const auto b = engine.getBinding (LooperEngine::mixerTarget (pad, control));
        const auto text = what + "\nMIDI: " + (b.kind == MidiBinding::Kind::none ? juce::String ("not mapped") : b.describe())
                        + " (right-click to map)";
        if (text != c.getTooltip())
            c.setTooltip (text);
    }

    LooperEngine& engine;
    const int pad;
    juce::String shownName;

    juce::Label numberLabel, nameLabel, levelLabel;
    Learnable<juce::Slider> pan, fader;
    LevelMeter meter;
    Learnable<juce::TextButton> mute { "M" }, solo { "S" };
};

//==============================================================================
// The mixer: Mute all / Unmute all and Normalise on load, then a strip per pad.
class MixerPanel : public juce::Component
{
public:
    explicit MixerPanel (LooperEngine& e) : engine (e)
    {
        muteAllButton.setTooltip ("Mute every pad");
        muteAllButton.onClick = [this] { engine.muteAll(); };
        muteAllButton.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::muteAllControl, muteAllButton); };

        unmuteAllButton.setTooltip ("Unmute every pad and clear every solo");
        unmuteAllButton.onClick = [this] { engine.unmuteAll(); };
        unmuteAllButton.onRightClick = [this] { showMidiLearnMenu (engine, LooperEngine::unmuteAllControl, unmuteAllButton); };

        normaliseButton.setTooltip ("Bring each sound file to the same loudness (RMS, peaks kept under -1 dB) as it is loaded "
                                    "onto a pad. Recordings are left as they are.");
        normaliseButton.setToggleState (MixerSettings::normaliseOnLoad(), juce::dontSendNotification);
        normaliseButton.onClick = [this] { MixerSettings::setNormaliseOnLoad (normaliseButton.getToggleState()); };

        hintLabel.setText ("Right-click any control to map it to MIDI.", juce::dontSendNotification);
        hintLabel.setFont (juce::FontOptions (12.0f));
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
        hintLabel.setJustificationType (juce::Justification::topLeft);

        for (juce::Component* c : { (juce::Component*) &muteAllButton, (juce::Component*) &unmuteAllButton,
                                    (juce::Component*) &normaliseButton, (juce::Component*) &hintLabel })
            addAndMakeVisible (c);

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            strips[(size_t) i] = std::make_unique<MixerStrip> (engine, i);
            addAndMakeVisible (*strips[(size_t) i]);
        }
    }

    MixerStrip& strip (int pad) { return *strips[(size_t) pad]; }

    void refresh()
    {
        for (auto& s : strips)
            s->refresh();

        const int learning = engine.getLearnTarget();
        muteAllButton.setLearning (learning == LooperEngine::muteAllControl);
        unmuteAllButton.setLearning (learning == LooperEngine::unmuteAllControl);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff161616)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);

        auto tools = r.removeFromLeft (124);
        muteAllButton.setBounds (tools.removeFromTop (26));
        tools.removeFromTop (6);
        unmuteAllButton.setBounds (tools.removeFromTop (26));
        tools.removeFromTop (12);
        normaliseButton.setBounds (tools.removeFromTop (40));
        tools.removeFromTop (6);
        hintLabel.setBounds (tools.removeFromTop (48));
        r.removeFromLeft (6);

        const int w = r.getWidth() / LooperEngine::numPads;
        for (auto& s : strips)
            s->setBounds (r.removeFromLeft (w));
    }

private:
    LooperEngine& engine;
    Learnable<juce::TextButton> muteAllButton { "Mute all" }, unmuteAllButton { "Unmute all" };
    juce::ToggleButton normaliseButton { "Normalise on load" };
    juce::Label hintLabel;
    std::array<std::unique_ptr<MixerStrip>, LooperEngine::numPads> strips;
};
