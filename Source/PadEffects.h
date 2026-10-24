#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include "LooperEngine.h"
#include "Plugins.h"

//==============================================================================
// A plugin's editor (or a generic slider panel if it has none). Closing it deletes it.
class PluginEditorWindow final : public juce::DocumentWindow,
                                 private juce::Timer
{
public:
    PluginEditorWindow (std::weak_ptr<FxSlot> s, const juce::String& title)
        : DocumentWindow (title, juce::Colour (0xff1b1b1b), DocumentWindow::closeButton), slot (std::move (s))
    {
        auto& p = *slot.lock()->plugin;
        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);

        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);

        intendedSize = editor->getBounds();
        openedAt = juce::Time::getMillisecondCounter();
        startTimer (40);
    }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([s = slot]
        {
            if (auto sp = s.lock())
                sp->editorWindow = nullptr;
        });
    }

private:
    // Some LV2 editors (e.g. x42's Black Pearl Drumkit) briefly report a 128x128 placeholder for their X window as it
    // is embedded; JUCE then shrinks the editor to that and the window lands in the top-left corner. If that happens
    // just after opening, put back the size the plugin asked for and centre the window again.
    void timerCallback() override
    {
        if (auto* editor = getContentComponent())
        {
            if (editor->getWidth() <= collapsedSize && editor->getHeight() <= collapsedSize
                && (intendedSize.getWidth() > collapsedSize || intendedSize.getHeight() > collapsedSize))
            {
                editor->setSize (intendedSize.getWidth(), intendedSize.getHeight());
                centreWithSize (getWidth(), getHeight());
            }
        }

        if (juce::Time::getMillisecondCounter() - openedAt > 2000)
            stopTimer();
    }

    static constexpr int collapsedSize = 128;

    std::weak_ptr<FxSlot> slot;
    juce::Rectangle<int> intendedSize;
    juce::uint32 openedAt = 0;
};

