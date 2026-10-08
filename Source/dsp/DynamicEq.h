#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "../Params.h"

// Six-band zero-latency dynamic EQ. Each band is a static EQ curve plus an
// optional dynamic gain driven by a band-limited detector (no look-ahead).
//
// The dynamic gain is proportional to how far the band's level is above the
// threshold: a soft knee eases it in, the ratio sets how much it moves per dB,
// and Range is approached gradually (not a hard stop), so the band follows the
// voice smoothly instead of switching between "off" and "full range".
namespace dsp
{
    class DynamicEq
    {
    public:
        static constexpr int numBands = params::numEqBands;
        static constexpr int maxChannels = 2;
        static constexpr int maxStages = 4;            // up to 48 dB/oct
        static constexpr int controlInterval = 16;     // samples between coefficient updates
        static constexpr float kneeDb = 6.0f;

        struct BandSettings
        {
            bool on = false;
            params::BandType type = params::BandType::bell;
            int slope = 12;                            // dB/oct, pass types only
            float freq = 1000.0f, gainDb = 0.0f, q = 1.0f;
            bool dynamic = false;
            float thresholdDb = -30.0f, rangeDb = -6.0f, ratio = 2.0f, attackMs = 10.0f, releaseMs = 120.0f;
        };

        // Dynamic gain (dB, signed like Range) for a detector level. Proportional with a soft knee;
        // the range is reached asymptotically so there is no audible "hit the stop" point.
        static float dynamicGainFor (float levelDb, const BandSettings& s) noexcept
        {
            const float slope = 1.0f - 1.0f / std::max (1.0f, s.ratio);
            const float d = levelDb - s.thresholdDb;
            float amount;
            if (2.0f * d <= -kneeDb)      amount = 0.0f;
            else if (2.0f * d < kneeDb)   amount = slope * (d + kneeDb * 0.5f) * (d + kneeDb * 0.5f) / (2.0f * kneeDb);
            else                          amount = slope * d;

            const float range = std::abs (s.rangeDb);
            if (range < 0.01f) return 0.0f;
            const float limited = range * std::tanh (amount / range);
            return s.rangeDb >= 0.0f ? limited : -limited;
        }

        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            channels = juce::jlimit (1, maxChannels, numChannels);
            ctlFs = fs / controlInterval;
            reset();
        }

        void reset()
        {
            for (auto& b : bands)
            {
                for (auto& stage : b.state) for (auto& s : stage) s.reset();
                b.det.reset();
                b.envPow = 0.0f;
                b.dynGainDb = 0.0f;
                b.smoothedFreq = b.settings.freq;
                b.smoothedGain = b.settings.gainDb;
                b.smoothedQ = b.settings.q;
                b.initialised = false;
                b.detValid = false;
                b.displayDynGain.store (0.0f);
                b.displayLevel.store (-100.0f);
            }
            counter = 0;
        }

        void setBand (int index, const BandSettings& s) noexcept { bands[(size_t) index].settings = s; }

