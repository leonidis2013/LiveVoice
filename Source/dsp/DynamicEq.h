#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "../Params.h"

// Six-band zero-latency dynamic EQ. Each band is a static EQ curve plus an
// optional dynamic gain driven by a band-limited detector (no look-ahead).
namespace dsp
{
    class DynamicEq
    {
    public:
        static constexpr int numBands = params::numEqBands;
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 16;   // samples between coefficient updates

        struct BandSettings
        {
            bool on = false;
            params::BandType type = params::BandType::bell;
            float freq = 1000.0f, gainDb = 0.0f, q = 1.0f;
            bool dynamic = false;
            float thresholdDb = -30.0f, rangeDb = -6.0f, ratio = 3.0f, attackMs = 5.0f, releaseMs = 120.0f;
        };

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
                b.displayDynGain.store (0.0f);
            }
            counter = 0;
        }

        void setBand (int index, const BandSettings& s) noexcept { bands[(size_t) index].settings = s; }

        float getDynamicGainDb (int index) const noexcept { return bands[(size_t) index].displayDynGain.load (std::memory_order_relaxed); }

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
                        const float p = d * d;
                        const float c = p > b.envPow ? b.attCoeff : b.relCoeff;
                        b.envPow = c * b.envPow + (1.0f - c) * p;
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

        // Coefficients describing the band as the UI should draw it.
        static void designBand (const BandSettings& s, double freq, double q, double gainDb, double sampleRate,
                                SvfCoeffs out[2], int& numStages)
        {
            using BT = params::BandType;
            numStages = 1;
            switch (s.type)
            {
                case BT::bell:       out[0].bell (freq, q, gainDb, sampleRate); break;
                case BT::lowShelf:   out[0].lowShelf (freq, juce::jlimit (0.3, 2.0, q), gainDb, sampleRate); break;
                case BT::highShelf:  out[0].highShelf (freq, juce::jlimit (0.3, 2.0, q), gainDb, sampleRate); break;
                case BT::highPass12: out[0].highPass (freq, juce::jlimit (0.3, 2.0, q), sampleRate); break;
                case BT::lowPass12:  out[0].lowPass (freq, juce::jlimit (0.3, 2.0, q), sampleRate); break;
                case BT::highPass24:
                    out[0].highPass (freq, 0.54119610, sampleRate); out[1].highPass (freq, 1.30656296, sampleRate); numStages = 2; break;
                case BT::lowPass24:
                    out[0].lowPass (freq, 0.54119610, sampleRate);  out[1].lowPass (freq, 1.30656296, sampleRate);  numStages = 2; break;
            }
        }

    private:
        struct Band
        {
            BandSettings settings;
            std::array<SvfCoeffs, 2> coeffs;
            std::array<std::array<SvfState, maxChannels>, 2> state;
            SvfCoeffs detCoeffs;
            SvfState det;
            int numStages = 1;
            bool active = false, detecting = false, initialised = false;
            float envPow = 0.0f, dynGainDb = 0.0f;
            float attCoeff = 0.0f, relCoeff = 0.0f;
            float smoothedFreq = 1000.0f, smoothedGain = 0.0f, smoothedQ = 1.0f;
            BandSettings lastDetSettings;
            bool detValid = false;
            std::atomic<float> displayDynGain { 0.0f };
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
                if (! b.detValid || ! juce::exactlyEqual (b.lastDetSettings.freq, s.freq) || ! juce::exactlyEqual (b.lastDetSettings.q, s.q) || b.lastDetSettings.type != s.type)
                {
                    using BT = params::BandType;
                    if (s.type == BT::lowShelf)       b.detCoeffs.lowPass (s.freq, 0.707, fs);
                    else if (s.type == BT::highShelf) b.detCoeffs.highPass (s.freq, 0.707, fs);
                    else                              b.detCoeffs.bandPass (s.freq, std::max (0.5f, s.q), fs);
                    b.lastDetSettings = s;
                    b.detValid = true;
                }

                b.attCoeff = timeCoeff (s.attackMs, fs);
                b.relCoeff = timeCoeff (s.releaseMs, fs);

                // sine-peak referenced level: 10log10(meanSquare) + 3 dB
                const float levelDb = 10.0f * std::log10 (std::max (b.envPow, 1.0e-12f)) + 3.0103f;
                const float over = levelDb - s.thresholdDb;
                float target = 0.0f;
                if (over > 0.0f)
                {
                    const float amount = over * (1.0f - 1.0f / std::max (1.0f, s.ratio));
                    target = s.rangeDb >= 0.0f ? std::min (amount, s.rangeDb) : std::max (-amount, s.rangeDb);
                }
                // 1 ms de-zipper (attack/release already shape the envelope)
                const float z = std::exp (-1.0f / (0.001f * (float) ctlFs));
                b.dynGainDb = z * b.dynGainDb + (1.0f - z) * target;
            }
            else
            {
                b.envPow = 0.0f;
                b.dynGainDb = 0.0f;
            }

            b.displayDynGain.store (b.dynGainDb, std::memory_order_relaxed);

            SvfCoeffs c[2];
            int stages = 1;
            designBand (s, b.smoothedFreq, b.smoothedQ, b.smoothedGain + b.dynGainDb, fs, c, stages);
            if (stages != b.numStages)
                for (auto& st : b.state[1]) st.reset();
            b.coeffs[0] = c[0];
            b.coeffs[1] = c[1];
            b.numStages = stages;
        }

        std::array<Band, numBands> bands;
        double fs = 48000.0, ctlFs = 3000.0;
        int channels = 2, counter = 0;
    };
}
