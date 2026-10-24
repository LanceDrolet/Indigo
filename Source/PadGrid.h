#pragma once
#include <JuceHeader.h>
#include "LooperEngine.h"
#include <array>
#include <functional>

//==============================================================================
// One square pad. Accepts files dragged from the OS and from the in-app file tree.
// Right-click for the pad mode menu (mode, loop length, choke group, instrument & effects, clear).
class PadComponent : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     public juce::DragAndDropTarget
{
public:
    static constexpr const char* treeDragDescription = "audio-file";
    static constexpr int lengthChoices[7] = { 1, 2, 4, 8, 16, 32, 64 };   // loop lengths, in beats
    static constexpr int chokeMenuBase = 30;                               // menu ids 30 (none) .. 38

    explicit PadComponent (int idx) : index (idx) {}

    std::function<void (int)> onPressed, onReleased, onCleared, onEffects;
    std::function<void (int, const juce::File&)> onFileAssigned;
    std::function<void (int, PadMode)> onModeChanged;
    std::function<void (int, int)> onLengthChanged;      // (pad, beats); 0 = auto
    std::function<void (int, int)> onChokeChanged;       // (pad, group); 0 = none

    void setClipName (const juce::String& n) { clipName = n; repaint(); }
    void setMode (PadMode m)                 { mode = m; repaint(); }
    void setLengthBeats (int b)              { lengthBeats = b; repaint(); }
    void setChokeGroup (int g)               { chokeGroup = g; repaint(); }
    void setHasEffects (bool on)             { if (on != hasEffects) { hasEffects = on; repaint(); } }
    void setInstrumentName (const juce::String& n) { if (n != instrumentName) { instrumentName = n; repaint(); } }
    void setStatus (PadStatus s)             { if (s != status) { status = s; repaint(); } }
    void setRecordMode (bool on)             { if (on != recordMode) { recordMode = on; repaint(); } }
    void setActive (bool on)                 { if (on != active) { active = on; repaint(); } }
    void setSilenced (bool on)               { if (on != silenced) { silenced = on; repaint(); } }   // muted or soloed out

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        const auto orange = juce::Colour (0xffe8a33d);

        const auto red = juce::Colour (0xffe53935);

        auto fill = clipName.isEmpty() ? juce::Colour (0xff2b2b2b) : juce::Colour (0xff3a4a6b);
        if (status == PadStatus::playing || status == PadStatus::overdubQueued || status == PadStatus::overdubbing)
            fill = orange;
        if (status == PadStatus::recording) fill = juce::Colour (0xffb71c1c);
        if (dragHover)                      fill = juce::Colour (0xff4caf50);

        g.setColour (fill);
        g.fillRoundedRectangle (r, 8.0f);

        if (status == PadStatus::queued)                                   // waiting for the next grid line
        {
            g.setColour (orange);
            g.drawRoundedRectangle (r.reduced (1.5f), 8.0f, 3.0f);
        }
        else if (status == PadStatus::recQueued || status == PadStatus::overdubQueued
                 || status == PadStatus::recording || status == PadStatus::overdubbing)
        {
            g.setColour (red);
            g.drawRoundedRectangle (r.reduced (1.5f), 8.0f, 3.0f);
        }
        else
        {
            g.setColour (recordMode ? red.withAlpha (0.7f) : juce::Colours::white.withAlpha (0.25f));
            g.drawRoundedRectangle (r, 8.0f, recordMode ? 2.0f : 1.5f);
        }

        if (active)     // the input goes through this pad's effects, and the performance channel plays its instrument
        {
            g.setColour (juce::Colour (0xff4fc3f7));
            g.drawRoundedRectangle (r.reduced (5.0f), 6.0f, 1.5f);
        }

        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (12.0f);
        auto inner = r.reduced (6.0f).toNearestInt();
        g.drawText (juce::String (index + 1), inner, juce::Justification::topLeft);

        if (chokeGroup > 0)     // a coloured tag next to the pad number, the same colour across the group
        {
            auto tagArea = juce::Rectangle<float> ((float) inner.getX() + 20.0f, (float) inner.getY(), 26.0f, 15.0f);
            g.setColour (chokeColour (chokeGroup));
            g.fillRoundedRectangle (tagArea, 4.0f);
            g.setColour (juce::Colours::black);
            g.drawText ("C" + juce::String (chokeGroup), tagArea, juce::Justification::centred);
            g.setColour (juce::Colours::white.withAlpha (0.5f));
        }
        g.drawText (modeLabel(), inner, juce::Justification::bottomRight);

