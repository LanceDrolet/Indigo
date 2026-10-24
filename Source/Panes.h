#pragma once
#include <JuceHeader.h>
#include "MidiPresets.h"

//==============================================================================
// The main window's layout: which side panes are shown, their sizes, their selected tabs, and the window size. Kept
// in ~/.config/Indigo/Layout.xml.
struct LayoutSettings
{
    bool leftShown = true, bottomShown = true;
    int leftWidth = 300, bottomHeight = 270;
    int leftTab = 0, bottomTab = 0;
    int windowWidth = 0, windowHeight = 0;          // 0 = not saved yet

    static juce::File file() { return MidiPresets::folder().getChildFile ("Layout.xml"); }

    static LayoutSettings load()
    {
        LayoutSettings s;
        auto xml = juce::XmlDocument::parse (file());
        if (xml == nullptr || ! xml->hasTagName ("Layout"))
            return s;

        s.leftShown    = xml->getBoolAttribute ("leftShown", s.leftShown);
        s.bottomShown  = xml->getBoolAttribute ("bottomShown", s.bottomShown);
        s.leftWidth    = xml->getIntAttribute ("leftWidth", s.leftWidth);
        s.bottomHeight = xml->getIntAttribute ("bottomHeight", s.bottomHeight);
        s.leftTab      = xml->getIntAttribute ("leftTab", 0);
        s.bottomTab    = xml->getIntAttribute ("bottomTab", 0);
        s.windowWidth  = xml->getIntAttribute ("windowWidth", 0);
        s.windowHeight = xml->getIntAttribute ("windowHeight", 0);
        return s;
    }

    bool save() const
    {
        juce::XmlElement xml ("Layout");
        xml.setAttribute ("leftShown", leftShown);
        xml.setAttribute ("bottomShown", bottomShown);
        xml.setAttribute ("leftWidth", leftWidth);
        xml.setAttribute ("bottomHeight", bottomHeight);
        xml.setAttribute ("leftTab", leftTab);
        xml.setAttribute ("bottomTab", bottomTab);
        xml.setAttribute ("windowWidth", windowWidth);
        xml.setAttribute ("windowHeight", windowHeight);
        return MidiPresets::folder().createDirectory() && xml.writeTo (file());
    }
};

//==============================================================================
// A strip between a pane and the pads that can be dragged to resize the pane. onDrag gets the distance from where
// the drag started, along the strip's axis.
class PaneDivider : public juce::Component
{
public:
    explicit PaneDivider (bool isVertical) : vertical (isVertical)
    {
        setMouseCursor (vertical ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
    }

    std::function<void()> onDragStart;
    std::function<void (int)> onDrag;

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colours::white.withAlpha (isMouseOverOrDragging() ? 0.25f : 0.08f));
        auto r = getLocalBounds().toFloat();
        g.fillRect (vertical ? r.withSizeKeepingCentre (1.0f, r.getHeight()) : r.withSizeKeepingCentre (r.getWidth(), 1.0f));
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDown (const juce::MouseEvent&) override  { if (onDragStart) onDragStart(); }
    void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (vertical ? e.getDistanceFromDragStartX() : e.getDistanceFromDragStartY()); }

private:
    const bool vertical;
};

//==============================================================================
// A side pane: tabs (more views can be added later) and a button to hide it. Hidden, it shrinks to a slim bar with
// a button per tab that brings it back on that tab.
class SidePane : public juce::Component
{
public:
    SidePane (bool onLeft, const juce::String& paneName) : left (onLeft), name (paneName)
    {
        tabs.setTabBarDepth (26);
        tabs.setOutline (0);
        tabs.setColour (juce::TabbedComponent::backgroundColourId, juce::Colour (0xff161616));
        addAndMakeVisible (tabs);

        hideButton.setTooltip ("Hide the " + name + " pane (View menu, or the bar at the edge, brings it back)");
        hideButton.onClick = [this] { setShown (false); };
        addAndMakeVisible (hideButton);
    }

    std::function<void()> onShownChanged;      // the owner lays out again

    void addTab (const juce::String& tabName, juce::Component& content)
    {
        tabs.addTab (tabName, juce::Colour (0xff161616), &content, false);

        auto* b = restoreButtons.add (new juce::TextButton (left ? juce::String::fromUTF8 ("\xe2\x80\xba") : tabName + juce::String::fromUTF8 ("  \xe2\x96\xb4")));
        b->setTooltip ("Show the " + name + " pane (" + tabName + ")");
        const int index = tabs.getNumTabs() - 1;
        b->onClick = [this, index] { tabs.setCurrentTabIndex (index); setShown (true); };
        addChildComponent (b);
        resized();
    }

    bool isShown() const noexcept { return shown; }

    void setShown (bool on)
    {
        if (on == shown)
            return;

        shown = on;
        tabs.setVisible (shown);
        hideButton.setVisible (shown);
        for (auto* b : restoreButtons)
            b->setVisible (! shown);

        if (onShownChanged)
            onShownChanged();
        resized();
    }

    int getCurrentTab() const          { return tabs.getCurrentTabIndex(); }
    void setCurrentTab (int index)     { if (juce::isPositiveAndBelow (index, tabs.getNumTabs())) tabs.setCurrentTabIndex (index); }
    juce::String getPaneName() const   { return name; }

    // How thick the pane is while hidden.
    static constexpr int collapsedSize = 22;

    void paint (juce::Graphics& g) override
    {
        if (! shown)
            g.fillAll (juce::Colour (0xff181818));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        tabs.setBounds (r);
        hideButton.setBounds (r.getRight() - 26, r.getY() + 3, 22, 20);

        if (left)
        {
            for (auto* b : restoreButtons)
                b->setBounds (r.removeFromTop (60).reduced (1, 2));
        }
        else
        {
            for (auto* b : restoreButtons)
                b->setBounds (r.removeFromLeft (100).reduced (2, 1));
        }
    }

private:
    const bool left;
    const juce::String name;
    bool shown = true;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::TextButton hideButton { juce::String::fromUTF8 ("\xe2\x80\x93") };     // en dash: "minimise"
    juce::OwnedArray<juce::TextButton> restoreButtons;
};
