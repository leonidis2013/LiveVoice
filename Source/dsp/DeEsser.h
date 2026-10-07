#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"

// Zero-latency de-esser. A high-pass sidechain measures sibilance; reduction
// is either a dynamic high shelf (Split) or full-band gain (Wideband).
// Detection is either absolute (sibilance level in dBFS) or relative
// (sibilance level compared with the whole voice), so a quiet talker and a
// loud talker get the same treatment.
// It also publishes a short history of sibilance level and gain reduction
// for the histogram strip.
namespace dsp
{
    class DeEsser
    {
    public:
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 16;
        static constexpr int historySize = 1024;
        static constexpr int historyBlockMs = 10;    // one histogram column per 10 ms

        struct Settings
        {
            bool on = true, split = true;
            float freq = 6000.0f, thresholdDb = -30.0f, rangeDb = 8.0f, releaseMs = 80.0f;
            bool relative = false;
            float relThresholdDb = -10.0f;     // sibilance band minus full band, in dB
        };

        static constexpr float relativeGateDb = -60.0f;   // no reduction on near-silence

        struct HistoryPoint { float levelDb; float grDb; };

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            fullValid = false;
            channels = juce::jlimit (1, maxChannels, numChannels);
            samplesPerColumn = std::max (1, (int) (fs * historyBlockMs / 1000.0));
            reset();
            for (auto& h : history) { h.level.store (-100.0f); h.gr.store (0.0f); }
        }

        void reset()
        {
            for (auto& s : shelf) s.reset();
            det.reset();
            fullDet.reset();
            envFull = 0.0f;
            envPow = 0.0f; grDb = 0.0f; counter = 0;
            colCount = 0; colMaxLevel = -100.0f; colMaxGr = 0.0f;
            lastFreq = -1.0f;
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            if (! juce::exactlyEqual (settings.freq, lastFreq))
            {
                detCoeffs.highPass (settings.freq, 0.707, fs);
                shelfG0 = SvfCoeffs::prewarp (settings.freq, fs);
                lastFreq = settings.freq;
            }
            if (! fullValid) { fullCoeffs.highPass (80.0, 0.707, fs); fullValid = true; }

            const float att = timeCoeff (0.5f, fs);
            const float rel = timeCoeff (settings.releaseMs, fs);
            const float grSmooth = std::exp (-1.0f / (0.0005f * (float) (fs / controlInterval)));

            for (int i = 0; i < numSamples; ++i)
            {
                float l = L[i];
                float r = R != nullptr ? R[i] : 0.0f;

                const float mono = R != nullptr ? 0.5f * (l + r) : l;
                const float d = det.process (mono, detCoeffs);
                const float p = d * d;
                envPow = (p > envPow ? att : rel) * envPow + (1.0f - (p > envPow ? att : rel)) * p;
                if (settings.relative)
                {
                    const float f = fullDet.process (mono, fullCoeffs);
                    const float pf = f * f;
                    const float c = pf > envFull ? att : rel;
                    envFull = c * envFull + (1.0f - c) * pf;
                }

                if (counter == 0)
                {
                    const float sibDb = 10.0f * std::log10 (std::max (envPow, 1.0e-12f)) + 3.0103f;
                    float threshold = settings.thresholdDb;
                    if (settings.relative)
                    {
                        const float fullDb = 10.0f * std::log10 (std::max (envFull, 1.0e-12f)) + 3.0103f;
                        levelDb = fullDb > relativeGateDb ? sibDb - fullDb : -100.0f;
                        threshold = settings.relThresholdDb;
                    }
                    else
                        levelDb = sibDb;

                    float target = 0.0f;
                    if (settings.on)
                    {
                        const float over = levelDb - threshold;
                        if (over > 0.0f) target = std::min (settings.rangeDb, over * 0.8f);
                    }
                    grDb = grSmooth * grDb + (1.0f - grSmooth) * target;

                    if (settings.split)
                        shelfCoeffs.highShelfFromG (shelfG0, 0.707, -grDb);
                    wideGain = settings.split ? 1.0f : dbToGain (-grDb);
                }
                counter = (counter + 1) % controlInterval;

                if (settings.on)
                {
                    if (settings.split)
                    {
                        l = shelf[0].process (l, shelfCoeffs);
                        if (R != nullptr) r = shelf[1].process (r, shelfCoeffs);
                    }
                    else
                    {
                        l *= wideGain;
                        r *= wideGain;
                    }
                }

                L[i] = l;
                if (R != nullptr) R[i] = r;

                colMaxLevel = std::max (colMaxLevel, levelDb);
                colMaxGr = std::max (colMaxGr, settings.on ? grDb : 0.0f);
                if (++colCount >= samplesPerColumn)
                {
                    const int w = writeIndex.load (std::memory_order_relaxed);
                    history[(size_t) w].level.store (colMaxLevel, std::memory_order_relaxed);
                    history[(size_t) w].gr.store (colMaxGr, std::memory_order_relaxed);
                    writeIndex.store ((w + 1) % historySize, std::memory_order_release);
                    colCount = 0; colMaxLevel = -100.0f; colMaxGr = 0.0f;
                }
            }

            currentGr.store (settings.on ? grDb : 0.0f, std::memory_order_relaxed);
        }

        // Copies the most recent `count` columns, oldest first.
        void readHistory (HistoryPoint* dest, int count) const noexcept
        {
            count = std::min (count, historySize);
            const int w = writeIndex.load (std::memory_order_acquire);
            for (int i = 0; i < count; ++i)
            {
                const int idx = (w - count + i + historySize) % historySize;
                dest[i] = { history[(size_t) idx].level.load (std::memory_order_relaxed),
                            history[(size_t) idx].gr.load (std::memory_order_relaxed) };
            }
        }

        float getCurrentGainReductionDb() const noexcept { return currentGr.load (std::memory_order_relaxed); }

    private:
        struct Slot { std::atomic<float> level { -100.0f }, gr { 0.0f }; };

        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0;
        SvfCoeffs detCoeffs, shelfCoeffs, fullCoeffs;
        SvfState det, fullDet;
        float envFull = 0.0f;
        bool fullValid = false;
        std::array<SvfState, maxChannels> shelf;
        double shelfG0 = 0.4;
        float envPow = 0.0f, grDb = 0.0f, levelDb = -100.0f, wideGain = 1.0f, lastFreq = -1.0f;

        int samplesPerColumn = 480, colCount = 0;
        float colMaxLevel = -100.0f, colMaxGr = 0.0f;
        std::array<Slot, historySize> history;
        std::atomic<int> writeIndex { 0 };
        std::atomic<float> currentGr { 0.0f };
    };
}
