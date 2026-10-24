#pragma once
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
// The output mixer's arithmetic, shared by the engine (audio thread) and the mixer panel.
namespace Mixer
{
    // Fader: a position 0..1 with a cubic taper (finer near the top, like a console fader). 1 = +6 dB, unity sits at
    // about 0.79, 0 = silence. MIDI maps CC 0..127 straight onto the position.
    constexpr float maxGain = 2.0f;

    inline float faderToGain (float pos) noexcept
    {
        pos = juce::jlimit (0.0f, 1.0f, pos);
        return maxGain * pos * pos * pos;
    }

    inline float gainToFader (float gain) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, std::cbrt (juce::jmax (0.0f, gain) / maxGain));
    }

    inline float unityFader() noexcept { return gainToFader (1.0f); }

    inline float faderToDb (float pos) noexcept { return juce::Decibels::gainToDecibels (faderToGain (pos), -100.0f); }
    inline float dbToFader (float db) noexcept  { return db <= -100.0f ? 0.0f : gainToFader (juce::Decibels::decibelsToGain (db)); }

    inline juce::String faderText (float pos)
    {
        const float db = faderToDb (pos);
        if (db <= -100.0f)
            return "-inf";

        return (db > 0.05f ? "+" : "") + juce::String (db, 1);
    }

    // Constant-power pan, -1 (left) .. 1 (right). Centre is unity on both sides; hard over is +3 dB on one side and
    // silence on the other, so the total power stays the same wherever the pan is.
    inline void panGains (float pan, float& left, float& right) noexcept
    {
        const float angle = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        left  = juce::MathConstants<float>::sqrt2 * std::cos (angle);
        right = juce::MathConstants<float>::sqrt2 * std::sin (angle);
    }

    // CC 64 is dead centre; 0 is hard left, 127 hard right.
    inline float panFromMidi (int value) noexcept
    {
        value = juce::jlimit (0, 127, value);
        return value <= 64 ? (float) (value - 64) / 64.0f : (float) (value - 64) / 63.0f;
    }

    inline float panToMidi (float pan) noexcept
    {
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        return pan <= 0.0f ? 64.0f + pan * 64.0f : 64.0f + pan * 63.0f;
    }

    inline juce::String panText (float pan)
    {
        const int percent = juce::roundToInt (pan * 100.0f);
        return percent == 0 ? juce::String ("C") : (percent < 0 ? "L" : "R") + juce::String (std::abs (percent));
    }

    //==========================================================================
    // Normalise on load: the gain that brings a sound to targetRmsDb, measured over its audible parts only (10 ms
    // windows quieter than gateDb are left out, so a short hit with a long silent tail isn't boosted for the silence),
    // but never so far that a peak goes over ceilingDb, and never more than maxBoostDb (a near-silent file stays quiet).
    constexpr float targetRmsDb = -18.0f;
    constexpr float ceilingDb   = -1.0f;
    constexpr float gateDb      = -50.0f;
    constexpr float maxBoostDb  = 24.0f;

    inline float normaliseGain (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        const int numCh = audio.getNumChannels(), len = audio.getNumSamples();
        if (numCh == 0 || len == 0)
            return 1.0f;

        const int window = juce::jmax (1, (int) (0.010 * sampleRate));
        const double gate = std::pow (10.0, gateDb / 10.0);      // as a mean square
        double sum = 0.0;
        juce::int64 counted = 0;
        float peak = 0.0f;

        for (int start = 0; start < len; start += window)
        {
            const int n = juce::jmin (window, len - start);
            double windowSum = 0.0;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* d = audio.getReadPointer (ch, start);
                for (int i = 0; i < n; ++i)
                {
                    windowSum += (double) d[i] * d[i];
                    peak = juce::jmax (peak, std::abs (d[i]));
                }
            }

            if (windowSum / (double) (n * numCh) >= gate)
            {
                sum += windowSum;
                counted += n * numCh;
            }
        }

        if (counted == 0 || peak <= 0.0f)
            return 1.0f;

        const double rms = std::sqrt (sum / (double) counted);
        const double gain = juce::jmin ((double) juce::Decibels::decibelsToGain (targetRmsDb) / rms,
                                        (double) juce::Decibels::decibelsToGain (ceilingDb) / peak,
                                        (double) juce::Decibels::decibelsToGain (maxBoostDb));
        return (float) gain;
    }

    inline std::shared_ptr<const juce::AudioBuffer<float>> normalised (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        auto out = std::make_shared<juce::AudioBuffer<float>> (audio);
        out->applyGain (normaliseGain (audio, sampleRate));
        return out;
    }
}
