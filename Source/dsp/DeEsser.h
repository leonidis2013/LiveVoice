// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "LevelHistory.h"

// Zero-latency two-band de-esser: "S" (around 6 kHz) and "SH" (around 3.8 kHz).
//
// Detection, per band
//   A fairly wide band-pass at the band's Freq measures the sibilance with a fast envelope.
//   Absolute: that level in dBFS against Threshold.
//   Relative: that level compared with the voice's body (S: 200 Hz - 3 kHz, SH: 200 Hz - 1.8 kHz).
//   An "s" is loud in its band and weak in the body; a vowel is the opposite.
//   Bleed gate: the band must be within 18 dB of the voice's recent level, so quiet hiss-like
//   sounds from the room or stage (cymbals into a face mic) never trigger it.
//
// Follow
//   While an "s" is being reduced, a bank of five quarter-octave detectors finds where its energy
//   actually peaks (within +-0.55 octave of Freq) and the cut moves there. Between "s" sounds the
//   cut glides back to Freq. This lets the cut be narrow (default Q 2.5) and still land on the "s".
//
// Gain
//   Soft knee (6 dB), ~4:1, reaching Range with a short rounded corner; 1 ms attack, Release as set.
//
// Reduction modes
//   Band:     a dynamic bell at the (followed) peak, Width = Q. Only the "s" itself is turned down.
//   Split:    the signal above Freq is split off (perfect-reconstruction split) and only that part
//             is turned down; everything below Freq is untouched, even during an "s".
//   Wideband: the whole voice ducks (ramped, no zipper).
//
// Solo writes into a separate buffer (the main output is never touched): the selected band's cut
// region as it is right now, or "removed" (what the de-esser takes out).
namespace dsp
{
    class DeEsser
    {
    public:
        static constexpr int maxChannels = 2, controlInterval = 16, historyColumnMs = 25;
        static constexpr int numBands = 2, bankSize = 5;
        static constexpr float kneeDb = 6.0f, slope = 0.75f;    // ~4:1
        static constexpr float gateDb = -65.0f;                  // nothing below this is sibilance
        static constexpr float bleedWindowDb = 18.0f;            // band must be within this of the voice level
        static constexpr float bodyFloorDb = -70.0f;
        static constexpr float followRangeOct = 0.55f;

        enum Mode { band = 0, split, wideband };
        enum Listen { listenBand = 0, listenRemoved };
        enum BandId { bandS = 0, bandSH = 1 };

        struct BandSettings
        {
            bool on = true;
            float freq = 6000.0f, q = 2.5f, thresholdDb = -30.0f, relThresholdDb = -3.0f, rangeDb = 10.0f;
        };

        struct Settings
        {
            bool on = true, relative = true, follow = true;
            int mode = band, listen = listenBand, listenBand = bandS;
            float releaseMs = 80.0f;
            std::array<BandSettings, numBands> bands { BandSettings {}, BandSettings { false, 3800.0f, 2.0f, -30.0f, 0.0f, 6.0f } };
        };

        using HistoryPoint = LevelHistory::Point;

        static float gainReductionFor (float levelDb, float threshold, float range) noexcept
        {
            const float d = levelDb - threshold;
            float amount;
            if (2.0f * d <= -kneeDb)    amount = 0.0f;
            else if (2.0f * d < kneeDb) amount = slope * (d + kneeDb * 0.5f) * (d + kneeDb * 0.5f) / (2.0f * kneeDb);
            else                        amount = slope * d;
            constexpr float corner = 2.0f;
            return std::max (0.0f, 0.5f * (amount + range - std::sqrt ((amount - range) * (amount - range) + corner * corner)));
        }

