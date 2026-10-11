// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"

// Three small zero-latency tools between the de-esser and the compressor:
//
//   ProximityComp  - turns the low end down when someone gets close to the mic
//   AutoLeveler    - slowly rides the voice toward a target level (before the compressor)
//   Warmth         - adds a little 2nd/3rd harmonic to the low-mids only
namespace dsp
{
    // ------------------------------------------------------------------ proximity
    // Watches the low end (around 150 Hz) against the voice's mids (around 1.5 kHz).
    // The voice's normal balance is learned slowly while they talk; when the low end
    // rises above it (proximity effect) a 250 Hz low shelf takes the excess back out,
    // up to Amount. Holds still between phrases.
    class ProximityComp
    {
    public:
        static constexpr int maxChannels = 2, controlInterval = 16;
        static constexpr float marginDb = 3.0f;      // ignore normal vowel-to-vowel variation
        static constexpr float gateDb = -55.0f;      // mids must be above this to count as talking

        struct Settings { bool on = false; float amountDb = 6.0f; };

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            lfC.bandPass (150.0, 0.8, fs);
            midC.bandPass (1500.0, 0.7, fs);
            shelfG0 = SvfCoeffs::prewarp (250.0, fs);
            shelf.identity();
            reset();
        }

        void reset()
        {
            lfS.reset(); midS.reset();
            for (auto& s : shelfS) s.reset();
            envLf = envMid = 0.0f; ratioDb = 0.0f; baselineDb = 0.0f; cutDb = 0.0f;
            talkedSamples = 0; counter = 0; haveRatio = haveBaseline = false;
            shelf.identity();
        }

