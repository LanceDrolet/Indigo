#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

//==============================================================================
// One effect plugin in a chain. Shared between the UI's copy of the chain (PadEffects) and the snapshot
// the engine is running, so a plugin keeps its state when the chain around it is edited. Only ever
// deleted on the message thread (the engine hands retired snapshots back through its event queue).
struct FxSlot
{
    juce::PluginDescription description;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    bool mono = false;                          // mono in/out: fed L+R, its output copied to both sides
    bool instrument = false;                    // played from MIDI; its output replaces the audio coming in
    bool routed = false;                        // any other layout: run through `scratch` (see FxChain::process)
    int numIns = 2, numOuts = 2;                // all the plugin's channels once configureBuses() has settled them
    int insUsed = 2, outsUsed = 2;              // how many of them carry Indigo's audio (see configureBuses)
    juce::AudioBuffer<float> scratch;           // routed only; sized in prepare()

    // Rate / block size the plugin was last prepared at. Written under LooperEngine::getFxPrepareLock().
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<int> preparedBlock { 0 };

    std::unique_ptr<juce::DocumentWindow> editorWindow;     // UI only; declared after `plugin`, so it goes first

    // Main buses stereo in and out (or mono in and out; an instrument stereo or mono out). A plugin that can't do
    // either (LV2s can't change layout at all), or has extra buses, keeps its own layout and is routed: it gets L/R
    // (or L+R) on its first inputs, and its outputs are folded down to stereo. False only if it has no audio output.
    // Extra buses (sidechains, multi-outs) are left on: some plugins write to every output whether or not the host
    // switched it off (AI_Bass_Mini through yabridge overran the buffer). Their inputs get silence. A VST3 has the
    // full mix on its main output, so only that is used; an LV2 multi-out (e.g. a drum kit's mics) splits it across
    // buses, so they are all summed. Set `instrument` first.
    bool configureBuses()
    {
        auto& p = *plugin;

        auto tryLayout = [&p] (const juce::AudioChannelSet& set)
        {
            auto layout = p.getBusesLayout();
            if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
                return false;

            layout.inputBuses.getReference (0) = set;
            layout.outputBuses.getReference (0) = set;
            return p.setBusesLayout (layout);
        };

        // An instrument only needs an output: switch its input off if it lets us, else give it the same layout.
        auto tryInstrument = [&p, &tryLayout] (const juce::AudioChannelSet& set)
        {
            auto layout = p.getBusesLayout();
            if (layout.outputBuses.isEmpty())
                return false;

            layout.outputBuses.getReference (0) = set;
            if (layout.inputBuses.isEmpty())
                return p.setBusesLayout (layout);

            layout.inputBuses.getReference (0) = juce::AudioChannelSet::disabled();
            return p.setBusesLayout (layout) || tryLayout (set);
        };

        auto attempt = [&] (const juce::AudioChannelSet& set) { return instrument ? tryInstrument (set) : tryLayout (set); };

        if (! attempt (juce::AudioChannelSet::stereo()))
            attempt (juce::AudioChannelSet::mono());

        numIns = p.getTotalNumInputChannels();
        numOuts = p.getTotalNumOutputChannels();
        const int mainIns  = p.getBusCount (true)  > 0 ? p.getChannelCountOfBus (true, 0)  : 0;
        const int mainOuts = p.getBusCount (false) > 0 ? p.getChannelCountOfBus (false, 0) : 0;
        const bool vst3 = p.getPluginDescription().pluginFormatName == "VST3";
        insUsed  = vst3 ? mainIns  : numIns;
        outsUsed = vst3 ? mainOuts : numOuts;
        if (outsUsed == 0)
            return false;

        const bool extraBuses = numIns != insUsed || numOuts != outsUsed;
        const bool inputFits = insUsed == outsUsed || (instrument && insUsed == 0);
        mono = ! extraBuses && outsUsed == 1 && inputFits;
        routed = extraBuses || ! (inputFits && outsUsed <= 2);
        return true;
    }

    // For the effects list: empty for plain stereo.
    juce::String channelInfo() const
    {
        if (! routed)
            return mono ? "mono" : juce::String();

        return (numIns > 0 && ! instrument ? juce::String (numIns) + " in / " : juce::String())
               + juce::String (numOuts) + " out";
    }

    // Caller holds the prepare lock, and the plugin is not in a chain the engine is running.
    void prepare (double rate, int block)
    {
        plugin->setNonRealtime (false);
        plugin->prepareToPlay (rate, block);
        if (routed)
            scratch.setSize (juce::jmax (numIns, numOuts), block);

        // One block of silence: an LV2 plugin only reports its latency (through JUCE) after it has run
        juce::AudioBuffer<float> silence (juce::jmax (1, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), block);
        silence.clear();
        juce::MidiBuffer noMidi;
        plugin->processBlock (silence, noMidi);
        preparedRate = rate;
        preparedBlock = block;
    }