        // Response (dB) of the current reduction for drawing: gr / freq per band (freq = followed peak).
        static float reductionResponseDb (const Settings& s, const float* grDb, const float* freqs, double f, double sampleRate)
        {
            if (! s.on) return 0.0f;
            if (s.mode == wideband) return -std::max (grDb[0], grDb[1]);
            double db = 0.0;
            for (int b = 0; b < numBands; ++b)
            {
                if (grDb[b] <= 0.01f) continue;
                SvfCoeffs c;
                if (s.mode == band) c.bell (freqs[b], s.bands[(size_t) b].q, -grDb[b], sampleRate);
                else                c.highShelf (s.bands[(size_t) b].freq, 0.707, -grDb[b], sampleRate);
                db += c.magnitudeDb (f, sampleRate);
            }
            return (float) db;
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            bodyHp.highPass (200.0, 0.707, fs);
            bodyLpFull.lowPass (3000.0, 0.707, fs);
            bodyLpLow.lowPass (1800.0, 0.707, fs);
            history.prepare (fs, historyColumnMs);
            for (auto& b : bandState) b.lastFreq = -1.0f;
            reset();
        }

        void reset()
        {
            for (auto& b : bandState)
            {
                b.det.reset();
                for (auto& s : b.bank) s.reset();
                for (auto& s : b.cut) s.reset();
                for (auto& s : b.splitLp) s.reset();
                for (auto& s : b.listen) s.reset();
                b.envDet = 0.0f; b.bankEnv.fill (0.0f);
                b.grDb = 0.0f; b.levelDb = -100.0f; b.gain = 1.0f; b.gainStep = 0.0f;
                b.trackedFreq = b.lastFreq > 0.0f ? b.lastFreq : 6000.0f;
                b.cutCoeffs.identity();
            }
            hpS.reset(); lpFullS.reset(); lpLowS.reset();
            envFull = envLow = 0.0f; voiceRefDb = -100.0f;
            wideGain = 1.0f; wideStep = 0.0f; counter = 0;
        }

        void setSettings (const Settings& s) noexcept { settings = s; }

        // listenOut: optional buffer (same channel count) that receives the solo signal.
        void process (juce::AudioBuffer<float>& buffer, int numSamples, juce::AudioBuffer<float>* listenOut = nullptr) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            for (int b = 0; b < numBands; ++b) updateBandFilters (b);

            const float envAtt = timeCoeff (0.5f, fs), envRel = timeCoeff (20.0f, fs);
            const float bankAtt = timeCoeff (1.0f, fs), bankRel = timeCoeff (15.0f, fs);
            const float ctlFs = (float) (fs / controlInterval);
            const float grAtt = std::exp (-1.0f / (0.001f * ctlFs));
            const float grRel = std::exp (-1.0f / (std::max (5.0f, settings.releaseMs) * 0.001f * ctlFs));
            const float followIn = std::exp (-1.0f / (0.003f * ctlFs)), followOut = std::exp (-1.0f / (0.060f * ctlFs));
            const float voiceDecayDb = 3.0f / ctlFs;   // the voice reference falls 3 dB per second, so it outlasts a pause between phrases
            const bool doFollow = settings.follow && settings.mode == band;

            float* lisL = listenOut != nullptr ? listenOut->getWritePointer (0) : nullptr;
            float* lisR = (listenOut != nullptr && R != nullptr && listenOut->getNumChannels() > 1) ? listenOut->getWritePointer (1) : nullptr;
            const bool listenRemovedMode = settings.listen == listenRemoved;
            auto& lb = bandState[(size_t) juce::jlimit (0, numBands - 1, settings.listenBand)];