        void setSettings (const Settings& s) noexcept { settings = s; }
        float getCutDb() const noexcept { return currentCut.load (std::memory_order_relaxed); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            if (! settings.on)
            {
                if (cutDb != 0.0f || haveBaseline) reset();
                currentCut.store (0.0f, std::memory_order_relaxed);
                return;
            }
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
            const float env = timeCoeff (150.0f, fs);
            const float ctlFs = (float) (fs / controlInterval);
            auto ctl = [ctlFs] (float sec) { return std::exp (-1.0f / (sec * ctlFs)); };
            const float ratioC = ctl (0.5f), baseFast = ctl (20.0f), baseSlow = ctl (120.0f);
            const float cutAtt = ctl (0.2f), cutRel = ctl (0.6f);

            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                const float a = lfS.process (mono, lfC), b = midS.process (mono, midC);
                envLf = env * envLf + (1.0f - env) * a * a;
                envMid = env * envMid + (1.0f - env) * b * b;

                if (counter == 0)
                {
                    const float midDb = 10.0f * std::log10 (envMid + 1.0e-12f);
                    if (midDb > gateDb)   // someone is talking: measure, learn, react
                    {
                        const float r = 10.0f * std::log10 (envLf + 1.0e-12f) - midDb;
                        ratioDb = haveRatio ? ratioC * ratioDb + (1.0f - ratioC) * r : r;
                        haveRatio = true;
                        talkedSamples += controlInterval;
                        if (! haveBaseline && talkedSamples > (int) fs)   // ~1 s of voice before trusting it
                        {
                            baselineDb = ratioDb;
                            haveBaseline = true;
                        }
                        else if (haveBaseline)
                        {
                            const float c = ratioDb < baselineDb + marginDb ? baseFast : baseSlow;
                            baselineDb = c * baselineDb + (1.0f - c) * ratioDb;
                        }
                        const float target = haveBaseline ? juce::jlimit (0.0f, settings.amountDb, ratioDb - baselineDb - marginDb) : 0.0f;
                        const float c = target > cutDb ? cutAtt : cutRel;
                        cutDb = c * cutDb + (1.0f - c) * target;
                        shelf.lowShelfFromG (shelfG0, 0.707, -cutDb);
                    }
                }
                counter = (counter + 1) % controlInterval;

                L[i] = shelfS[0].process (L[i], shelf);
                if (R != nullptr) R[i] = shelfS[1].process (R[i], shelf);
            }
            currentCut.store (cutDb, std::memory_order_relaxed);
        }

    private:
        Settings settings;
        double fs = 48000.0, shelfG0 = 0.1;
        int channels = 2, counter = 0, talkedSamples = 0;
        SvfCoeffs lfC, midC, shelf;
        SvfState lfS, midS;
        std::array<SvfState, maxChannels> shelfS;
        float envLf = 0.0f, envMid = 0.0f, ratioDb = 0.0f, baselineDb = 0.0f, cutDb = 0.0f;
        bool haveRatio = false, haveBaseline = false;
        std::atomic<float> currentCut { 0.0f };
    };

    // ------------------------------------------------------------------ leveler
    // Slow gain rider: measures the voice's loudness (RMS, 300 ms, above 100 Hz) and
    // moves the gain toward Target, never more than Range up or down. It only moves
    // while someone is talking (within 20 dB of the target); between phrases it holds,
    // so it never raises the room or the stage between words. Turning down is twice as
    // quick as turning up. When feedback is detected, holdBoost() stops it turning UP for a few
    // seconds (it may still turn down), so it can never feed a ring.
    class AutoLeveler
    {
    public:
        static constexpr int maxChannels = 2, controlInterval = 16;
        static constexpr float listenWindowDb = 20.0f;
        enum Speed { slow = 0, medium, fast };

        struct Settings { bool on = false; float targetDb = -28.0f, rangeDb = 6.0f; int speed = medium; };

        static float speedSeconds (int s) noexcept { return s == slow ? 4.0f : (s == fast ? 1.0f : 2.0f); }

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            hp.highPass (100.0, 0.707, fs);
            reset();
        }

        void reset()
        {
            hpS.reset();
            power = 0.0f; gainDb = 0.0f; gainLin = 1.0f; counter = 0; holdSamples = 0;
        }

        // Called from the audio thread when the feedback detector reports a ring.
        void holdBoost (double seconds) noexcept { holdSamples = std::max (holdSamples, (int) (seconds * fs)); }
        bool isHoldingBoost() const noexcept { return holding.load (std::memory_order_relaxed); }

        void setSettings (const Settings& s) noexcept { settings = s; }
        float getGainDb() const noexcept { return currentGain.load (std::memory_order_relaxed); }
        float getLevelDb() const noexcept { return currentLevel.load (std::memory_order_relaxed); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
            const float avg = timeCoeff (300.0f, fs);
            const float ctlFs = (float) (fs / controlInterval);
            const float up = std::exp (-1.0f / (speedSeconds (settings.speed) * ctlFs));
            const float down = std::exp (-1.0f / (0.5f * speedSeconds (settings.speed) * ctlFs));
            const float offC = std::exp (-1.0f / (0.05f * ctlFs));

            float g = gainLin, step = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                const float h = hpS.process (mono, hp);
                power = avg * power + (1.0f - avg) * h * h;

                if (counter == 0)
                {
                    const float levelDb = 10.0f * std::log10 (power + 1.0e-12f);
                    lastLevel = levelDb;
                    if (! settings.on)
                        gainDb = offC * gainDb;   // glide back to unity when switched off
                    else if (levelDb > settings.targetDb - listenWindowDb && levelDb > -70.0f)
                    {
                        float desired = juce::jlimit (-settings.rangeDb, settings.rangeDb, settings.targetDb - levelDb);
                        if (holdSamples > 0) desired = std::min (desired, gainDb);   // feedback: no turning up
                        const float c = desired < gainDb ? down : up;
                        gainDb = c * gainDb + (1.0f - c) * desired;
                    }
                    else
                        gainDb = juce::jlimit (-settings.rangeDb, settings.rangeDb, gainDb);   // hold (Range may have changed)
                    step = (dbToGain (gainDb) - g) / (float) controlInterval;
                    holdSamples = std::max (0, holdSamples - controlInterval);
                }
                counter = (counter + 1) % controlInterval;
                g += step;
                L[i] *= g;
                if (R != nullptr) R[i] *= g;
            }
            gainLin = g;
            currentGain.store (gainDb, std::memory_order_relaxed);
            currentLevel.store (lastLevel, std::memory_order_relaxed);
            holding.store (settings.on && holdSamples > 0, std::memory_order_relaxed);
        }

    private:
        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0;
        SvfCoeffs hp;
        SvfState hpS;
        float power = 0.0f, gainDb = 0.0f, gainLin = 1.0f, lastLevel = -100.0f;
        int holdSamples = 0;
        std::atomic<float> currentGain { 0.0f }, currentLevel { -100.0f };
        std::atomic<bool> holding { false };
    };

    // ------------------------------------------------------------------ warmth
    // Subtle harmonic enhancement, low-mids only. The 80 Hz - 1.5 kHz part of the voice
    // is normalised by its own envelope and passed through Chebyshev polynomials, which
    // turn a sine into an exact 2nd (and a little 3rd) harmonic. The result is scaled
    // back and added in parallel, so it sounds the same at any level, the highest new
    // harmonic stays below ~4.5 kHz (no aliasing without oversampling), and 0 % is an
    // exact bypass. At 100 % the 2nd harmonic sits about 20 dB under the low-mids.
    class Warmth
    {
    public:
        static constexpr int maxChannels = 2;
        static constexpr float h2 = 0.6f, h3 = 0.25f, scale = 0.167f;

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            lp.lowPass (1500.0, 0.707, fs);
            hpIn.highPass (80.0, 0.707, fs);
            dc.highPass (40.0, 0.707, fs);
            reset();
        }

        void reset()
        {
            for (auto& c : ch) c = {};
            amountNow = 0.0f;
        }

        void setAmount (float percent) noexcept { amountTarget = juce::jlimit (0.0f, 1.0f, percent * 0.01f); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            if (amountTarget <= 0.0f && amountNow <= 0.0f) return;   // exact bypass
            const int nCh = std::min (channels, buffer.getNumChannels());
            const float att = timeCoeff (2.0f, fs), rel = timeCoeff (150.0f, fs);
            const float aStep = (amountTarget - amountNow) / (float) std::max (1, numSamples);
            for (int c = 0; c < nCh; ++c)
            {
                auto* x = buffer.getWritePointer (c);
                auto& s = ch[(size_t) c];
                float a = amountNow;
                for (int i = 0; i < numSamples; ++i)
                {
                    a += aStep;
                    const float band = s.lpS.process (s.hpS.process (x[i], hpIn), lp);
                    const float mag = std::abs (band);
                    const float k = mag > s.env ? att : rel;
                    s.env = k * s.env + (1.0f - k) * mag;
                    const float e = std::max (s.env, 1.0e-6f);
                    const float u = juce::jlimit (-1.0f, 1.0f, band / e);
                    const float harm = h2 * (2.0f * u * u - 1.0f) + h3 * (4.0f * u * u * u - 3.0f * u);
                    x[i] += a * scale * s.dcS.process (harm * e, dc);
                }
            }
            amountNow = amountTarget;
        }

    private:
        struct Channel { SvfState hpS, lpS, dcS; float env = 0.0f; };
        double fs = 48000.0;
        int channels = 2;
        SvfCoeffs lp, hpIn, dc;
        std::array<Channel, maxChannels> ch {};
        float amountTarget = 0.0f, amountNow = 0.0f;
    };
}