        auto tagX = (float) inner.getX();

        if (instrumentName.isNotEmpty())
        {
            auto tagArea = juce::Rectangle<float> (tagX, (float) inner.getBottom() - 15.0f, 32.0f, 15.0f);
            g.setColour (juce::Colour (0xffba68c8));
            g.fillRoundedRectangle (tagArea, 4.0f);
            g.setColour (juce::Colours::black);
            g.drawText ("INST", tagArea, juce::Justification::centred);
            tagX += 36.0f;
        }

        if (hasEffects)
        {
            auto tagArea = juce::Rectangle<float> (tagX, (float) inner.getBottom() - 15.0f, 24.0f, 15.0f);
            g.setColour (juce::Colour (0xff4fc3f7));
            g.fillRoundedRectangle (tagArea, 4.0f);
            g.setColour (juce::Colours::black);
            g.drawText ("FX", tagArea, juce::Justification::centred);
        }

        const char* tag = status == PadStatus::recQueued     ? "REC ARMED"
                        : status == PadStatus::recording     ? "RECORDING"
                        : status == PadStatus::overdubQueued ? "OVERDUB ARMED"
                        : status == PadStatus::overdubbing   ? "OVERDUB" : nullptr;
        if (tag != nullptr)
        {
            g.setColour (juce::Colours::white);
            g.drawText (tag, inner, juce::Justification::topRight);
        }

        if (silenced)   // muted, or another pad is soloed: dimmed, with a tag (it keeps playing silently)
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillRoundedRectangle (r, 8.0f);

            auto tagArea = juce::Rectangle<float> ((float) inner.getRight() - 42.0f, (float) inner.getBottom() - 32.0f, 42.0f, 15.0f);
            g.setColour (juce::Colour (0xffffb300));
            g.fillRoundedRectangle (tagArea, 4.0f);
            g.setColour (juce::Colours::black);
            g.drawText ("MUTED", tagArea, juce::Justification::centred);
        }

        // An empty pad with an instrument shows the instrument's name: record it to fill the pad.
        g.setColour (clipName.isEmpty() && instrumentName.isNotEmpty() ? juce::Colour (0xffe1bee7) : juce::Colours::white);
        g.setFont (14.0f);
        g.drawFittedText (clipName.isNotEmpty() ? clipName : instrumentName.isNotEmpty() ? instrumentName : juce::String ("drop sample"),
                          r.reduced (8.0f).toNearestInt(), juce::Justification::centred, 2);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showModeMenu(); return; }

        pressed = true;
        if (onPressed) onPressed (index);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! pressed) return;
        pressed = false;
        if (onReleased) onReleased (index);
    }

    //==== OS file drops ====
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (isAudioFile (juce::File (f))) return true;
        return false;
    }
    void fileDragEnter (const juce::StringArray&, int, int) override { setHover (true); }
    void fileDragExit  (const juce::StringArray&) override           { setHover (false); }
    void filesDropped  (const juce::StringArray& files, int, int) override
    {
        setHover (false);
        for (auto& f : files)
            if (isAudioFile (juce::File (f))) { assign (juce::File (f)); break; }
    }

    //==== Drags from the in-app FileTreeComponent ====
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return d.description.toString() == treeDragDescription;
    }
    void itemDragEnter (const SourceDetails&) override { setHover (true); }
    void itemDragExit  (const SourceDetails&) override { setHover (false); }
    void itemDropped   (const SourceDetails& d) override
    {
        setHover (false);

        if (auto* src = d.sourceComponent.get())
        {
            // The TreeView passes itself as the drag source, so check src first.
            auto* tree = dynamic_cast<juce::FileTreeComponent*> (src);

            if (tree == nullptr)
                tree = src->findParentComponentOfClass<juce::FileTreeComponent>();

            if (tree != nullptr)
            {
                auto f = tree->getSelectedFile();
                if (f.existsAsFile() && isAudioFile (f))
                    assign (f);
            }
        }
    }

