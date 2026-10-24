#pragma once
#include <JuceHeader.h>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <set>
#include "MidiPresets.h"

//==============================================================================
// Plugin scanning happens in a second copy of Indigo, started with this id on its command line, so a
// plugin that crashes while being probed takes down only that process. The crashed plugin is then
// blacklisted (shown in red in the plugin manager) and the scan carries on with a fresh worker.
inline constexpr const char* pluginScanProcessId = "indigoPluginScanner";

// The worker side. Receives (format name, file or identifier), replies with the plugins it found as XML.
class PluginScanWorker final : private juce::ChildProcessWorker,
                               private juce::AsyncUpdater
{
public:
    PluginScanWorker() { formatManager.addDefaultFormats(); }

    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
    {
        if (mb.isEmpty())
            return;

        const std::lock_guard<std::mutex> lock (mutex);

        // Formats that need the message thread for creation are scanned from there instead.
        if (const auto results = scan (mb); ! results.isEmpty())
        {
            sendResults (results);
        }
        else
        {
            pending.emplace (mb);
            triggerAsyncUpdate();
        }
    }

    void handleConnectionLost() override { juce::JUCEApplicationBase::quit(); }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (pending.empty())
                return;

            sendResults (scan (pending.front()));
            pending.pop();
        }
    }

    juce::OwnedArray<juce::PluginDescription> scan (const juce::MemoryBlock& block)
    {
        juce::MemoryInputStream stream { block, false };
        const auto formatName = stream.readString();
        const auto identifier = stream.readString();

        juce::PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        pd.uniqueId = pd.deprecatedUid = 0;

        juce::OwnedArray<juce::PluginDescription> results;

        for (auto* format : formatManager.getFormats())
            if (format->getName() == formatName
                && (juce::MessageManager::getInstance()->isThisTheMessageThread()
                    || format->requiresUnblockedMessageThreadDuringCreation (pd)))
                format->findAllTypesForFile (results, identifier);

        return results;
    }

    void sendResults (const juce::OwnedArray<juce::PluginDescription>& results)
    {
        juce::XmlElement xml ("LIST");

        for (auto* desc : results)
            xml.addChildElement (desc->createXml().release());

        const auto str = xml.toString();
        sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });
    }

    std::mutex mutex;
    std::queue<juce::MemoryBlock> pending;
    juce::AudioPluginFormatManager formatManager;
};

//==============================================================================
// The Indigo side: KnownPluginList calls this (on the scanning thread) for each file it probes.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        if (worker == nullptr)
            worker = std::make_unique<Coordinator>();

        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream stream { block, true };
            stream.writeString (format.getName());
            stream.writeString (fileOrIdentifier);
        }

        if (worker->sendMessageToWorker (block))
        {
            for (;;)
            {
                if (shouldExit())
                    return true;

                auto response = worker->getResponse();

                if (response.state == Coordinator::State::timeout)
                    continue;

                if (response.xml != nullptr)
                {
                    for (auto* item : response.xml->getChildIterator())
                    {
                        auto desc = std::make_unique<juce::PluginDescription>();
                        if (desc->loadFromXml (*item))
                            result.add (std::move (desc));
                    }
                }

                if (response.state == Coordinator::State::gotResult)
                    return true;

                break;
            }
        }

        worker = nullptr;     // it crashed (or never started): the next file gets a fresh one
        return false;         // ...and this one is blacklisted
    }

    void scanFinished() override { worker = nullptr; }

private:
    class Coordinator final : private juce::ChildProcessCoordinator
    {
    public:
        Coordinator()
        {
            launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), pluginScanProcessId, 0, 0);
        }

        enum class State { timeout, gotResult, connectionLost };

        struct Response
        {
            State state;
            std::unique_ptr<juce::XmlElement> xml;
        };

        Response getResponse()
        {
            std::unique_lock<std::mutex> lock { mutex };

            if (! condvar.wait_for (lock, std::chrono::milliseconds { 50 }, [this] { return gotResult || connectionLost; }))
                return { State::timeout, nullptr };

            const auto state = connectionLost ? State::connectionLost : State::gotResult;
            connectionLost = gotResult = false;
            return { state, std::move (xml) };
        }

        using juce::ChildProcessCoordinator::sendMessageToWorker;

    private:
        void handleMessageFromWorker (const juce::MemoryBlock& mb) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            xml = juce::parseXML (mb.toString());
            gotResult = true;
            condvar.notify_one();
        }

        void handleConnectionLost() override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            connectionLost = true;
            condvar.notify_one();
        }

        std::mutex mutex;
        std::condition_variable condvar;
        std::unique_ptr<juce::XmlElement> xml;
        bool connectionLost = false, gotResult = false;
    };

    std::unique_ptr<Coordinator> worker;
};

//==============================================================================
// The plugins Indigo knows about (VST3 and LV2), which of them the user has disabled, and the folders
// last scanned. All of it lives under ~/.config/Indigo and is saved whenever it changes.
class PluginLibrary : private juce::ChangeListener
{
public:
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    PluginLibrary()
    {
        formats.addDefaultFormats();     // VST3 + LV2, as enabled in CMakeLists.txt

        juce::PropertiesFile::Options options;
        options.applicationName = "PluginScan";
        options.filenameSuffix = "settings";
        options.folderName = "Indigo";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        scanSettings = std::make_unique<juce::PropertiesFile> (MidiPresets::folder().getChildFile ("PluginScan.settings"), options);

        load();
        known.setCustomScanner (std::make_unique<OutOfProcessPluginScanner>());
        known.addChangeListener (this);
    }

    ~PluginLibrary() override
    {
        known.removeChangeListener (this);
        save();
    }

    // Folders per format, remembered by the plugin manager's scan dialog.
    juce::PropertiesFile& getScanSettings() noexcept       { return *scanSettings; }
    static juce::File deadMansPedalFile()                  { return MidiPresets::folder().getChildFile ("RecentlyCrashedPlugins"); }

    bool isEnabled (const juce::PluginDescription& d) const { return disabled.count (d.createIdentifierString()) == 0; }

    void setEnabled (const juce::PluginDescription& d, bool on)
    {
        if (on) disabled.erase (d.createIdentifierString());
        else    disabled.insert (d.createIdentifierString());
        save();
    }

    // What the pads are offered.
    juce::Array<juce::PluginDescription> getEnabledTypes() const
    {
        juce::Array<juce::PluginDescription> result;
        for (const auto& d : known.getTypes())
            if (isEnabled (d))
                result.add (d);
        return result;
    }

private:
    static juce::File listFile() { return MidiPresets::folder().getChildFile ("Plugins.xml"); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { save(); }

    void load()
    {
        auto xml = juce::XmlDocument::parse (listFile());
        if (xml == nullptr || ! xml->hasTagName ("IndigoPlugins"))
            return;

        if (auto* list = xml->getChildByName ("KNOWNPLUGINS"))
            known.recreateFromXml (*list);

        for (auto* el : xml->getChildWithTagNameIterator ("Disabled"))
            disabled.insert (el->getStringAttribute ("id"));
    }

    void save() const
    {
        juce::XmlElement xml ("IndigoPlugins");

        if (auto list = known.createXml())
            xml.addChildElement (list.release());

        for (const auto& id : disabled)
            xml.createNewChildElement ("Disabled")->setAttribute ("id", id);

        if (MidiPresets::folder().createDirectory())
            xml.writeTo (listFile());
    }

    std::unique_ptr<juce::PropertiesFile> scanSettings;
    std::set<juce::String> disabled;    // PluginDescription::createIdentifierString()
};
