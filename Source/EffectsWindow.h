#pragma once
#include <JuceHeader.h>
#include "PadEffects.h"

//==============================================================================
// The effects chain of one pad (or the input monitor): add, remove, reorder, open a plugin's editor.
// Sound runs through the list from top to bottom. A pad can also have an instrument, which plays before the effects.
class EffectsPanel final : public juce::Component,
                           private juce::ListBoxModel,
                           private juce::ChangeListener
{
public:
    EffectsPanel (PadEffects& fx, LooperEngine& e, int targetId) : effects (fx), engine (e), target (targetId)
    {
        list.setModel (this);
        list.setRowHeight (26);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff121212));
        addAndMakeVisible (list);

        addButton.onClick    = [this] { showAddMenu(); };
        removeButton.onClick = [this] { effects.remove (target, list.getSelectedRow()); };
        upButton.onClick     = [this] { moveSelected (-1); };
        downButton.onClick   = [this] { moveSelected (1); };
        editButton.onClick   = [this] { effects.showEditor (target, list.getSelectedRow()); };

        for (auto* b : { &addButton, &removeButton, &upButton, &downButton, &editButton })
            addAndMakeVisible (b);

        if (target != LooperEngine::monitorTarget)
        {
            instrumentLabel.setText ("Instrument", juce::dontSendNotification);
            addAndMakeVisible (instrumentLabel);

            instrumentButton.setTooltip ("Played from the performance channel (Settings > MIDI Mapping) while this is the active pad");
            instrumentButton.onClick = [this] { showInstrumentMenu(); };
            addAndMakeVisible (instrumentButton);

            instrumentEditButton.onClick = [this] { effects.showInstrumentEditor (target); };
            addAndMakeVisible (instrumentEditButton);
        }

        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);

        latencyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8a33d));
        addAndMakeVisible (latencyLabel);

        effects.addChangeListener (this);
        refresh();
        setSize (460, target == LooperEngine::monitorTarget ? 360 : 400);
    }

    ~EffectsPanel() override { effects.removeChangeListener (this); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        hintLabel.setBounds (r.removeFromTop (44));

        if (target != LooperEngine::monitorTarget)
        {
            auto row = r.removeFromTop (28);
            instrumentLabel.setBounds (row.removeFromLeft (80));
            instrumentEditButton.setBounds (row.removeFromRight (72));
            row.removeFromRight (6);
            instrumentButton.setBounds (row);
            r.removeFromTop (8);
        }

        auto buttons = r.removeFromBottom (28);
        for (auto* b : { &addButton, &removeButton, &upButton, &downButton })
        {
            b->setBounds (buttons.removeFromLeft (72));
            buttons.removeFromLeft (6);
        }
        editButton.setBounds (buttons.removeFromRight (72));

        r.removeFromBottom (6);
        latencyLabel.setBounds (r.removeFromBottom (latencyLabel.getText().isEmpty() ? 0 : 36));
        list.setBounds (r);
    }