private:
    static bool isAudioFile (const juce::File& f)
    {
        return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg");
    }

    static juce::String lengthText (int beats)
    {
        const int bar = LooperEngine::beatsPerBar;

        if (beats % bar == 0)
            return beats == bar ? juce::String ("1 bar") : juce::String (beats / bar) + " bars";

        return juce::String (beats) + (beats == 1 ? " beat" : " beats");
    }

    static juce::Colour chokeColour (int group)
    {
        static const juce::uint32 colours[] = { 0xff4dd0e1, 0xffaed581, 0xfff06292, 0xffffd54f,
                                                0xff9575cd, 0xffff8a65, 0xff4db6ac, 0xffe0e0e0 };
        return juce::Colour (colours[(group - 1) % (int) std::size (colours)]);
    }

    juce::String modeLabel() const
    {
        switch (mode)
        {
            case PadMode::oneShot: return "ONE-SHOT";
            case PadMode::loop:    return lengthBeats > 0 ? "LOOP  " + lengthText (lengthBeats) : "LOOP";
            case PadMode::gate:    return "GATE";
        }
        return {};
    }

    void showModeMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "One-shot",         true, mode == PadMode::oneShot);
        m.addItem (2, "Loop (quantized)", true, mode == PadMode::loop);
        m.addItem (3, "Gate (hold)",      true, mode == PadMode::gate);

        // Loop length: how many beats the clip spans, so it can be stretched to the project tempo.
        juce::PopupMenu len;
        len.addItem (10, "Auto");
        for (int i = 0; i < 7; ++i)
            len.addItem (11 + i, lengthText (lengthChoices[i]), true, lengthBeats == lengthChoices[i]);

        m.addSeparator();
        m.addSubMenu ("Loop length", len);

        // Choke group: starting a pad cuts off the others in its group (on the same grid line for loops).
        juce::PopupMenu choke;
        choke.addItem (chokeMenuBase, "None", true, chokeGroup == 0);
        for (int gr = 1; gr <= LooperEngine::numChokeGroups; ++gr)
            choke.addItem (chokeMenuBase + gr, "Group " + juce::String (gr), true, chokeGroup == gr);

        m.addSubMenu ("Choke group", choke);

        m.addSeparator();
        m.addItem (5, "Instrument & Effects...");
        m.addItem (4, "Clear", clipName.isNotEmpty() || status != PadStatus::idle);

        juce::Component::SafePointer<PadComponent> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result >= chokeMenuBase)
            {
                const int group = result - chokeMenuBase;
                safe->setChokeGroup (group);
                if (safe->onChokeChanged) safe->onChokeChanged (safe->index, group);
                return;
            }

            if (result >= 10)
            {
                const int beats = result == 10 ? 0 : lengthChoices[result - 11];
                if (safe->onLengthChanged) safe->onLengthChanged (safe->index, beats);
                return;
            }

            if (result == 4)
            {
                if (safe->onCleared) safe->onCleared (safe->index);
                return;
            }

            if (result == 5)
            {
                if (safe->onEffects) safe->onEffects (safe->index);
                return;
            }

            const auto newMode = result == 1 ? PadMode::oneShot
                               : result == 2 ? PadMode::loop
                                             : PadMode::gate;
            safe->setMode (newMode);
            if (safe->onModeChanged) safe->onModeChanged (safe->index, newMode);
        });
    }

    void assign (const juce::File& f) { if (onFileAssigned) onFileAssigned (index, f); }
    void setHover (bool h)            { dragHover = h; repaint(); }

    int index;
    juce::String clipName, instrumentName;
    PadMode mode = PadMode::oneShot;
    int lengthBeats = 0;
    int chokeGroup = 0;
    PadStatus status = PadStatus::idle;
    bool dragHover = false, pressed = false, recordMode = false, hasEffects = false, active = false, silenced = false;
};

//==============================================================================
// 4x4 grid of square pads, centred and kept square at any window size.
class PadGrid : public juce::Component
{
public:
    static constexpr int cols = 4, rows = 4;

    PadGrid()
    {
        for (int i = 0; i < cols * rows; ++i)
        {
            pads[(size_t) i] = std::make_unique<PadComponent> (i);
            addAndMakeVisible (*pads[(size_t) i]);
        }
    }

    PadComponent& pad (int i) { return *pads[(size_t) i]; }

    void setRecordMode (bool on)
    {
        for (auto& p : pads)
            p->setRecordMode (on);
    }

    void resized() override
    {
        const int side = juce::jmin (getWidth() / cols, getHeight() / rows);
        auto area = juce::Rectangle<int> (side * cols, side * rows).withCentre (getLocalBounds().getCentre());

        // Pad 1 = bottom-left, like an MPC.
        for (int i = 0; i < cols * rows; ++i)
        {
            const int c = i % cols, r = rows - 1 - (i / cols);
            pads[(size_t) i]->setBounds (area.getX() + c * side, area.getY() + r * side, side, side);
        }
    }

private:
    std::array<std::unique_ptr<PadComponent>, cols * rows> pads;
};
