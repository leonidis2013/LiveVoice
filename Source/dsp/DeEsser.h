#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "LevelHistory.h"

// Zero-latency de-esser.
//   Band mode (default): a narrow band-pass sidechain measures the sibilance and
//     a dynamic bell cuts only that band (width set by Q).
//   Shelf mode: high-pass sidechain, dynamic high shelf (all highs).
//   Wideband mode: high-pass sidechain, whole signal ducks.
// Detection is absolute (band level in dBFS) or relative (band level compared
// with the whole voice). Listen (solo) outputs exactly what the detector hears.
namespace dsp
{
    class DeEsser
    {
    public:
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 16;
        static constexpr int historyColumnMs = 30;    // slower scroll: ~20 s across the strip
        static constexpr float relativeGateDb = -60.0f;   // no reduction on near-silence

        enum Mode { band = 0, shelf, wideband };

        struct Settings
        {
            bool on = true, relative = false, listen = false;
            int mode = band;
            float freq = 6500.0f, q = 3.0f, thresholdDb = -30.0f, rangeDb = 8.0f, releaseMs = 80.0f;
            float relThresholdDb = -18.0f;     // sibilance band minus full band, in dB
        };

        using HistoryPoint = LevelHistory::Point;

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            fullCoeffs.highPass (80.0, 0.707, fs);
            history.prepare (fs, historyColumnMs);
            reset();
        }

        void reset()
        {
            for (auto& s : redState) s.reset();
            for (auto& s : listenState) s.reset();
            det.reset(); fullDet.reset();
            envFull = 0.0f; envPow = 0.0f; grDb = 0.0f; counter = 0;
            lastFreq = lastQ = -1.0f; lastMode = -1;
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        // Response (dB) of the reduction filter for a given amount of gain reduction, for drawing.
        static float reductionResponseDb (const Settings& s, float gr, double freq, double sampleRate)
        {
            if (gr <= 0.0f) return 0.0f;
            SvfCoeffs c;
            if (s.mode == band)        c.bell (s.freq, s.q, -gr, sampleRate);
            else if (s.mode == shelf)  c.highShelf (s.freq, 0.707, -gr, sampleRate);
            else                       return -gr;
            return (float) c.magnitudeDb (freq, sampleRate);
        }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            if (! juce::exactlyEqual (settings.freq, lastFreq) || ! juce::exactlyEqual (settings.q, lastQ) || settings.mode != lastMode)
            {
                if (settings.mode == band) detCoeffs.bandPass (settings.freq, settings.q, fs);   // 0 dB at centre
                else                       detCoeffs.highPass (settings.freq, 0.707, fs);
                redG0 = SvfCoeffs::prewarp (settings.freq, fs);
                lastFreq = settings.freq; lastQ = settings.q; lastMode = settings.mode;
            }

            const float att = timeCoeff (0.5f, fs);
            const float rel = timeCoeff (settings.releaseMs, fs);
            const float grSmooth = std::exp (-1.0f / (0.0005f * (float) (fs / controlInterval)));
            const bool listen = settings.listen;

            for (int i = 0; i < numSamples; ++i)
            {
                float l = L[i];
                float r = R != nullptr ? R[i] : 0.0f;

                const float mono = R != nullptr ? 0.5f * (l + r) : l;
                const float d = det.process (mono, detCoeffs);
                const float p = d * d;
                const float ce = p > envPow ? att : rel;
                envPow = ce * envPow + (1.0f - ce) * p;
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

                    if (settings.mode == band)       redCoeffs.bellFromG (redG0, settings.q, -grDb);
                    else if (settings.mode == shelf) redCoeffs.highShelfFromG (redG0, 0.707, -grDb);
                    wideGain = settings.mode == wideband ? dbToGain (-grDb) : 1.0f;
                }
                counter = (counter + 1) % controlInterval;

                if (listen)
                {
                    // solo: hear the sibilance band exactly as the detector does
                    l = listenState[0].process (l, detCoeffs);
                    if (R != nullptr) r = listenState[1].process (r, detCoeffs);
                }
                else if (settings.on)
                {
                    if (settings.mode == wideband)
                    {
                        l *= wideGain;
                        r *= wideGain;
                    }
                    else
                    {
                        l = redState[0].process (l, redCoeffs);
                        if (R != nullptr) r = redState[1].process (r, redCoeffs);
                    }
                }

                L[i] = l;
                if (R != nullptr) R[i] = r;

                history.add (levelDb, settings.on ? grDb : 0.0f);
            }

            currentGr.store (settings.on ? grDb : 0.0f, std::memory_order_relaxed);
        }

        void readHistory (HistoryPoint* dest, int count) const noexcept { history.read (dest, count); }
        float getCurrentGainReductionDb() const noexcept { return currentGr.load (std::memory_order_relaxed); }

    private:
        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0, lastMode = -1;
        SvfCoeffs detCoeffs, redCoeffs, fullCoeffs;
        SvfState det, fullDet;
        std::array<SvfState, maxChannels> redState, listenState;
        double redG0 = 0.4;
        float envFull = 0.0f, envPow = 0.0f, grDb = 0.0f, levelDb = -100.0f, wideGain = 1.0f;
        float lastFreq = -1.0f, lastQ = -1.0f;
        LevelHistory history;
        std::atomic<float> currentGr { 0.0f };
    };
}