private:
    int getNumRows() override { return (int) effects.getSlots (target).size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        const auto& slots = effects.getSlots (target);
        if (! juce::isPositiveAndBelow (row, (int) slots.size()))
            return;

        if (selected)
            g.fillAll (juce::Colour (0xff2e4a6b));

        const auto& slot = *slots[(size_t) row];
        g.setColour (juce::Colours::white);
        g.setFont (14.0f);
        g.drawText (juce::String (row + 1) + ".  " + slot.description.name, 8, 0, width - 140, height,
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (12.0f);
        const auto channels = slot.channelInfo();
        auto right = slot.description.pluginFormatName + (channels.isNotEmpty() ? ", " + channels : juce::String());
        if (const int ms = latencyMs (slot.plugin->getLatencySamples()); ms > 0)
        {
            g.setColour (juce::Colour (0xffe8a33d));
            right = "+" + juce::String (ms) + " ms  " + right;
        }
        g.drawText (right, width - 132, 0, 124, height, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { effects.showEditor (target, row); }
    void selectedRowsChanged (int) override                                    { updateButtons(); }
    void deleteKeyPressed (int row) override                                   { effects.remove (target, row); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        list.updateContent();
        list.repaint();

        const auto* instrument = target != LooperEngine::monitorTarget ? effects.getInstrument (target) : nullptr;

        hintLabel.setText (target == LooperEngine::monitorTarget
                               ? "Your input runs through these first, top to bottom, before any pad's effects. "
                                 "They're recorded."
                           : instrument != nullptr
                               ? "The performance channel plays the instrument while this is the active pad. It runs "
                                 "through the effects, top to bottom; that's what you hear and what the pad records."
                               : "Your input runs through these, top to bottom, when this pad records, and while it's "
                                 "the active pad with Monitor on. They're recorded; the loop plays back as recorded.",
                           juce::dontSendNotification);

        instrumentButton.setButtonText (instrument != nullptr ? instrument->description.name + "  (" + instrument->description.pluginFormatName + ")"
                                                              : juce::String ("None"));
        instrumentEditButton.setEnabled (instrument != nullptr);

        int total = instrument != nullptr ? instrument->plugin->getLatencySamples() : 0;
        for (auto& slot : effects.getSlots (target))
            total += slot->plugin->getLatencySamples();

        // A pad without an instrument hears the input through the input chain first
        int inputChain = 0;
        if (target != LooperEngine::monitorTarget && instrument == nullptr)
            for (auto& slot : effects.getSlots (LooperEngine::monitorTarget))
                inputChain += slot->plugin->getLatencySamples();
        total += inputChain;

        const auto text = total > 0 ? juce::String (instrument != nullptr ? "The instrument and effects delay what you play by "
                                                    : inputChain > 0      ? "These effects and the input's delay your input by "
                                                                          : "These effects delay your input by ")
                                          + juce::String (latencyMs (total))
                                          + " ms, so you'll hear yourself late. Recordings are moved back to make up for it."
                                    : juce::String();
        if (text != latencyLabel.getText())
        {
            latencyLabel.setText (text, juce::dontSendNotification);
            resized();
        }

        updateButtons();
    }

    void updateButtons()
    {
        const int row = list.getSelectedRow();
        const int n = getNumRows();
        removeButton.setEnabled (row >= 0);
        editButton.setEnabled (row >= 0);
        upButton.setEnabled (row > 0);
        downButton.setEnabled (row >= 0 && row < n - 1);
    }

    void moveSelected (int delta)
    {
        const int row = list.getSelectedRow();
        effects.move (target, row, row + delta);
        list.selectRow (row + delta);
    }

    // Sorts `available` by manufacturer and name, and adds them to `menu` grouped by manufacturer, as ids 1..n.
    static void addPluginItems (juce::PopupMenu& menu, juce::Array<juce::PluginDescription>& available, const juce::String& noneText)
    {
        std::sort (available.begin(), available.end(), [] (const auto& a, const auto& b)
        {
            const auto ma = a.manufacturerName.isEmpty() ? juce::String ("Other") : a.manufacturerName;
            const auto mb = b.manufacturerName.isEmpty() ? juce::String ("Other") : b.manufacturerName;
            if (ma.compareIgnoreCase (mb) != 0) return ma.compareIgnoreCase (mb) < 0;
            return a.name.compareIgnoreCase (b.name) < 0;
        });

        if (available.isEmpty())
        {
            menu.addItem (-1, noneText, false);
            return;
        }

        juce::PopupMenu sub;
        juce::String maker;

        for (int i = 0; i < available.size(); ++i)
        {
            const auto& d = available.getReference (i);
            const auto m = d.manufacturerName.isEmpty() ? juce::String ("Other") : d.manufacturerName;

            if (m != maker && sub.getNumItems() > 0)
            {
                menu.addSubMenu (maker, sub);
                sub = {};
            }
            maker = m;
            sub.addItem (i + 1, d.name + "  (" + d.pluginFormatName + ")");
        }
        menu.addSubMenu (maker, sub);
    }

    // Enabled instruments, grouped by manufacturer, plus None.
    void showInstrumentMenu()
    {
        auto available = effects.getAvailableInstruments();
        const bool has = effects.getInstrument (target) != nullptr;

        juce::PopupMenu menu;
        menu.addItem (1000000, "None", has, ! has);
        menu.addSeparator();
        addPluginItems (menu, available, "No instruments found: scan for plugins in Edit > Plugins");

        juce::Component::SafePointer<EffectsPanel> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (instrumentButton), [safe, available] (int result)
        {
            if (safe == nullptr || result <= 0)
                return;

            if (result == 1000000)
            {
                safe->effects.removeInstrument (safe->target);
                return;
            }

            juce::MouseCursor::showWaitCursor();
            const auto error = safe->effects.setInstrument (safe->target, available.getReference (result - 1));
            juce::MouseCursor::hideWaitCursor();

            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Instrument", error, {}, safe.getComponent());
        });
    }

    // Enabled effects, grouped by manufacturer.
    void showAddMenu()
    {
        auto available = effects.getAvailableEffects();

        juce::PopupMenu menu;
        addPluginItems (menu, available, "No effects found: scan for plugins in Edit > Plugins");

        juce::Component::SafePointer<EffectsPanel> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton), [safe, available] (int result)
        {
            if (safe == nullptr || result <= 0)
                return;

            juce::MouseCursor::showWaitCursor();
            const auto error = safe->effects.add (safe->target, available.getReference (result - 1));
            juce::MouseCursor::hideWaitCursor();

            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Add effect", error, {}, safe.getComponent());
            else
                safe->list.selectRow (safe->getNumRows() - 1);
        });
    }

    int latencyMs (int samples) const
    {
        return (int) std::lround (1000.0 * samples / juce::jmax (1.0, engine.getSampleRate()));
    }

    PadEffects& effects;
    LooperEngine& engine;
    const int target;

    juce::ListBox list;
    juce::TextButton addButton { "Add..." }, removeButton { "Remove" }, upButton { "Up" }, downButton { "Down" },
                     editButton { "Edit" };
    juce::Label hintLabel, latencyLabel, instrumentLabel;
    juce::TextButton instrumentButton { "None" }, instrumentEditButton { "Edit" };
};

//==============================================================================
// Pad right-click > Instrument & Effects, or the FX button next to Monitor. Closing hides it.
class EffectsWindow final : public juce::DocumentWindow
{
public:
    EffectsWindow (PadEffects& fx, LooperEngine& e, int target)
        : DocumentWindow ((target == LooperEngine::monitorTarget ? "Effects - " : "Instrument & Effects - ") + PadEffects::targetName (target), juce::Colour (0xff1b1b1b), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new EffectsPanel (fx, e, target), true);
        setResizable (true, false);
        setResizeLimits (420, 260, 1000, 1200);
    }

    void open (juce::Component* centreOn)
    {
        if (! isVisible())
            centreAroundComponent (centreOn, getWidth(), getHeight());

        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override { setVisible (false); }
};
