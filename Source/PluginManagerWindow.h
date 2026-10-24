#pragma once
#include <JuceHeader.h>
#include "Plugins.h"

//==============================================================================
// JUCE's plugin table, plus an "On" column: disabled plugins stay in the list but aren't offered on pads.
// Rows past the known plugins are files that failed (or crashed) while being scanned.
class PluginTableModel final : public juce::TableListBoxModel
{
public:
    enum { nameCol = 1, typeCol, categoryCol, manufacturerCol, descCol, enabledCol };   // 1..5 match JUCE's

    PluginTableModel (juce::PluginListComponent& c, PluginLibrary& l) : owner (c), library (l) {}

    int getNumRows() override
    {
        return library.known.getNumTypes() + library.known.getBlacklistedFiles().size();
    }

    void paintRowBackground (juce::Graphics& g, int, int, int, bool selected) override
    {
        const auto bg = owner.findColour (juce::ListBox::backgroundColourId);
        g.fillAll (selected ? bg.interpolatedWith (owner.findColour (juce::ListBox::textColourId), 0.3f) : bg);
    }

    void paintCell (juce::Graphics& g, int row, int col, int width, int height, bool) override
    {
        const int numTypes = library.known.getNumTypes();
        const auto textColour = owner.findColour (juce::ListBox::textColourId);
        juce::String text;
        auto colour = textColour;

        if (row >= numTypes)
        {
            if (col == nameCol)       text = library.known.getBlacklistedFiles()[row - numTypes];
            else if (col == descCol)  text = "Failed or crashed while being scanned";
            colour = juce::Colour (0xffef5350);
        }
        else
        {
            const auto desc = library.known.getTypes()[row];
            const bool on = library.isEnabled (desc);

            if (col == enabledCol)
            {
                const int box = juce::jmin (width, height) - 6;
                owner.getLookAndFeel().drawTickBox (g, owner, (float) (width - box) / 2.0f, (float) (height - box) / 2.0f,
                                                    (float) box, (float) box, on, true, false, false);
                return;
            }

            switch (col)
            {
                case nameCol:         text = desc.name; break;
                case typeCol:         text = desc.pluginFormatName; break;
                case categoryCol:     text = describeType (desc); break;
                case manufacturerCol: text = desc.manufacturerName; break;
                case descCol:         text = desc.version; break;
                default: break;
            }

            colour = textColour.withAlpha (! on ? 0.35f : col == nameCol ? 1.0f : 0.7f);
        }

        g.setColour (colour);
        g.setFont ((float) height * 0.7f);
        g.drawFittedText (text, 4, 0, width - 6, height, juce::Justification::centredLeft, 1, 0.9f);
    }

    void cellClicked (int row, int col, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, getNumRows()))
            return;

        if (e.mods.isPopupMenu())
        {
            owner.createMenuForRow (row).showMenuAsync (juce::PopupMenu::Options().withDeletionCheck (owner));
            return;
        }

        if (col == enabledCol && row < library.known.getNumTypes())
        {
            const auto desc = library.known.getTypes()[row];
            library.setEnabled (desc, ! library.isEnabled (desc));
            owner.getTableListBox().repaintRow (row);
        }
    }

    void deleteKeyPressed (int) override { owner.removeSelectedPlugins(); }

    void sortOrderChanged (int col, bool forwards) override
    {
        switch (col)
        {
            case nameCol:         library.known.sort (juce::KnownPluginList::sortAlphabetically, forwards); break;
            case typeCol:         library.known.sort (juce::KnownPluginList::sortByFormat, forwards); break;
            case categoryCol:     library.known.sort (juce::KnownPluginList::sortByCategory, forwards); break;
            case manufacturerCol: library.known.sort (juce::KnownPluginList::sortByManufacturer, forwards); break;
            default: break;
        }
    }

private:
    // "Instrument" / "Effect", plus the plugin's own category minus the words that just repeat that
    // (VST3 sends e.g. "Instrument|Synth" or "Fx|Reverb"; LV2 a class label like "Instrument" or "Reverb").
    static juce::String describeType (const juce::PluginDescription& desc)
    {
        auto parts = juce::StringArray::fromTokens (desc.category, "|", {});
        for (auto* word : { "Instrument", "Fx", "Plugin", "Generator" })
            parts.removeString (word, true);
        parts.trim();
        parts.removeEmptyStrings();

        const juce::String kind = desc.isInstrument ? "Instrument" : "Effect";
        return parts.isEmpty() ? kind : kind + " (" + parts.joinIntoString (", ") + ")";
    }

    juce::PluginListComponent& owner;
    PluginLibrary& library;
};

//==============================================================================
class PluginManagerPanel final : public juce::Component
{
public:
    explicit PluginManagerPanel (PluginLibrary& l)
        : library (l),
          list (l.formats, l.known, PluginLibrary::deadMansPedalFile(), &l.getScanSettings(),
                true)   // scan on a background thread (each probe waits on the worker process)
    {
        list.setTableModel (new PluginTableModel (list, library));

        auto& header = list.getTableListBox().getHeader();
        header.addColumn ("On", PluginTableModel::enabledCol, 36, 36, 36, juce::TableHeaderComponent::notResizableOrSortable, 0);
        header.setColumnName (PluginTableModel::categoryCol, "Type");
        header.setColumnName (PluginTableModel::descCol, "Version");
        header.setColumnWidth (PluginTableModel::descCol, 90);

        list.setScanDialogText ("Scanning for plugins", "Each plugin is checked in a separate process, so one that "
                                                        "crashes is just marked as failed.");
        addAndMakeVisible (list);

        for (auto* format : library.formats.getFormats())
        {
            auto* b = scanButtons.add (new juce::TextButton ("Scan " + format->getName() + "..."));
            b->setTooltip ("Choose the " + format->getName() + " folders to scan, then look for new or updated plugins");
            b->onClick = [this, format] { if (! list.isScanning()) list.scanFor (*format); };
            addAndMakeVisible (b);
        }

        hintLabel.setText ("Untick a plugin to keep it off the pads' plugin menus. Right-click a row for more; "
                           "Options has Clear list and Remove missing.", juce::dontSendNotification);
        hintLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hintLabel);

        setSize (760, 560);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        auto top = r.removeFromTop (28);
        for (auto* b : scanButtons)
        {
            b->setBounds (top.removeFromLeft (110));
            top.removeFromLeft (6);
        }

        r.removeFromTop (6);
        hintLabel.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        list.setBounds (r);
    }

private:
    PluginLibrary& library;
    juce::PluginListComponent list;
    juce::OwnedArray<juce::TextButton> scanButtons;
    juce::Label hintLabel;
};

//==============================================================================
// Edit > Plugins. Closing hides it.
class PluginManagerWindow final : public juce::DocumentWindow
{
public:
    explicit PluginManagerWindow (PluginLibrary& l)
        : DocumentWindow ("Plugins", juce::Colour (0xff1b1b1b), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new PluginManagerPanel (l), true);
        setResizable (true, false);
        setResizeLimits (560, 360, 1600, 1400);
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