    bool isPreparedFor (double rate, int block) const noexcept
    {
        return juce::exactlyEqual (preparedRate.load(), rate) && preparedBlock.load() == block;
    }
};

//==============================================================================
// An ordered list of effects for one pad (or the input monitor), as the audio thread runs it. Built on the
// message thread with every plugin already prepared at `rate` / `block`, then handed to the engine, which
// owns it until it is replaced. The audio thread never allocates or frees anything here.
struct FxChain
{
    std::vector<std::shared_ptr<FxSlot>> slots;
    double rate = 0.0;
    int block = 0;
    int latency = 0;                // getLatencySamples() when it was sent
    int effectsLatency = 0;         // the part of it after the instrument: the pad plays its clip this much early
    bool instrument = false;        // slots[0] is an instrument (it's always first)
    juce::MidiBuffer midi;          // what each plugin gets: the notes for the instrument, else empty

    FxChain() { midi.ensureSize (4096); }

    // Audio thread. Runs the 2-channel buffer through the plugins from slots[first] up to (not including)
    // slots[end] in place, in pieces no longer than the block size the plugins were prepared for. An instrument
    // ignores the buffer's audio and plays `notes` (sample positions within the buffer) into it.
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const juce::MidiBuffer* notes = nullptr,
                  size_t first = 0, size_t end = SIZE_MAX) noexcept
    {
        end = juce::jmin (end, slots.size());

        for (int start = 0; start < numSamples; start += block)
        {
            const int n = juce::jmin (block, numSamples - start);
            float* chans[2] = { buffer.getWritePointer (0, start), buffer.getWritePointer (1, start) };

            for (size_t i = first; i < end; ++i)
            {
                auto& s = slots[i];
                auto& p = *s->plugin;
                const juce::ScopedLock sl (p.getCallbackLock());

                midi.clear();

                if (s->instrument)
                {
                    juce::FloatVectorOperations::clear (chans[0], n);
                    juce::FloatVectorOperations::clear (chans[1], n);

                    if (notes != nullptr)
                        midi.addEvents (*notes, start, n, -start);
                }

                if (p.isSuspended())
                    continue;

                if (s->routed)
                {
                    processRouted (*s, chans, n);
                }
                else if (s->instrument)
                {
                    juce::AudioBuffer<float> view (chans, s->mono ? 1 : 2, n);
                    p.processBlock (view, midi);

                    if (s->mono)
                        juce::FloatVectorOperations::copy (chans[1], chans[0], n);
                }
                else if (s->mono)
                {
                    juce::FloatVectorOperations::add (chans[0], chans[1], n);
                    juce::FloatVectorOperations::multiply (chans[0], 0.5f, n);
                    juce::AudioBuffer<float> view (chans, 1, n);
                    p.processBlock (view, midi);
                    juce::FloatVectorOperations::copy (chans[1], chans[0], n);
                }
                else
                {
                    juce::AudioBuffer<float> view (chans, 2, n);
                    p.processBlock (view, midi);
                }
            }
        }
    }

    // A plugin with a layout of its own: the stereo pair goes in on its first inputs (L+R if it has just one;
    // nothing for an instrument), and the outputs it uses come back as stereo: 1 to both sides, 2 as they are, more
    // (multi-outs like a drum kit's mics) all summed into both sides. Extra inputs and outputs stay silent / unused.
    void processRouted (FxSlot& s, float* const* chans, int n) noexcept
    {
        auto& buf = s.scratch;
        buf.clear (0, n);

        if (! s.instrument)
        {
            if (s.insUsed == 1)
            {
                buf.copyFrom (0, 0, chans[0], n, 0.5f);
                buf.addFrom (0, 0, chans[1], n, 0.5f);
            }
            else if (s.insUsed >= 2)
            {
                buf.copyFrom (0, 0, chans[0], n);
                buf.copyFrom (1, 0, chans[1], n);
            }
        }

        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), n);
        s.plugin->processBlock (view, midi);

        if (s.outsUsed == 2)
        {
            juce::FloatVectorOperations::copy (chans[0], buf.getReadPointer (0), n);
            juce::FloatVectorOperations::copy (chans[1], buf.getReadPointer (1), n);
            return;
        }

        juce::FloatVectorOperations::copy (chans[0], buf.getReadPointer (0), n);
        for (int ch = 1; ch < s.outsUsed; ++ch)
            juce::FloatVectorOperations::add (chans[0], buf.getReadPointer (ch), n);
        juce::FloatVectorOperations::copy (chans[1], chans[0], n);
    }

    // While the device is stopped (LooperEngine::prepareToPlay), with the prepare lock held.
    void reprepare (double newRate, int newBlock)
    {
        for (auto& s : slots)
            s->prepare (newRate, newBlock);

        rate = newRate;
        block = newBlock;
    }

    int getLatencySamples (size_t first = 0) const
    {
        int total = 0;
        for (size_t i = first; i < slots.size(); ++i)
            total += slots[i]->plugin->getLatencySamples();
        return total;
    }
};
