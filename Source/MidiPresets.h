#pragma once
#include <JuceHeader.h>
#include <array>
#include "LooperEngine.h"

//==============================================================================
// A complete MIDI mapping: what each pad, the REC toggle and the mixer controls respond to.
struct MidiMapping
{
    static constexpr int numMixerTargets = LooperEngine::numBindingTargets - LooperEngine::mixerControlBase;

    std::array<MidiBinding, LooperEngine::numPads> pads;
    MidiBinding rec;
    std::array<MidiBinding, numMixerTargets> mixer;     // [target - LooperEngine::mixerControlBase]
    int performanceChannel = 0;     // 1..16, 0 = none (see LooperEngine::setPerformanceChannel)

    // Pads on consecutive notes from `firstNote`; REC unmapped.
    static MidiMapping chromatic (int firstNote = 36)
    {
        MidiMapping m;
        for (int i = 0; i < LooperEngine::numPads; ++i)
            m.pads[(size_t) i] = MidiBinding::note (firstNote + i);
        return m;
    }

    static MidiMapping fromEngine (const LooperEngine& e)
    {
        MidiMapping m;
        for (int i = 0; i < LooperEngine::numPads; ++i)
            m.pads[(size_t) i] = e.getBinding (i);
        m.rec = e.getBinding (LooperEngine::recControl);
        for (int i = 0; i < numMixerTargets; ++i)
            m.mixer[(size_t) i] = e.getBinding (LooperEngine::mixerControlBase + i);
        m.performanceChannel = e.getPerformanceChannel();
        return m;
    }

    void applyTo (LooperEngine& e) const
    {
        for (int i = 0; i < LooperEngine::numPads; ++i)
            e.setBinding (i, pads[(size_t) i]);
        e.setBinding (LooperEngine::recControl, rec);
        for (int i = 0; i < numMixerTargets; ++i)
            e.setBinding (LooperEngine::mixerControlBase + i, mixer[(size_t) i]);
        e.setPerformanceChannel (performanceChannel);
    }

    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("MidiMapping");
        xml->setAttribute ("performanceChannel", performanceChannel);

        auto add = [&] (const char* tag, int index, const MidiBinding& b) -> juce::XmlElement*
        {
            auto* el = xml->createNewChildElement (tag);
            if (index >= 0) el->setAttribute ("index", index);
            el->setAttribute ("kind", b.kind == MidiBinding::Kind::note ? "note"
                                    : b.kind == MidiBinding::Kind::cc   ? "cc" : "none");
            el->setAttribute ("number", b.number);
            el->setAttribute ("channel", b.channel);
            return el;
        };

        add ("Rec", -1, rec);
        for (int i = 0; i < LooperEngine::numPads; ++i)
            add ("Pad", i, pads[(size_t) i]);

        // Mixer controls: only the mapped ones
        for (int i = 0; i < LooperEngine::numPads; ++i)
            for (int c = 0; c < LooperEngine::numMixerControls; ++c)
                if (const auto& b = mixer[(size_t) (i * LooperEngine::numMixerControls + c)]; b.kind != MidiBinding::Kind::none)
                    add ("Mixer", i, b)->setAttribute ("control", mixerControlNames[c]);

        if (const auto& b = mixer[(size_t) (LooperEngine::muteAllControl - LooperEngine::mixerControlBase)]; b.kind != MidiBinding::Kind::none)
            add ("MuteAll", -1, b);
        if (const auto& b = mixer[(size_t) (LooperEngine::unmuteAllControl - LooperEngine::mixerControlBase)]; b.kind != MidiBinding::Kind::none)
            add ("UnmuteAll", -1, b);

