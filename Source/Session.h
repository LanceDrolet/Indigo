#pragma once
#include <JuceHeader.h>
#include <array>
#include "LooperEngine.h"
#include "MidiPresets.h"

//==============================================================================
// Pad modes as text, for the XML files.
inline juce::String padModeName (PadMode m)
{
    return m == PadMode::loop ? "loop" : m == PadMode::gate ? "gate" : "oneShot";
}

inline PadMode padModeFromName (const juce::String& s)
{
    return s == "loop" ? PadMode::loop : s == "gate" ? PadMode::gate : PadMode::oneShot;
}

//==============================================================================
// A template: how each pad behaves (one-shot / loop / gate, choke group, effects), without any sounds. The template in use is
// kept in ~/.config/Indigo/CurrentTemplate.xml so pad modes survive a restart; named templates are
// *.indigotemplate files (default folder ~/.config/Indigo/Templates).
struct PadTemplate
{
    static constexpr const char* fileExtension = ".indigotemplate";

    std::array<PadMode, LooperEngine::numPads> modes {};
    std::array<int, LooperEngine::numPads> chokes {};       // 0 = no choke group
    std::shared_ptr<const juce::XmlElement> effects;        // PadEffects::toXml(); null = none

    static juce::File folder()          { return MidiPresets::folder().getChildFile ("Templates"); }
    static juce::File currentFile()     { return MidiPresets::folder().getChildFile ("CurrentTemplate.xml"); }

    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("PadTemplate");

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            auto* el = xml->createNewChildElement ("Pad");
            el->setAttribute ("index", i);
            el->setAttribute ("mode", padModeName (modes[(size_t) i]));
            el->setAttribute ("choke", chokes[(size_t) i]);
        }

        if (effects != nullptr)
            xml->addChildElement (new juce::XmlElement (*effects));

        return xml;
    }

    // Pads missing from the file stay one-shot, with no choke group.
    static bool fromXml (const juce::XmlElement& xml, PadTemplate& t)
    {
        if (! xml.hasTagName ("PadTemplate"))
            return false;

        t = {};
        for (auto* el : xml.getChildWithTagNameIterator ("Pad"))
        {
            const int i = el->getIntAttribute ("index", -1);
            if (juce::isPositiveAndBelow (i, LooperEngine::numPads))
            {
                t.modes[(size_t) i]  = padModeFromName (el->getStringAttribute ("mode"));
                t.chokes[(size_t) i] = juce::jlimit (0, LooperEngine::numChokeGroups, el->getIntAttribute ("choke", 0));
            }
        }

        if (auto* fx = xml.getChildByName ("Effects"))
            t.effects = std::make_shared<const juce::XmlElement> (*fx);

        return true;
    }

    bool save (const juce::File& f) const
    {
        return f.getParentDirectory().createDirectory() && toXml()->writeTo (f);
    }

    static bool load (const juce::File& f, PadTemplate& t)
    {
        auto xml = juce::XmlDocument::parse (f);
        return xml != nullptr && fromXml (*xml, t);
    }
};

//==============================================================================
// A session: tempo, quantize, every pad's mode, choke group, loop length, sound and mixer strip, and the effects. Saved as a *.indigo file
// (default folder ~/Music/Indigo Sessions). Sounds that exist only in memory (recorded takes, overdubbed
// loops) are written as WAVs into a "<session name> Audio" folder next to it; other pads point at the
// file they were loaded from. Paths inside the session's folder are stored relative, so the session and
// its audio folder can be moved together.
struct Session
{
    static constexpr const char* fileExtension = ".indigo";

    struct Pad
    {
        PadMode mode = PadMode::oneShot;
        int choke = 0;          // 0 = no choke group
        int beats = 4;
        juce::String name;      // shown on the pad
        juce::File file;        // empty = no sound
        bool normalised = false;    // the sound was normalised when it was loaded (Normalise on load); do it again

