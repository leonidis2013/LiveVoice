// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "LevelHistory.h"

// Transparent, smooth vocal compressor (zero latency, feed-forward).
//
// Design choices aimed at "you don't hear it working":
//  - RMS-style detector (~5 ms) rather than peak, so it reacts to loudness, not spikes
//  - sidechain high-pass so plosives and proximity boom don't pump the voice
//  - wide soft knee (default 10 dB) for a gradual onset of compression
//  - attack/release smoothing in the log (dB) domain, the approach that gives
//    the cleanest, artefact-free gain changes
//  - optional program-dependent ("auto") release, opto-style: quick recovery
//    after short peaks, slower and gentler after sustained compression
//  - mix control for parallel compression
//  - optional auto gain: makeup follows the average gain reduction (over ~2 s, only
//    while the voice is present) so the output stays as loud as the input;
//    the Makeup knob then trims on top of it
namespace dsp
{
    class VocalCompressor
    {
    public:
        struct Settings
        {
            bool on = false, autoRelease = true, autoGain = false;
            float thresholdDb = -20.0f, ratio = 3.0f, kneeDb = 10.0f, attackMs = 8.0f, releaseMs = 250.0f;
            float makeupDb = 0.0f, mix = 1.0f, scHpHz = 100.0f;
        };

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, 2, numChannels);
            lastHp = -1.0f;
            history.prepare (fs, 25);
            reset();
        }

        void reset()
        {
            sc.reset();
            envPow = 0.0f; grSmooth = 0.0f; grSlow = 0.0f;
            makeupSmoothed = 0.0f; avgLossDb = 0.0f;
            appliedMakeup.store (0.0f);
            currentGr.store (0.0f);
            inputLevel.store (-100.0f);
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        // Static curve: gain reduction (positive dB) for an input level, soft knee.
        static float computeGainReduction (float xDb, float thr, float ratio, float knee) noexcept
        {
            const float slope = 1.0f / std::max (1.0f, ratio) - 1.0f;   // <= 0
            const float d = xDb - thr;
            float yDb;
            if (2.0f * d < -knee)                   yDb = xDb;
            else if (knee > 0.0f && 2.0f * std::abs (d) <= knee)
                                                    yDb = xDb + slope * (d + knee * 0.5f) * (d + knee * 0.5f) / (2.0f * knee);
            else                                    yDb = thr + d / std::max (1.0f, ratio);
            return std::max (0.0f, xDb - yDb);
        }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            if (! juce::exactlyEqual (settings.scHpHz, lastHp)) { scCoeffs.highPass (settings.scHpHz, 0.707, fs); lastHp = settings.scHpHz; }

            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            const float rmsCoeff = timeCoeff (5.0f, fs);

            if (! settings.on)
            {
                // keep metering the input so the graphs help set the threshold before switching on
                float maxX = -100.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                    const float s = sc.process (mono, scCoeffs);
                    envPow = rmsCoeff * envPow + (1.0f - rmsCoeff) * s * s;
                    const float xDb = 10.0f * std::log10 (envPow + 1.0e-12f) + 3.0103f;
                    maxX = std::max (maxX, xDb);
                    history.add (xDb, 0.0f);
                }
                currentGr.store (0.0f, std::memory_order_relaxed);
                inputLevel.store (maxX, std::memory_order_relaxed);
                appliedMakeup.store (0.0f, std::memory_order_relaxed);
                grSmooth = grSlow = 0.0f;
                return;
            }

            const float aA = timeCoeff (settings.attackMs, fs);
            const float aRbase = timeCoeff (settings.releaseMs, fs);
            const float aRfast = timeCoeff (settings.releaseMs * 0.5f, fs);
            const float aRslow = timeCoeff (std::min (3000.0f, settings.releaseMs * 3.0f), fs);
            const float aSlowTrack = timeCoeff (800.0f, fs);
            const float aMakeup = timeCoeff (20.0f, fs);
            const float aAuto = timeCoeff (2000.0f, fs);
            const float mix = juce::jlimit (0.0f, 1.0f, settings.mix);
            float maxGr = 0.0f, maxX = -100.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                const float s = sc.process (mono, scCoeffs);
                envPow = rmsCoeff * envPow + (1.0f - rmsCoeff) * s * s;
                const float xDb = 10.0f * std::log10 (envPow + 1.0e-12f) + 3.0103f;   // sine-peak referenced

                const float target = computeGainReduction (xDb, settings.thresholdDb, settings.ratio, settings.kneeDb);

                float aR = aRbase;
                if (settings.autoRelease)
                {
                    grSlow = aSlowTrack * grSlow + (1.0f - aSlowTrack) * target;
                    const float sustained = juce::jlimit (0.0f, 1.0f, grSlow / std::max (0.5f, grSmooth));
                    aR = aRfast + (aRslow - aRfast) * sustained;
                }

                grSmooth = target > grSmooth ? aA * grSmooth + (1.0f - aA) * target
                                             : aR * grSmooth + (1.0f - aR) * target;
                maxGr = std::max (maxGr, grSmooth);
                maxX = std::max (maxX, xDb);
                history.add (xDb, grSmooth);

                if (settings.autoGain && xDb > settings.thresholdDb - 24.0f)
                {
                    // gain lost to compression right now (accounting for the parallel mix)
                    const float lossDb = -gainToDb (mix * dbToGain (-grSmooth) + (1.0f - mix));
                    avgLossDb = aAuto * avgLossDb + (1.0f - aAuto) * lossDb;
                }
                const float makeupTarget = settings.makeupDb + (settings.autoGain ? avgLossDb : 0.0f);
                makeupSmoothed = aMakeup * makeupSmoothed + (1.0f - aMakeup) * makeupTarget;
                const float g = dbToGain (makeupSmoothed - grSmooth);
                const float gl = mix * g + (1.0f - mix) * dbToGain (makeupSmoothed);   // parallel blend, makeup on both

                L[i] *= gl;
                if (R != nullptr) R[i] *= gl;
            }

            currentGr.store (maxGr, std::memory_order_relaxed);
            inputLevel.store (maxX, std::memory_order_relaxed);
            appliedMakeup.store (makeupSmoothed, std::memory_order_relaxed);
        }

        float getMakeupDb() const noexcept        { return appliedMakeup.load (std::memory_order_relaxed); }   // incl. auto gain
        float getGainReductionDb() const noexcept { return currentGr.load (std::memory_order_relaxed); }
        float getInputLevelDb() const noexcept    { return inputLevel.load (std::memory_order_relaxed); }
        void readHistory (LevelHistory::Point* dest, int count) const noexcept { history.read (dest, count); }

    private:
        Settings settings;
        double fs = 48000.0;
        int channels = 2;
        SvfCoeffs scCoeffs;
        SvfState sc;
        float lastHp = -1.0f;
        float envPow = 0.0f, grSmooth = 0.0f, grSlow = 0.0f, makeupSmoothed = 0.0f, avgLossDb = 0.0f;
        std::atomic<float> appliedMakeup { 0.0f };
        std::atomic<float> currentGr { 0.0f }, inputLevel { -100.0f };
        LevelHistory history;
    };
}
