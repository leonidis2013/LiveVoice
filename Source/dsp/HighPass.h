#pragma once

#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"

// Zero-latency high-pass filter, first in the chain (right after input trim), so the PSE,
// the feedback detector and everything after it never hear rumble or handling noise.
// Butterworth cascades for 6..48 dB/oct. Frequency changes glide (no zipper noise).
namespace dsp
{
    class HighPass
    {
    public:
        static constexpr int maxChannels = 2, maxStages = 4;
        static constexpr int slopes[] { 6, 12, 18, 24, 36, 48 };

        struct Settings { bool on = true; float freq = 80.0f; int slopeIndex = 3; };

        // Butterworth cascade for a slope (dB/oct) at freq.
        static int design (int slopeDb, double freq, double fs, SvfCoeffs out[maxStages])
        {
            auto second = [&] (SvfCoeffs& c, double q) { c.highPass (freq, q, fs); };
            switch (slopeDb)
            {
                case 6:  out[0].highPass1 (freq, fs); return 1;
                case 12: second (out[0], 0.70710678); return 1;
                case 18: out[0].highPass1 (freq, fs); second (out[1], 1.0); return 2;
                case 36: second (out[0], 0.51763809); second (out[1], 0.70710678); second (out[2], 1.93185165); return 3;
                case 48: second (out[0], 0.50979558); second (out[1], 0.60134489); second (out[2], 0.89997622); second (out[3], 2.56291545); return 4;
                default: second (out[0], 0.54119610); second (out[1], 1.30656296); return 2;   // 24
            }
        }

        // Response in dB at f, for drawing.
        static float responseDb (const Settings& s, double f, double fs)
        {
            if (! s.on) return 0.0f;
            SvfCoeffs c[maxStages];
            const int n = design (slopes[juce::jlimit (0, 5, s.slopeIndex)], s.freq, fs, c);
            double db = 0.0;
            for (int i = 0; i < n; ++i) db += c[i].magnitudeDb (f, fs);
            return (float) db;
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            reset();
        }

        void reset()
        {
            for (auto& st : state) for (auto& c : st) c.reset();
            currentFreq = -1.0f; currentSlope = -1;
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            if (! settings.on)
            {
                if (wasOn) { reset(); wasOn = false; }
                return;
            }
            wasOn = true;
            const int slope = slopes[juce::jlimit (0, 5, settings.slopeIndex)];
            const int nCh = std::min (channels, buffer.getNumChannels());

            // glide toward the set frequency in sub-blocks of 32 samples (about 30 ms to settle)
            for (int start = 0; start < numSamples; start += 32)
            {
                const int n = std::min (32, numSamples - start);
                if (slope != currentSlope)
                {
                    for (auto& st : state) for (auto& c : st) c.reset();
                    currentSlope = slope;
                    if (currentFreq < 0.0f) currentFreq = settings.freq;
                    numStages = design (slope, currentFreq, fs, coeffs.data());
                }
                if (currentFreq < 0.0f) currentFreq = settings.freq;
                if (! juce::exactlyEqual (currentFreq, settings.freq))
                {
                    const float ratio = settings.freq / currentFreq;
                    currentFreq = std::abs (std::log (ratio)) < 0.002f ? settings.freq : currentFreq * std::pow (ratio, 0.2f);
                    numStages = design (slope, currentFreq, fs, coeffs.data());
                }
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto* x = buffer.getWritePointer (ch, start);
                    for (int st = 0; st < numStages; ++st)
                    {
                        auto& s = state[(size_t) st][(size_t) ch];
                        const auto& c = coeffs[(size_t) st];
                        for (int i = 0; i < n; ++i) x[i] = s.process (x[i], c);
                    }
                }
            }
        }

    private:
        Settings settings;
        double fs = 48000.0;
        int channels = 2, numStages = 1, currentSlope = -1;
        float currentFreq = -1.0f;
        bool wasOn = false;
        std::array<SvfCoeffs, maxStages> coeffs;
        std::array<std::array<SvfState, maxChannels>, maxStages> state;
    };
}