        float fader = Mixer::unityFader();      // mixer strip (see LooperEngine's mixer)
        float pan = 0.0f;
        bool mute = false, solo = false;
    };

    double bpm = 120.0;
    int quantize = (int) Quantize::bar;
    std::array<Pad, LooperEngine::numPads> pads;
    std::shared_ptr<const juce::XmlElement> effects;        // PadEffects::toXml(); null = none

    static juce::File folder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Indigo Sessions");
    }

    static juce::File audioFolderFor (const juce::File& sessionFile)
    {
        return sessionFile.getSiblingFile (sessionFile.getFileNameWithoutExtension() + " Audio");
    }

    std::unique_ptr<juce::XmlElement> toXml (const juce::File& sessionFile) const
    {
        auto xml = std::make_unique<juce::XmlElement> ("IndigoSession");
        xml->setAttribute ("version", 1);
        xml->setAttribute ("bpm", bpm);
        xml->setAttribute ("quantize", quantize);

        const auto base = sessionFile.getParentDirectory();

        for (int i = 0; i < LooperEngine::numPads; ++i)
        {
            const auto& p = pads[(size_t) i];
            auto* el = xml->createNewChildElement ("Pad");
            el->setAttribute ("index", i);
            el->setAttribute ("mode", padModeName (p.mode));
            el->setAttribute ("choke", p.choke);
            el->setAttribute ("beats", p.beats);
            el->setAttribute ("volumeDb", juce::String (Mixer::faderToDb (p.fader), 2));     // -100 = silent
            el->setAttribute ("pan", juce::String (p.pan, 3));
            el->setAttribute ("mute", p.mute);
            el->setAttribute ("solo", p.solo);

            if (p.file != juce::File())
            {
                el->setAttribute ("name", p.name);
                el->setAttribute ("normalised", p.normalised);
                el->setAttribute ("file", p.file.isAChildOf (base) ? p.file.getRelativePathFrom (base)
                                                                   : p.file.getFullPathName());
            }
        }

        if (effects != nullptr)
            xml->addChildElement (new juce::XmlElement (*effects));

        return xml;
    }

    static bool fromXml (const juce::XmlElement& xml, const juce::File& sessionFile, Session& s)
    {
        if (! xml.hasTagName ("IndigoSession"))
            return false;

        s = {};
        s.bpm = xml.getDoubleAttribute ("bpm", 120.0);
        s.quantize = juce::jlimit (0, (int) Quantize::sixteenth, xml.getIntAttribute ("quantize", (int) Quantize::bar));

        const auto base = sessionFile.getParentDirectory();

        for (auto* el : xml.getChildWithTagNameIterator ("Pad"))
        {
            const int i = el->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, LooperEngine::numPads))
                continue;

            auto& p = s.pads[(size_t) i];
            p.mode  = padModeFromName (el->getStringAttribute ("mode"));
            p.choke = juce::jlimit (0, LooperEngine::numChokeGroups, el->getIntAttribute ("choke", 0));
            p.beats = juce::jmax (1, el->getIntAttribute ("beats", 4));
            p.name  = el->getStringAttribute ("name");
            p.normalised = el->getBoolAttribute ("normalised", false);
            p.fader = Mixer::dbToFader ((float) el->getDoubleAttribute ("volumeDb", 0.0));
            p.pan   = juce::jlimit (-1.0f, 1.0f, (float) el->getDoubleAttribute ("pan", 0.0));
            p.mute  = el->getBoolAttribute ("mute", false);
            p.solo  = el->getBoolAttribute ("solo", false);

            const auto path = el->getStringAttribute ("file");
            if (path.isNotEmpty())
                p.file = base.getChildFile (path);      // handles absolute paths too
        }

        if (auto* fx = xml.getChildByName ("Effects"))
            s.effects = std::make_shared<const juce::XmlElement> (*fx);

        return true;
    }
};
