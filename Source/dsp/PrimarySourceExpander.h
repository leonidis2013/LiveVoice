// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"

// Primary Source Expander (PSE): a smooth downward expander that pulls the
// mic down by up to `range` dB whenever the talker/singer pauses. Less
// stage bleed and room pickup between phrases also means less chance of
// feedback building while the mic is idle.
//
// Zero latency (no look-ahead). Opening uses a fast attack so first
// syllables come through; hysteresis + hold stop it chattering on word
// endings; closing is a gentle 1:3 expansion, not a hard gate.
namespace dsp
{
    class PrimarySourceExpander
    {
    public:
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 16;
        static constexpr float hysteresisDb = 4.0f;
        static constexpr float expansionSlope = 2.0f;   // 1:3 below the close threshold

        struct Settings
        {
            bool on = false;
            float thresholdDb = -45.0f, rangeDb = 12.0f, attackMs = 1.0f, holdMs = 80.0f, releaseMs = 300.0f;
            float scHpHz = 150.0f, scLpHz = 8000.0f;
        };

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            lastHp = lastLp = -1.0f;
            reset();
        }

        void reset()
        {
            hp.reset(); lp.reset();
            env = 0.0f; gainDb = 0.0f; gain = 1.0f; gainStep = 0.0f; open = false; holdCounter = 0; counter = 0;
            keyDb.store (-100.0f); grDb.store (0.0f); isOpen.store (false);
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            if (! settings.on)
            {
                gainDb = 0.0f; gain = 1.0f; gainStep = 0.0f; open = true;
                grDb.store (0.0f, std::memory_order_relaxed);
                isOpen.store (true, std::memory_order_relaxed);
                // keep the key meter alive so the threshold can be set before switching on
                measureOnly (buffer, numSamples);
                return;
            }

            updateFilters();
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            const float envAtt = timeCoeff (0.3f, fs), envRel = timeCoeff (25.0f, fs);
            const float ctlFs = (float) (fs / controlInterval);
            const float openCoeff  = std::exp (-1.0f / (std::max (0.05f, settings.attackMs) * 0.001f * ctlFs));
            const float closeCoeff = std::exp (-1.0f / (std::max (5.0f, settings.releaseMs) * 0.001f * ctlFs));
            const int holdSamples = (int) (settings.holdMs * 0.001f * fs);
            const float closeThr = settings.thresholdDb - hysteresisDb;

            float minGainDb = 0.0f, maxKey = -100.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                const float k = lp.process (hp.process (mono, hpCoeffs), lpCoeffs);
                const float p = k * k;
                env = (p > env ? envAtt : envRel) * env + (1.0f - (p > env ? envAtt : envRel)) * p;

                if (counter == 0)
                {
                    const float key = 10.0f * std::log10 (std::max (env, 1.0e-12f)) + 3.0103f;
                    maxKey = std::max (maxKey, key);

                    if (key >= settings.thresholdDb)        { open = true; holdCounter = holdSamples; }
                    else if (key < closeThr)
                    {
                        if (holdCounter > 0) holdCounter -= controlInterval;
                        else open = false;
                    }

                    float target = 0.0f;
                    if (! open)
                        target = std::max (-settings.rangeDb, std::min (0.0f, (key - closeThr) * expansionSlope));

                    const float c = target > gainDb ? openCoeff : closeCoeff;
                    gainDb = c * gainDb + (1.0f - c) * target;
                    minGainDb = std::min (minGainDb, gainDb);

                    // ramp linearly to the new gain across the control interval (click-free)
                    const float next = dbToGain (gainDb);
                    gainStep = (next - gain) / (float) controlInterval;
                }
                counter = (counter + 1) % controlInterval;

                gain += gainStep;
                L[i] *= gain;
                if (R != nullptr) R[i] *= gain;
            }

            grDb.store (-minGainDb, std::memory_order_relaxed);
            keyDb.store (maxKey, std::memory_order_relaxed);
            isOpen.store (open, std::memory_order_relaxed);
        }

        float getKeyLevelDb() const noexcept       { return keyDb.load (std::memory_order_relaxed); }
        float getGainReductionDb() const noexcept  { return grDb.load (std::memory_order_relaxed); }
        bool  getIsOpen() const noexcept           { return isOpen.load (std::memory_order_relaxed); }

    private:
        void updateFilters() noexcept
        {
            if (! juce::exactlyEqual (settings.scHpHz, lastHp)) { hpCoeffs.highPass (settings.scHpHz, 0.707, fs); lastHp = settings.scHpHz; }
            if (! juce::exactlyEqual (settings.scLpHz, lastLp)) { lpCoeffs.lowPass (settings.scLpHz, 0.707, fs); lastLp = settings.scLpHz; }
        }

        void measureOnly (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            updateFilters();
            const int nCh = std::min (channels, buffer.getNumChannels());
            const float* L = buffer.getReadPointer (0);
            const float* R = nCh > 1 ? buffer.getReadPointer (1) : nullptr;
            const float envAtt = timeCoeff (0.3f, fs), envRel = timeCoeff (25.0f, fs);
            float peakEnv = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
                const float k = lp.process (hp.process (mono, hpCoeffs), lpCoeffs);
                const float p = k * k;
                env = (p > env ? envAtt : envRel) * env + (1.0f - (p > env ? envAtt : envRel)) * p;
                peakEnv = std::max (peakEnv, env);
            }
            keyDb.store (10.0f * std::log10 (std::max (peakEnv, 1.0e-12f)) + 3.0103f, std::memory_order_relaxed);
        }

        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0, holdCounter = 0;
        SvfCoeffs hpCoeffs, lpCoeffs;
        SvfState hp, lp;
        float lastHp = -1.0f, lastLp = -1.0f;
        float env = 0.0f, gainDb = 0.0f, gain = 1.0f, gainStep = 0.0f;
        bool open = false;
        std::atomic<float> keyDb { -100.0f }, grDb { 0.0f };
        std::atomic<bool> isOpen { false };
    };
}