        return xml;
    }

    // Missing entries are left unmapped; a missing channel (older files) means any channel.
    static MidiMapping fromXml (const juce::XmlElement& xml)
    {
        MidiMapping m;
        m.performanceChannel = juce::jlimit (0, 16, xml.getIntAttribute ("performanceChannel"));

        auto read = [] (const juce::XmlElement& el)
        {
            const auto kind = el.getStringAttribute ("kind");
            const int n = el.getIntAttribute ("number");
            const int ch = el.getIntAttribute ("channel");
            return kind == "note" ? MidiBinding::note (n, ch) : kind == "cc" ? MidiBinding::cc (n, ch) : MidiBinding();
        };

        for (auto* el : xml.getChildIterator())
        {
            if (el->hasTagName ("Rec"))
                m.rec = read (*el);
            else if (el->hasTagName ("Pad") && juce::isPositiveAndBelow (el->getIntAttribute ("index", -1), LooperEngine::numPads))
                m.pads[(size_t) el->getIntAttribute ("index")] = read (*el);
            else if (el->hasTagName ("MuteAll"))
                m.mixer[(size_t) (LooperEngine::muteAllControl - LooperEngine::mixerControlBase)] = read (*el);
            else if (el->hasTagName ("UnmuteAll"))
                m.mixer[(size_t) (LooperEngine::unmuteAllControl - LooperEngine::mixerControlBase)] = read (*el);
            else if (el->hasTagName ("Mixer") && juce::isPositiveAndBelow (el->getIntAttribute ("index", -1), LooperEngine::numPads))
            {
                const int c = juce::StringArray (mixerControlNames, LooperEngine::numMixerControls).indexOf (el->getStringAttribute ("control"));
                const int target = LooperEngine::mixerTarget (el->getIntAttribute ("index"), (MixerControl) c);
                auto b = read (*el);

                if (c >= 0 && ! (LooperEngine::isContinuous (target) && b.kind == MidiBinding::Kind::note))
                    m.mixer[(size_t) (target - LooperEngine::mixerControlBase)] = b;
            }
        }
        return m;
    }

private:
    static constexpr const char* mixerControlNames[LooperEngine::numMixerControls] = { "volume", "pan", "mute", "solo" };
};

//==============================================================================
// Named mapping presets (one per controller / layout), plus the mapping in use, which is restored at
// startup. Stored as XML under ~/.config/Indigo (the platform's application data folder).
class MidiPresets
{
public:
    juce::String currentName;       // preset the current mapping came from / was last saved as ("" = none)

    static juce::File folder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Indigo");
    }

    static juce::File presetFolder() { return folder().getChildFile ("MidiPresets"); }

    juce::StringArray list() const
    {
        juce::StringArray names;
        for (auto& f : presetFolder().findChildFiles (juce::File::findFiles, false, "*.xml"))
            names.add (f.getFileNameWithoutExtension());

        names.sortNatural();
        return names;
    }

    // Returns the name actually used (made safe for a file name), or "" on failure.
    juce::String save (const juce::String& name, const MidiMapping& m)
    {
        const auto safeName = juce::File::createLegalFileName (name.trim());
        if (safeName.isEmpty() || ! presetFolder().createDirectory())
            return {};

        if (! m.toXml()->writeTo (presetFile (safeName)))
            return {};

        currentName = safeName;
        return safeName;
    }

    bool load (const juce::String& name, MidiMapping& m)
    {
        auto xml = juce::XmlDocument::parse (presetFile (name));
        if (xml == nullptr || ! xml->hasTagName ("MidiMapping"))
            return false;

        m = MidiMapping::fromXml (*xml);
        currentName = name;
        return true;
    }

    bool remove (const juce::String& name)
    {
        if (name == currentName)
            currentName = {};

        return presetFile (name).deleteFile();
    }

    //==== The mapping in use ====
    void saveCurrent (const MidiMapping& m) const
    {
        auto xml = m.toXml();
        xml->setAttribute ("preset", currentName);

        if (folder().createDirectory())
            xml->writeTo (folder().getChildFile ("CurrentMidiMapping.xml"));
    }

    // False if nothing has been saved yet (first run).
    bool loadCurrent (MidiMapping& m)
    {
        auto xml = juce::XmlDocument::parse (folder().getChildFile ("CurrentMidiMapping.xml"));
        if (xml == nullptr || ! xml->hasTagName ("MidiMapping"))
            return false;

        m = MidiMapping::fromXml (*xml);
        currentName = xml->getStringAttribute ("preset");
        return true;
    }

private:
    static juce::File presetFile (const juce::String& name) { return presetFolder().getChildFile (name + ".xml"); }
};
