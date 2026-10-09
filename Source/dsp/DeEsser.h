#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "LevelHistory.h"

// Zero-latency de-esser.
//
// Detection
//   The sibilance band (band-pass at Freq / Width, or high-pass above Freq in the
//   Shelf / Wideband modes) is measured with a fast envelope.
//   Absolute: that level in dBFS against Threshold.
//   Relative: that level compared with the voice's BODY (200 Hz - 3 kHz). An "s" is
//   loud in the sibilance band and weak in the body; a bright vowel is the opposite,
//   so this separates the two far better than comparing with the whole signal.
//
// Gain
//   Soft knee (6 dB), ~4:1 above the threshold, reaching Range with a short rounded corner.
//   Gain changes are smoothed in dB: 1 ms attack, Release as set.
//
// Reduction
//   Band: dynamic bell over the sibilance band. Shelf: dynamic high shelf.
//   Wideband: the whole voice ducks.
//
// Solo writes into a separate buffer (the main output is never touched):
//   the sibilance band as the detector hears it, or "removed" (what the de-esser takes out).
namespace dsp
{
    class DeEsser
    {
    public:
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 16;
        static constexpr int historyColumnMs = 25;
        static constexpr float kneeDb = 6.0f, slope = 0.75f;   // ~4:1
        static constexpr float gateDb = -65.0f;                  // no reduction on near-silence

        enum Mode { band = 0, shelf, wideband };
        enum Listen { listenBand = 0, listenRemoved };

        struct Settings
        {
            bool on = true, relative = true;
            int mode = band, listen = listenBand;
            float freq = 6500.0f, q = 1.0f, thresholdDb = -30.0f, rangeDb = 8.0f, releaseMs = 80.0f;
            float relThresholdDb = -6.0f;     // sibilance band minus voice body, in dB
        };

        using HistoryPoint = LevelHistory::Point;

        static float gainReductionFor (float levelDb, float threshold, float range) noexcept
        {
            const float d = levelDb - threshold;
            float amount;
            if (2.0f * d <= -kneeDb)    amount = 0.0f;
            else if (2.0f * d < kneeDb) amount = slope * (d + kneeDb * 0.5f) * (d + kneeDb * 0.5f) / (2.0f * kneeDb);
            else                        amount = slope * d;
            // reach Range with a short 2 dB rounded corner (not a long tanh crawl, not a hard edge)
            constexpr float corner = 2.0f;
            return std::max (0.0f, 0.5f * (amount + range - std::sqrt ((amount - range) * (amount - range) + corner * corner)));
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            bodyHp.highPass (200.0, 0.707, fs);
            bodyLp.lowPass (3000.0, 0.707, fs);
            history.prepare (fs, historyColumnMs);
            reset();
        }

        void reset()
        {
            for (auto& s : redState) s.reset();
            for (auto& s : listenState) s.reset();
            det.reset(); bodyA.reset(); bodyB.reset();
            envSib = envBody = 0.0f; grDb = 0.0f; counter = 0;
            lastFreq = lastQ = -1.0f; lastMode = -1;
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        // Response (dB) of the reduction filter for a given gain reduction, for drawing.
        static float reductionResponseDb (const Settings& s, float gr, double freq, double sampleRate)
        {
            if (gr <= 0.0f) return 0.0f;
            SvfCoeffs c;
            if (s.mode == band)        c.bell (s.freq, s.q, -gr, sampleRate);
            else if (s.mode == shelf)  c.highShelf (s.freq, 0.707, -gr, sampleRate);
            else                       return -gr;
            return (float) c.magnitudeDb (freq, sampleRate);
        }

        // listenOut: optional buffer (same channel count) that receives the solo signal.
        void process (juce::AudioBuffer<float>& buffer, int numSamples, juce::AudioBuffer<float>* listenOut = nullptr) noexcept
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

            const float envAtt = timeCoeff (0.5f, fs), envRel = timeCoeff (20.0f, fs);
            const float ctlFs = (float) (fs / controlInterval);
            const float grAtt = std::exp (-1.0f / (0.001f * ctlFs));
            const float grRel = std::exp (-1.0f / (std::max (5.0f, settings.releaseMs) * 0.001f * ctlFs));
            float* lisL = listenOut != nullptr ? listenOut->getWritePointer (0) : nullptr;
            float* lisR = (listenOut != nullptr && R != nullptr && listenOut->getNumChannels() > 1) ? listenOut->getWritePointer (1) : nullptr;
            const bool listenRemovedMode = settings.listen == listenRemoved;

            for (int i = 0; i < numSamples; ++i)
            {
                const float inL = L[i];
                const float inR = R != nullptr ? R[i] : 0.0f;
                float l = inL, r = inR;

                const float mono = R != nullptr ? 0.5f * (l + r) : l;
                const float d = det.process (mono, detCoeffs);
                const float ps = d * d;
                const float cs = ps > envSib ? envAtt : envRel;
                envSib = cs * envSib + (1.0f - cs) * ps;
                if (settings.relative)
                {
                    const float b = bodyB.process (bodyA.process (mono, bodyHp), bodyLp);
                    const float pb = b * b;
                    const float cb = pb > envBody ? envAtt : envRel;
                    envBody = cb * envBody + (1.0f - cb) * pb;
                }

                if (counter == 0)
                {
                    const float sibDb = 10.0f * std::log10 (std::max (envSib, 1.0e-12f)) + 3.0103f;
                    float threshold = settings.thresholdDb;
                    if (settings.relative)
                    {
                        const float bodyDb = 10.0f * std::log10 (std::max (envBody, 1.0e-12f)) + 3.0103f;
                        levelDb = sibDb > gateDb ? sibDb - bodyDb : -100.0f;
                        threshold = settings.relThresholdDb;
                    }
                    else
                        levelDb = sibDb;

                    const float target = settings.on ? gainReductionFor (levelDb, threshold, settings.rangeDb) : 0.0f;
                    const float c = target > grDb ? grAtt : grRel;
                    grDb = c * grDb + (1.0f - c) * target;

                    if (settings.mode == band)       redCoeffs.bellFromG (redG0, settings.q, -grDb);
                    else if (settings.mode == shelf) redCoeffs.highShelfFromG (redG0, 0.707, -grDb);
                    wideGain = settings.mode == wideband ? dbToGain (-grDb) : 1.0f;
                }
                counter = (counter + 1) % controlInterval;

                if (settings.on)
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

                if (lisL != nullptr)
                {
                    if (listenRemovedMode)
                    {
                        lisL[i] = inL - l;
                        if (lisR != nullptr) lisR[i] = inR - r;
                    }
                    else
                    {
                        lisL[i] = listenState[0].process (inL, detCoeffs);
                        if (lisR != nullptr) lisR[i] = listenState[1].process (inR, detCoeffs);
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
        SvfCoeffs detCoeffs, redCoeffs, bodyHp, bodyLp;
        SvfState det, bodyA, bodyB;
        std::array<SvfState, maxChannels> redState, listenState;
        double redG0 = 0.4;
        float envSib = 0.0f, envBody = 0.0f, grDb = 0.0f, levelDb = -100.0f, wideGain = 1.0f;
        float lastFreq = -1.0f, lastQ = -1.0f;
        LevelHistory history;
        std::atomic<float> currentGr { 0.0f };
    };
}
