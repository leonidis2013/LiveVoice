#pragma once

#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"

// Zero-latency safety limiter: instant attack (no look-ahead), smooth
// release, and a final hard ceiling so nothing past the ceiling ever leaves
// the plugin. It is a last line of defence, not a tone tool.
namespace dsp
{
    class SafetyLimiter
    {
    public:
        void prepare (double sampleRate) { fs = sampleRate; reset(); }
        void reset() { gain = 1.0f; currentGrDb.store (0.0f); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples, bool enabled, float ceilingDb) noexcept
        {
            if (! enabled)
            {
                gain = 1.0f;
                currentGrDb.store (0.0f, std::memory_order_relaxed);
                return;
            }

            const float ceil = dbToGain (ceilingDb);
            const float rel = timeCoeff (80.0f, fs);
            const int nCh = std::min (2, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
            float minGain = 1.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                const float peak = std::max (std::abs (L[i]), R != nullptr ? std::abs (R[i]) : 0.0f);
                const float target = peak > ceil ? ceil / peak : 1.0f;
                gain = target < gain ? target : rel * gain + (1.0f - rel) * target;
                minGain = std::min (minGain, gain);

                L[i] = juce::jlimit (-ceil, ceil, L[i] * gain);
                if (R != nullptr) R[i] = juce::jlimit (-ceil, ceil, R[i] * gain);
            }

            currentGrDb.store (-gainToDb (minGain), std::memory_order_relaxed);
        }

        float getGainReductionDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    private:
        double fs = 48000.0;
        float gain = 1.0f;
        std::atomic<float> currentGrDb { 0.0f };
    };
}