            for (int i = 0; i < numSamples; ++i)
            {
                const float inL = L[i];
                const float inR = R != nullptr ? R[i] : 0.0f;
                float l = inL, r = inR;
                const float mono = R != nullptr ? 0.5f * (l + r) : l;

                // ---- detection ----
                const float hb = hpS.process (mono, bodyHp);
                envelope (envFull, lpFullS.process (hb, bodyLpFull), envAtt, envRel);
                envelope (envLow, lpLowS.process (hb, bodyLpLow), envAtt, envRel);
                for (int b = 0; b < numBands; ++b)
                {
                    auto& st = bandState[(size_t) b];
                    if (! settings.bands[(size_t) b].on) continue;
                    envelope (st.envDet, st.det.process (mono, st.detCoeffs), envAtt, envRel);
                    if (doFollow)
                        for (int k = 0; k < bankSize; ++k)
                            envelope (st.bankEnv[(size_t) k], st.bank[(size_t) k].process (mono, st.bankCoeffs[(size_t) k]), bankAtt, bankRel);
                }

                // ---- control rate: gain reduction, follow, coefficients ----
                if (counter == 0)
                {
                    const float fullDb = toDb (envFull), lowDb = toDb (envLow);
                    voiceRefDb = std::max (fullDb, voiceRefDb - voiceDecayDb);
                    float maxGr = 0.0f;
                    for (int b = 0; b < numBands; ++b)
                    {
                        auto& st = bandState[(size_t) b];
                        const auto& bs = settings.bands[(size_t) b];
                        float target = 0.0f;
                        if (bs.on)
                        {
                            const float sibDb = toDb (st.envDet);
                            const bool gated = sibDb > gateDb && (voiceRefDb < -60.0f || sibDb > voiceRefDb - bleedWindowDb);
                            if (settings.relative)
                            {
                                const float bodyDb = std::max (b == bandS ? fullDb : lowDb, bodyFloorDb);
                                st.levelDb = gated ? sibDb - bodyDb : -100.0f;
                                target = gainReductionFor (st.levelDb, bs.relThresholdDb, bs.rangeDb);
                            }
                            else
                            {
                                st.levelDb = gated ? sibDb : -100.0f;
                                target = gainReductionFor (st.levelDb, bs.thresholdDb, bs.rangeDb);
                            }
                            if (! settings.on) target = 0.0f;
                        }
                        const float c = target > st.grDb ? grAtt : grRel;
                        st.grDb = c * st.grDb + (1.0f - c) * target;
                        maxGr = std::max (maxGr, st.grDb);

                        // follow the peak while an "s" is being reduced, glide home otherwise
                        float goal = bs.freq;
                        if (doFollow && bs.on && target > 0.3f)
                        {
                            int k = 0;
                            for (int j = 1; j < bankSize; ++j) if (st.bankEnv[(size_t) j] > st.bankEnv[(size_t) k]) k = j;
                            float off = 0.0f;
                            if (k > 0 && k < bankSize - 1)
                            {
                                const float a = toDb (st.bankEnv[(size_t) k - 1]), m = toDb (st.bankEnv[(size_t) k]), p = toDb (st.bankEnv[(size_t) k + 1]);
                                const float den = a - 2.0f * m + p;
                                if (den < -1.0e-3f) off = juce::jlimit (-0.5f, 0.5f, 0.5f * (a - p) / den);
                            }
                            const float oct = juce::jlimit (-followRangeOct, followRangeOct, ((float) (k - bankSize / 2) + off) * 0.25f);
                            goal = bs.freq * std::exp2 (oct);
                        }
                        const float fc = (doFollow && target > 0.3f) ? followIn : followOut;
                        st.trackedFreq = std::exp2 (fc * std::log2 (st.trackedFreq) + (1.0f - fc) * std::log2 (goal));

                        if (settings.mode == band)
                            st.cutCoeffs.bell (st.trackedFreq, bs.q, -st.grDb, fs);
                        const float g = dbToGain (-st.grDb);
                        st.gainStep = (g - st.gain) / (float) controlInterval;
                        grBand[(size_t) b].store (settings.on && bs.on ? st.grDb : 0.0f, std::memory_order_relaxed);
                        freqBand[(size_t) b].store (st.trackedFreq, std::memory_order_relaxed);
                    }
                    wideStep = ((settings.mode == wideband ? dbToGain (-maxGr) : 1.0f) - wideGain) / (float) controlInterval;
                    currentGr.store (settings.on ? maxGr : 0.0f, std::memory_order_relaxed);
                    displayLevel = bandState[0].levelDb;
                    displayGr = settings.on ? maxGr : 0.0f;
                }
                counter = (counter + 1) % controlInterval;

                // ---- reduction ----
                if (settings.on)
                {
                    if (settings.mode == wideband)
                    {
                        wideGain += wideStep;
                        l *= wideGain;
                        r *= wideGain;
                    }
                    else
                        for (int b = 0; b < numBands; ++b)
                        {
                            auto& st = bandState[(size_t) b];
                            if (! settings.bands[(size_t) b].on) continue;
                            if (settings.mode == band)
                            {
                                l = st.cut[0].process (l, st.cutCoeffs);
                                if (R != nullptr) r = st.cut[1].process (r, st.cutCoeffs);
                            }
                            else   // split: y = low + g * (x - low), exact reconstruction when g = 1
                            {
                                st.gain += st.gainStep;
                                const float lowL = st.splitLp[0].process (l, st.splitCoeffs);
                                l = lowL + st.gain * (l - lowL);
                                if (R != nullptr)
                                {
                                    const float lowR = st.splitLp[1].process (r, st.splitCoeffs);
                                    r = lowR + st.gain * (r - lowR);
                                }
                            }
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
                        lisL[i] = lb.listen[0].process (inL, lb.listenCoeffs);
                        if (lisR != nullptr) lisR[i] = lb.listen[1].process (inR, lb.listenCoeffs);
                    }
                }

                L[i] = l;
                if (R != nullptr) R[i] = r;
                history.add (displayLevel, displayGr);
            }
        }

