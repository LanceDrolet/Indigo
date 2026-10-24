#pragma once
#include <JuceHeader.h>
#include "signalsmith-stretch/signalsmith-stretch.h"

// Returns a copy of `in` time-stretched (pitch preserved) to exactly `outLen` samples.
// Neighbouring copies of the loop are fed in as context and then trimmed off, so the
// loop seam stays clean. Offline: run it on a background thread.
inline juce::AudioBuffer<float> stretchLoopToLength (const juce::AudioBuffer<float>& in,
                                                     int outLen, double sampleRate)
{
    const int numCh = in.getNumChannels();
    const int n = in.getNumSamples();

    juce::AudioBuffer<float> result (numCh, outLen);
    result.clear();

    if (n <= 0 || outLen <= 0)
        return result;

    const double ratio = (double) outLen / (double) n;

    // Already at the right tempo: copy (trim or zero-pad the odd sample) rather than add artefacts.
    if (std::abs (ratio - 1.0) < 0.0005)
    {
        for (int ch = 0; ch < numCh; ++ch)
            result.copyFrom (ch, 0, in, ch, 0, juce::jmin (n, outLen));
        return result;
    }

    const int ctx    = juce::jmin (n, (int) (2.0 * sampleRate));
    const int ctxOut = (int) std::llround ((double) ctx * ratio);
    const int extLen = ctx + n + ctx;
    const int resLen = ctxOut + outLen + ctxOut;

    // [tail of loop][loop][head of loop]
    juce::AudioBuffer<float> ext (numCh, extLen), res (numCh, resLen);
    res.clear();

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* dst = ext.getWritePointer (ch);
        auto* src = in.getReadPointer (ch);
        for (int i = 0; i < extLen; ++i)
            dst[i] = src[((i - ctx) % n + n) % n];
    }

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault (numCh, (float) sampleRate);
    stretch.exact (ext.getArrayOfReadPointers(), extLen, res.getArrayOfWritePointers(), resLen);

    for (int ch = 0; ch < numCh; ++ch)
        result.copyFrom (ch, 0, res, ch, ctxOut, outLen);

    return result;
}