        float getDynamicGainDb (int index) const noexcept   { return bands[(size_t) index].displayDynGain.load (std::memory_order_relaxed); }
        float getDetectorLevelDb (int index) const noexcept { return bands[(size_t) index].displayLevel.load (std::memory_order_relaxed); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
        {
            const int nCh = std::min (channels, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            for (auto& b : bands)
                if (! b.initialised) initBand (b);

            for (int i = 0; i < numSamples; ++i)
            {
                if (counter == 0)
                    for (auto& b : bands) updateControl (b);
                counter = (counter + 1) % controlInterval;

                float l = L[i];
                float r = R != nullptr ? R[i] : 0.0f;

                for (auto& b : bands)
                {
                    if (! b.active) continue;

                    if (b.detecting)
                    {
                        const float mono = R != nullptr ? 0.5f * (l + r) : l;
                        const float d = b.det.process (mono, b.detCoeffs);
                        b.envPow = b.rmsCoeff * b.envPow + (1.0f - b.rmsCoeff) * d * d;
                    }

                    for (int st = 0; st < b.numStages; ++st)
                    {
                        l = b.state[(size_t) st][0].process (l, b.coeffs[(size_t) st]);
                        if (R != nullptr) r = b.state[(size_t) st][1].process (r, b.coeffs[(size_t) st]);
                    }
                }

                L[i] = l;
                if (R != nullptr) R[i] = r;
            }
        }

        // Coefficients describing the band (used for audio and for drawing).
        static void designBand (const BandSettings& s, double freq, double q, double gainDb, double sampleRate,
                                SvfCoeffs out[maxStages], int& numStages)
        {
            using BT = params::BandType;
            numStages = 1;
            switch (s.type)
            {
                case BT::bell:      out[0].bell (freq, q, gainDb, sampleRate); return;
                case BT::lowShelf:  out[0].lowShelf (freq, juce::jlimit (0.3, 2.0, q), gainDb, sampleRate); return;
                case BT::highShelf: out[0].highShelf (freq, juce::jlimit (0.3, 2.0, q), gainDb, sampleRate); return;
                case BT::highPass:
                case BT::lowPass:   break;
            }

            const bool hp = s.type == BT::highPass;
            auto second = [&] (SvfCoeffs& c, double qq) { if (hp) c.highPass (freq, qq, sampleRate); else c.lowPass (freq, qq, sampleRate); };
            auto first  = [&] (SvfCoeffs& c)            { if (hp) c.highPass1 (freq, sampleRate);   else c.lowPass1 (freq, sampleRate); };

            switch (s.slope)   // Butterworth cascades
            {
                case 6:  first (out[0]); break;
                case 18: first (out[0]); second (out[1], 1.0); numStages = 2; break;
                case 24: second (out[0], 0.54119610); second (out[1], 1.30656296); numStages = 2; break;
                case 36: second (out[0], 0.51763809); second (out[1], 0.70710678); second (out[2], 1.93185165); numStages = 3; break;
                case 48: second (out[0], 0.50979558); second (out[1], 0.60134489); second (out[2], 0.89997622);
                         second (out[3], 2.56291545); numStages = 4; break;
                default: second (out[0], juce::jlimit (0.5, 2.0, q)); break;   // 12 dB/oct, resonance from Q
            }
        }

        static bool usesQ (const BandSettings& s) noexcept
        {
            return ! params::isPassType (s.type) || s.slope == 12;
        }

    private:
        struct Band
        {
            BandSettings settings;
            std::array<SvfCoeffs, maxStages> coeffs;
            std::array<std::array<SvfState, maxChannels>, maxStages> state;
            SvfCoeffs detCoeffs;
            SvfState det;
            int numStages = 1;
            bool active = false, detecting = false, initialised = false;
            float envPow = 0.0f, dynGainDb = 0.0f, rmsCoeff = 0.0f;
            float smoothedFreq = 1000.0f, smoothedGain = 0.0f, smoothedQ = 1.0f;
            BandSettings lastDetSettings;
            bool detValid = false;
            std::atomic<float> displayDynGain { 0.0f }, displayLevel { -100.0f };
        };

        void initBand (Band& b)
        {
            b.smoothedFreq = b.settings.freq;
            b.smoothedGain = b.settings.gainDb;
            b.smoothedQ = b.settings.q;
            b.initialised = true;
        }

        void updateControl (Band& b) noexcept
        {
            const auto& s = b.settings;
            b.active = s.on;
            if (! b.active)
            {
                b.dynGainDb = 0.0f;
                b.displayDynGain.store (0.0f, std::memory_order_relaxed);
                b.displayLevel.store (-100.0f, std::memory_order_relaxed);
                return;
            }

            // ~20 ms smoothing of user parameter moves (control rate)
            const float k = std::exp (-1.0f / (0.02f * (float) ctlFs));
            b.smoothedFreq = std::exp (k * std::log (b.smoothedFreq) + (1.0f - k) * std::log (s.freq));
            b.smoothedGain = k * b.smoothedGain + (1.0f - k) * s.gainDb;
            b.smoothedQ    = k * b.smoothedQ + (1.0f - k) * s.q;

            const bool pass = params::isPassType (s.type);
            b.detecting = s.dynamic && ! pass;

            if (b.detecting)
            {
                // detector filter follows the band
                if (! b.detValid || ! juce::exactlyEqual (b.lastDetSettings.freq, s.freq)
                      || ! juce::exactlyEqual (b.lastDetSettings.q, s.q) || b.lastDetSettings.type != s.type)
                {
                    using BT = params::BandType;
                    if (s.type == BT::lowShelf)       b.detCoeffs.lowPass (s.freq, 0.707, fs);
                    else if (s.type == BT::highShelf) b.detCoeffs.highPass (s.freq, 0.707, fs);
                    else                              b.detCoeffs.bandPass (s.freq, std::max (0.5f, s.q), fs);
                    // RMS window: at least ~2 cycles of the band, so low bands don't ripple (distort)
                    b.rmsCoeff = timeCoeff (std::max (5.0f, 2000.0f / std::max (20.0f, s.freq)), fs);
                    b.lastDetSettings = s;
                    b.detValid = true;
                }

                const float levelDb = 10.0f * std::log10 (std::max (b.envPow, 1.0e-12f)) + 3.0103f;   // sine-peak referenced
                const float target = dynamicGainFor (levelDb, s);

                // attack = moving away from 0 dB, release = returning toward it (dB domain, smooth)
                const bool engaging = std::abs (target) > std::abs (b.dynGainDb);
                const float t = engaging ? s.attackMs : s.releaseMs;
                const float c = std::exp (-1.0f / (std::max (0.1f, t) * 0.001f * (float) ctlFs));
                b.dynGainDb = c * b.dynGainDb + (1.0f - c) * target;
                b.displayLevel.store (levelDb, std::memory_order_relaxed);
            }
            else
            {
                b.envPow = 0.0f;
                b.dynGainDb = 0.0f;
                b.displayLevel.store (-100.0f, std::memory_order_relaxed);
            }

            b.displayDynGain.store (b.dynGainDb, std::memory_order_relaxed);

            SvfCoeffs c[maxStages];
            int stages = 1;
            designBand (s, b.smoothedFreq, b.smoothedQ, b.smoothedGain + b.dynGainDb, fs, c, stages);
            for (int st = 0; st < maxStages; ++st)
            {
                const bool structureChanged = st >= b.numStages || c[st].order != b.coeffs[(size_t) st].order;
                if (st < stages && structureChanged)
                    for (auto& x : b.state[(size_t) st]) x.reset();
                b.coeffs[(size_t) st] = c[st];
            }
            b.numStages = stages;
        }

        std::array<Band, numBands> bands;
        double fs = 48000.0, ctlFs = 3000.0;
        int channels = 2, counter = 0;
    };
}