//==============================================================================
// The effects chains as the user edits them: one per pad plus one for the input monitor (target
// LooperEngine::monitorTarget), and each pad's instrument, if it has one (it goes first in the engine's chain).
// Every change sends the engine a fresh snapshot of that chain, reusing the plugin instances, so their settings
// carry over. Message thread only. Broadcasts a change on every edit.
class PadEffects : public juce::ChangeBroadcaster,
                   private juce::Timer
{
public:
    static constexpr int numTargets = LooperEngine::numPads + 1;
    using Slots = std::vector<std::shared_ptr<FxSlot>>;

    PadEffects (LooperEngine& e, PluginLibrary& l) : engine (e), library (l) {}

    // The engine still holds the running chains (and so the plugins); only the editors go here.
    ~PadEffects() override { closeAllEditors(); }

    static juce::String targetName (int target)
    {
        return target == LooperEngine::monitorTarget ? "Input" : "Pad " + juce::String (target + 1);
    }

    const Slots& getSlots (int target) const        { return chains[(size_t) target]; }
    bool hasEffects (int target) const              { return ! chains[(size_t) target].empty(); }
    const FxSlot* getInstrument (int target) const  { return instruments[(size_t) target].get(); }

    bool hasAnyInstrument() const
    {
        return std::any_of (instruments.begin(), instruments.end(), [] (auto& s) { return s != nullptr; });
    }

    // A VST3 that ships a moduleinfo.json is scanned from that file alone, which has no channel counts, so JUCE
    // records 0 in / 0 out. Treat that as "unknown" rather than "no audio".
    static bool channelsUnknown (const juce::PluginDescription& d)
    {
        return d.numInputChannels == 0 && d.numOutputChannels == 0;
    }

    // What a chain can be offered: enabled plugins that take audio in and give audio out.
    juce::Array<juce::PluginDescription> getAvailableEffects() const
    {
        juce::Array<juce::PluginDescription> result;
        for (const auto& d : library.getEnabledTypes())
            if (! d.isInstrument && ((d.numInputChannels > 0 && d.numOutputChannels > 0) || channelsUnknown (d)))
                result.add (d);
        return result;
    }

    // Enabled instrument plugins.
    juce::Array<juce::PluginDescription> getAvailableInstruments() const
    {
        juce::Array<juce::PluginDescription> result;
        for (const auto& d : library.getEnabledTypes())
            if (d.isInstrument && (d.numOutputChannels > 0 || channelsUnknown (d)))
                result.add (d);
        return result;
    }

    // Gives a pad an instrument (replacing any it had). Returns an error message, or "" on success.
    juce::String setInstrument (int target, const juce::PluginDescription& desc)
    {
        if (target == LooperEngine::monitorTarget)
            return "The input can't have an instrument.";

        juce::String error;
        auto slot = createSlot (desc, error, true);
        if (slot == nullptr)
            return error;

        removeInstrumentEditor (target);
        instruments[(size_t) target] = std::move (slot);
        changed (target);
        return {};
    }

    void removeInstrument (int target)
    {
        if (instruments[(size_t) target] == nullptr)
            return;

        removeInstrumentEditor (target);
        instruments[(size_t) target] = nullptr;
        changed (target);
    }

    void showInstrumentEditor (int target)
    {
        if (auto& slot = instruments[(size_t) target])
            showEditor (slot, target);
    }

    // Appends a plugin to the chain. Returns an error message, or "" on success.
    juce::String add (int target, const juce::PluginDescription& desc)
    {
        juce::String error;
        auto slot = createSlot (desc, error);
        if (slot == nullptr)
            return error;

        chains[(size_t) target].push_back (std::move (slot));
        changed (target);
        return {};
    }

    void remove (int target, int index)
    {
        auto& c = chains[(size_t) target];
        if (! juce::isPositiveAndBelow (index, (int) c.size()))
            return;

        c[(size_t) index]->editorWindow = nullptr;     // the plugin itself goes when the engine lets go of it
        c.erase (c.begin() + index);
        changed (target);
    }

    void move (int target, int from, int to)
    {
        auto& c = chains[(size_t) target];
        if (! juce::isPositiveAndBelow (from, (int) c.size()) || ! juce::isPositiveAndBelow (to, (int) c.size()) || from == to)
            return;

        auto slot = c[(size_t) from];
        c.erase (c.begin() + from);
        c.insert (c.begin() + to, std::move (slot));
        changed (target);
    }

    void showEditor (int target, int index)
    {
        auto& c = chains[(size_t) target];
        if (! juce::isPositiveAndBelow (index, (int) c.size()))
            return;

        showEditor (c[(size_t) index], target);
    }

    // The engine dropped a chain prepared for an older device setup: send the current one again.
    void resend (int target)
    {
        if (juce::isPositiveAndBelow (target, numTargets))
            send (target);
    }

    //==== Saving ====
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("Effects");

        for (int t = 0; t < numTargets; ++t)
        {
            if (chains[(size_t) t].empty() && instruments[(size_t) t] == nullptr)
                continue;

            auto* chainXml = xml->createNewChildElement ("Chain");
            chainXml->setAttribute ("target", t == LooperEngine::monitorTarget ? juce::String ("monitor") : juce::String (t));

            if (auto& inst = instruments[(size_t) t])
                writeSlot (*chainXml->createNewChildElement ("Instrument"), *inst);

            for (auto& slot : chains[(size_t) t])
                writeSlot (*chainXml->createNewChildElement ("Plugin"), *slot);
        }
        return xml;
    }

    // Replaces every chain with the ones in `xml` (none if null). Plugins that can't be loaded are left out;
    // returns a line for each.
    juce::StringArray fromXml (const juce::XmlElement* xml)
    {
        closeAllEditors();
        std::array<Slots, numTargets> loaded;
        std::array<std::shared_ptr<FxSlot>, numTargets> loadedInstruments;
        juce::StringArray missing;

        if (xml != nullptr)
        {
            for (auto* chainXml : xml->getChildWithTagNameIterator ("Chain"))
            {
                const auto t = chainXml->getStringAttribute ("target");
                const int target = t == "monitor" ? LooperEngine::monitorTarget : t.getIntValue();
                if (! juce::isPositiveAndBelow (target, numTargets) || (t != "monitor" && ! t.containsOnly ("0123456789")))
                    continue;

                if (target != LooperEngine::monitorTarget)
                    if (auto* instXml = chainXml->getChildByName ("Instrument"))
                        loadedInstruments[(size_t) target] = readSlot (*instXml, target, true, missing);

                for (auto* pluginXml : chainXml->getChildWithTagNameIterator ("Plugin"))
                    if (auto slot = readSlot (*pluginXml, target, false, missing))
                        loaded[(size_t) target].push_back (std::move (slot));
            }
        }

        for (int t = 0; t < numTargets; ++t)
        {
            const bool hadAny = ! chains[(size_t) t].empty() || instruments[(size_t) t] != nullptr;
            chains[(size_t) t] = std::move (loaded[(size_t) t]);
            instruments[(size_t) t] = std::move (loadedInstruments[(size_t) t]);

            if (hadAny || ! chains[(size_t) t].empty() || instruments[(size_t) t] != nullptr)
                send (t);
        }

        sendChangeMessage();
        return missing;
    }

