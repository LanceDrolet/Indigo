#pragma once
#include <JuceHeader.h>
#include <atomic>

#if JUCE_LINUX && JUCE_JACK
namespace juce { extern StringArray jackOutputPortNames; }   // cmake/PatchJuce.cmake
#endif

//==============================================================================
// Indigo's outputs. Under JACK (PipeWire's included) it has named ports, so each source can be patched on its
// own in a patchbay, e.g. the metronome and preview to the headphones only:
//   main_L / main_R            pads, instruments and the monitored input; connected to the output chosen in
//                              Settings > Audio
//   metronome_L / metronome_R  the click (not connected at startup)
//   preview_L / preview_R      the file browser preview (not connected at startup)
//   pad_01_L / pad_01_R ...    with per-pad outputs on (Settings > Audio): a copy of each pad, before or after its
//   pad_16_L / pad_16_R        mixer strip, to record the pads on separate tracks (not connected at startup)
// On any other device (ALSA) everything is mixed onto the main channels, as before.
// The port names reach JACK through juce::jackOutputPortNames (cmake/PatchJuce.cmake); they are read when the
// device is created, so turning per-pad outputs on or off reopens it.
namespace OutputBuses
{
    enum Bus { main = 0, metronome = 2, preview = 4, firstPad = 6 };
    constexpr int numChannels = 6;
    constexpr int numPads = 16;     // LooperEngine::numPads

    // Set by the UI: whether the next JACK device gets the pad ports, and whether they carry the pads before
    // their fader, pan and mute / solo (else after).
    inline std::atomic<bool> perPad { false };
    inline std::atomic<bool> padsPreFader { false };

    inline juce::StringArray portNames()
    {
        juce::StringArray names { "main_L", "main_R", "metronome_L", "metronome_R", "preview_L", "preview_R" };

        if (perPad.load())
            for (int i = 1; i <= numPads; ++i)
                for (auto side : { "_L", "_R" })
                    names.add ("pad_" + juce::String (i).paddedLeft ('0', 2) + side);

        return names;
    }

    // Hands the port names to JUCE's JACK device (before one is created).
    inline void setJackPortNames()
    {
       #if JUCE_LINUX && JUCE_JACK
        juce::jackOutputPortNames = portNames();
       #endif
    }

    // Set by the UI when the device opens: true if it has the named ports.
    inline std::atomic<bool> separate { false };

    // Whether a buffer of `channels` channels has the buses on their own channels.
    inline bool isSeparate (int channels) noexcept  { return separate.load (std::memory_order_relaxed) && channels >= numChannels; }

    // The channels [first, end) that `bus` is played on: its own pair, or every channel when there are no buses.
    inline juce::Range<int> channels (Bus bus, int numBufferChannels) noexcept
    {
        return isSeparate (numBufferChannels) ? juce::Range<int> ((int) bus, (int) bus + 2)
                                              : juce::Range<int> (0, numBufferChannels);
    }

    // The pair pad `pad` is copied to, or an empty range when there are no pad ports.
    inline juce::Range<int> padChannels (int pad, int numBufferChannels) noexcept
    {
        const int first = (int) firstPad + 2 * pad;
        return isSeparate (numBufferChannels) && first + 2 <= numBufferChannels ? juce::Range<int> (first, first + 2)
                                                                                : juce::Range<int>();
    }

    //==========================================================================
    // Plays a stereo source into one bus (or, with no buses, into every channel as it would on its own).
    class Source : public juce::AudioSource
    {
    public:
        Source (juce::AudioSource& s, Bus b) : source (s), bus (b) {}

        void prepareToPlay (int blockSize, double rate) override
        {
            scratch.setSize (2, blockSize);
            source.prepareToPlay (blockSize, rate);
        }

        void releaseResources() override { source.releaseResources(); }

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            auto& out = *info.buffer;

            if (! isSeparate (out.getNumChannels()))
            {
                source.getNextAudioBlock (info);
                return;
            }

            const int n = info.numSamples;
            scratch.setSize (2, n, false, false, true);
            source.getNextAudioBlock (juce::AudioSourceChannelInfo (&scratch, 0, n));

            info.clearActiveBufferRegion();
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom ((int) bus + ch, info.startSample, scratch, ch, 0, n);
        }

    private:
        juce::AudioSource& source;
        const Bus bus;
        juce::AudioBuffer<float> scratch;
    };
}
