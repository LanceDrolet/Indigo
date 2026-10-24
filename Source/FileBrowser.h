#pragma once
#include <JuceHeader.h>
#include "MidiPresets.h"
#include <optional>

//==============================================================================
// Where the sample browser opens (Settings > Browser): the starting folder (~/Music until the user picks one,
// or if the chosen one has gone), or, with "reopen last folder" on, wherever it was last. Kept in
// ~/.config/Indigo/Browser.xml.
struct BrowserSettings
{
    static juce::File file() { return MidiPresets::folder().getChildFile ("Browser.xml"); }

    static juce::File defaultFolder() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory); }

    static juce::File startFolder()     { return folderAttribute ("startFolder").value_or (defaultFolder()); }
    static bool reopensLastFolder()     { return load()->getBoolAttribute ("reopenLast", false); }

    // The folder to show at startup.
    static juce::File openingFolder()
    {
        if (reopensLastFolder())
            if (auto last = folderAttribute ("lastFolder"))
                return *last;

        return startFolder();
    }

    static bool setStartFolder (const juce::File& dir)   { return set ("startFolder", dir.getFullPathName()); }
    static bool setReopenLastFolder (bool on)            { return set ("reopenLast", on); }
    static bool setLastFolder (const juce::File& dir)    { return set ("lastFolder", dir.getFullPathName()); }

private:
    static std::unique_ptr<juce::XmlElement> load()
    {
        auto xml = juce::XmlDocument::parse (file());
        if (xml == nullptr || ! xml->hasTagName ("Browser"))
            xml = std::make_unique<juce::XmlElement> ("Browser");
        return xml;
    }

    static std::optional<juce::File> folderAttribute (const char* name)
    {
        const auto path = load()->getStringAttribute (name);
        if (juce::File::isAbsolutePath (path) && juce::File (path).isDirectory())
            return juce::File (path);
        return std::nullopt;
    }

    static bool set (const char* name, const juce::var& value)
    {
        if (! MidiPresets::folder().createDirectory())
            return false;

        auto xml = load();
        xml->setAttribute (name, value.toString());
        return xml->writeTo (file());
    }
};

//==============================================================================
// The file tree plus a navigation bar: Back, Up, Home (the starting folder), Browse... for any folder, and
// the current folder's path, which can be typed into. Double-clicking a folder opens it; right-clicking one
// offers to open it or make it the starting folder. Drags out of `tree` go to the pads as before.
class SampleBrowser : public juce::Component,
                      private juce::FileBrowserListener
{
public:
    SampleBrowser()
    {
        dirList.setDirectory (BrowserSettings::openingFolder(), true, true);
        scanThread.startThread();

        tree.setColour (juce::FileTreeComponent::backgroundColourId, juce::Colour (0xff1b1b1b));
        tree.addListener (this);
        addAndMakeVisible (tree);

        backButton.setTooltip ("Back to the previous folder");
        backButton.onClick = [this] { goBack(); };
        upButton.setTooltip ("Up to the parent folder");
        upButton.onClick = [this] { navigateTo (getCurrentFolder().getParentDirectory()); };
        homeButton.setTooltip ("Go to the starting folder (set in Edit > Settings > Browser)");
        homeButton.onClick = [this] { navigateTo (BrowserSettings::startFolder()); };
        browseButton.setTooltip ("Pick any folder to browse");
        browseButton.onClick = [this] { chooseFolder(); };

        for (auto* b : { &backButton, &upButton, &homeButton, &browseButton })
            addAndMakeVisible (b);

        pathEditor.setTooltip ("The folder being browsed. Type a path (~ works) and press Enter to go there.");
        pathEditor.setSelectAllWhenFocused (true);
        pathEditor.onReturnKey = [this] { goToTypedPath(); };
        pathEditor.onEscapeKey = [this] { showPath(); unfocusAllComponents(); };
        pathEditor.onFocusLost = [this] { showPath(); };
        addAndMakeVisible (pathEditor);

        showPath();
    }

    ~SampleBrowser() override { tree.removeListener (this); }

    juce::FileTreeComponent& getTree() noexcept { return tree; }
    juce::File getCurrentFolder() const     { return dirList.getDirectory(); }

    void navigateTo (const juce::File& dir)
    {
        if (! dir.isDirectory() || dir == getCurrentFolder())
            return;

        history.add (getCurrentFolder());
        if (history.size() > 50)
            history.remove (0);

        show (dir);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        auto buttons = r.removeFromTop (26);
        for (auto* b : { &backButton, &upButton, &homeButton })
        {
            b->setBounds (buttons.removeFromLeft (52));
            buttons.removeFromLeft (4);
        }
        browseButton.setBounds (buttons.removeFromRight (80));

        r.removeFromTop (4);
        pathEditor.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        tree.setBounds (r);
    }

private:
    void showPath()
    {
        const auto dir = getCurrentFolder();
        pathEditor.setText (dir.getFullPathName(), juce::dontSendNotification);
        pathEditor.moveCaretToEnd();
        backButton.setEnabled (! history.isEmpty());
        upButton.setEnabled (dir.getParentDirectory() != dir);
    }

    void goBack()
    {
        while (! history.isEmpty())
        {
            const auto dir = history.removeAndReturn (history.size() - 1);
            if (dir.isDirectory())
            {
                show (dir);
                return;
            }
        }
        showPath();
    }

    // Remembered on every move (not just at exit), so a crash doesn't lose it.
    void show (const juce::File& dir)
    {
        dirList.setDirectory (dir, true, true);
        BrowserSettings::setLastFolder (dir);
        showPath();
    }

    void goToTypedPath()
    {
        const auto text = pathEditor.getText().trim();
        const auto dir = juce::File::isAbsolutePath (text) ? juce::File (text)
                                                           : getCurrentFolder().getChildFile (text);
        if (dir.isDirectory())
            navigateTo (dir);
        else if (dir.existsAsFile())
            navigateTo (dir.getParentDirectory());

        showPath();
        tree.grabKeyboardFocus();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Browse folder", getCurrentFolder());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult().isDirectory())
                                      navigateTo (fc.getResult());
                              });
    }

    void showFolderMenu (const juce::File& dir)
    {
        juce::PopupMenu menu;
        menu.addItem ("Open", [this, dir] { navigateTo (dir); });
        menu.addItem ("Set as starting folder", [dir] { BrowserSettings::setStartFolder (dir); });
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    //==== FileBrowserListener ====
    void selectionChanged() override {}
    void browserRootChanged (const juce::File&) override {}

    void fileClicked (const juce::File& f, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && f.isDirectory())
            showFolderMenu (f);
    }

    void fileDoubleClicked (const juce::File& f) override
    {
        if (f.isDirectory())
            navigateTo (f);
    }

    juce::TimeSliceThread scanThread { "File scanner" };
    juce::WildcardFileFilter audioFilter { "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg", "*", "Audio files" };
    juce::DirectoryContentsList dirList { &audioFilter, scanThread };
    juce::FileTreeComponent tree { dirList };

    juce::TextButton backButton { "Back" }, upButton { "Up" }, homeButton { "Home" }, browseButton { "Browse..." };
    juce::TextEditor pathEditor;
    juce::Array<juce::File> history;        // folders Back returns to, most recent last
    std::unique_ptr<juce::FileChooser> chooser;
};