private:
    static void writeSlot (juce::XmlElement& xml, const FxSlot& slot)
    {
        xml.addChildElement (slot.description.createXml().release());

        juce::MemoryBlock state;
        slot.plugin->getStateInformation (state);
        xml.createNewChildElement ("State")->addTextElement (state.toBase64Encoding());
    }

    // A saved plugin with its settings, or null (and a line in `missing`) if it can't be loaded.
    std::shared_ptr<FxSlot> readSlot (const juce::XmlElement& xml, int target, bool instrument, juce::StringArray& missing)
    {
        juce::PluginDescription desc;
        auto* descXml = xml.getFirstChildElement();
        if (descXml == nullptr || ! desc.loadFromXml (*descXml))
            return nullptr;

        juce::String error;
        auto slot = createSlot (desc, error, instrument);
        if (slot == nullptr)
        {
            missing.add (targetName (target) + ": " + desc.name + " (" + desc.pluginFormatName + ")");
            return nullptr;
        }

        juce::MemoryBlock state;
        if (auto* stateXml = xml.getChildByName ("State"))
            if (state.fromBase64Encoding (stateXml->getAllSubText().trim()) && state.getSize() > 0)
                slot->plugin->setStateInformation (state.getData(), (int) state.getSize());

        return slot;
    }

    void showEditor (const std::shared_ptr<FxSlot>& slot, int target)
    {
        if (slot->editorWindow == nullptr)
            slot->editorWindow = std::make_unique<PluginEditorWindow> (slot, slot->description.name + " - " + targetName (target));

        slot->editorWindow->setVisible (true);
        slot->editorWindow->toFront (true);
    }

    void removeInstrumentEditor (int target)
    {
        if (auto& slot = instruments[(size_t) target])
            slot->editorWindow = nullptr;     // the plugin itself goes when the engine lets go of it
    }

    std::shared_ptr<FxSlot> createSlot (const juce::PluginDescription& desc, juce::String& error, bool instrument = false)
    {
        auto instance = library.formats.createPluginInstance (desc, engine.getSampleRate(), engine.getBlockSize(), error);
        if (instance == nullptr)
        {
            if (error.isEmpty())
                error = desc.name + " couldn't be loaded.";
            return nullptr;
        }

        auto slot = std::make_shared<FxSlot>();
        slot->description = desc;
        slot->plugin = std::move (instance);
        slot->instrument = instrument;

        if (! slot->configureBuses())
        {
            error = desc.name + " has no audio output Indigo can use.";
            return nullptr;
        }

        slot->plugin->setPlayHead (&engine);
        return slot;
    }

    void changed (int target)
    {
        send (target);
        sendChangeMessage();
    }

    // Prepares anything new at the engine's current settings and hands the engine a snapshot of the chain.
    void send (int target)
    {
        auto chain = std::make_unique<FxChain>();
        {
            const std::lock_guard<std::mutex> lock (engine.getFxPrepareLock());
            chain->rate = engine.getSampleRate();
            chain->block = engine.getBlockSize();

            if (auto& inst = instruments[(size_t) target])
                chain->slots.push_back (inst);

            for (auto& slot : chains[(size_t) target])
                chain->slots.push_back (slot);

            for (auto& slot : chain->slots)
                if (! slot->isPreparedFor (chain->rate, chain->block))
                    slot->prepare (chain->rate, chain->block);
        }

        chain->instrument = instruments[(size_t) target] != nullptr;
        chain->latency = chain->getLatencySamples();

        if (! engine.setEffects (target, std::move (chain)))
        {
            unsent[(size_t) target] = true;     // the command queue is full; try again shortly
            startTimer (50);
        }
    }

    void timerCallback() override
    {
        stopTimer();
        for (int t = 0; t < numTargets; ++t)
            if (std::exchange (unsent[(size_t) t], false))
                send (t);
    }

    void closeAllEditors()
    {
        for (auto& c : chains)
            for (auto& slot : c)
                slot->editorWindow = nullptr;

        for (auto& slot : instruments)
            if (slot != nullptr)
                slot->editorWindow = nullptr;
    }

    LooperEngine& engine;
    PluginLibrary& library;
    std::array<Slots, numTargets> chains;
    std::array<std::shared_ptr<FxSlot>, numTargets> instruments;     // monitorTarget's is always null
    std::array<bool, numTargets> unsent {};
};