        void readHistory (HistoryPoint* dest, int count) const noexcept { history.read (dest, count); }
        float getCurrentGainReductionDb() const noexcept { return currentGr.load (std::memory_order_relaxed); }
        float getBandGainReductionDb (int b) const noexcept { return grBand[(size_t) juce::jlimit (0, numBands - 1, b)].load (std::memory_order_relaxed); }
        float getBandFrequency (int b) const noexcept { return freqBand[(size_t) juce::jlimit (0, numBands - 1, b)].load (std::memory_order_relaxed); }

    private:
        struct BandState
        {
            SvfCoeffs detCoeffs, cutCoeffs, splitCoeffs, listenCoeffs;
            std::array<SvfCoeffs, bankSize> bankCoeffs;
            SvfState det;
            std::array<SvfState, bankSize> bank;
            std::array<SvfState, maxChannels> cut, splitLp, listen;
            std::array<float, bankSize> bankEnv {};
            float envDet = 0.0f, grDb = 0.0f, levelDb = -100.0f, gain = 1.0f, gainStep = 0.0f;
            float trackedFreq = 6000.0f, lastFreq = -1.0f, lastQ = -1.0f, lastListenFreq = -1.0f;
        };

        static float toDb (float pow) noexcept { return 10.0f * std::log10 (std::max (pow, 1.0e-12f)) + 3.0103f; }
        static void envelope (float& env, float x, float att, float rel) noexcept
        {
            const float p = x * x;
            const float c = p > env ? att : rel;
            env = c * env + (1.0f - c) * p;
        }

        void updateBandFilters (int b) noexcept
        {
            auto& st = bandState[(size_t) b];
            const auto& bs = settings.bands[(size_t) b];
            if (! juce::exactlyEqual (bs.freq, st.lastFreq))
            {
                st.detCoeffs.bandPass (bs.freq, b == bandS ? 1.0 : 1.2, fs);    // wide: catches the whole "s"
                for (int k = 0; k < bankSize; ++k)
                    st.bankCoeffs[(size_t) k].bandPass (bs.freq * std::exp2 ((k - bankSize / 2) * 0.25), 4.0, fs);
                st.splitCoeffs.lowPass (bs.freq, 0.707, fs);
                if (st.lastFreq < 0.0f) st.trackedFreq = bs.freq;
                st.lastFreq = bs.freq;
            }
            // the solo plays the region being cut right now (followed peak, cut width)
            const float lf = settings.mode == band ? st.trackedFreq : bs.freq;
            if (std::abs (lf - st.lastListenFreq) > 1.0f || ! juce::exactlyEqual (bs.q, st.lastQ))
            {
                if (settings.mode == band) st.listenCoeffs.bandPass (lf, bs.q, fs);
                else                       st.listenCoeffs.highPass (lf, 0.707, fs);
                st.lastListenFreq = lf; st.lastQ = bs.q;
            }
        }

        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0;
        SvfCoeffs bodyHp, bodyLpFull, bodyLpLow;
        SvfState hpS, lpFullS, lpLowS;
        std::array<BandState, numBands> bandState;
        float envFull = 0.0f, envLow = 0.0f, voiceRefDb = -100.0f, wideGain = 1.0f, wideStep = 0.0f;
        float displayLevel = -100.0f, displayGr = 0.0f;
        LevelHistory history;
        std::atomic<float> currentGr { 0.0f };
        std::array<std::atomic<float>, numBands> grBand {}, freqBand {};
    };
}
