#pragma once

#include <vector>

// Offline analysis of a short recording of the voice (15-30 s), used by the
// de-esser and EQ "Suggest" buttons. Runs on the message thread after the
// capture finishes; nothing here touches the audio path.
namespace dsp::analysis
{
    struct DeEssSuggestion
    {
        bool ok = false;
        int sibilantFrames = 0;
        float freq = 6500.0f, q = 1.0f;
        float thresholdDb = -30.0f;      // absolute mode
        float relThresholdDb = -6.0f;    // relative mode (sibilance band vs voice body)
    };

    struct EqSuggestion
    {
        bool ok = false;
        int voicedFrames = 0;
        float hpFreq = 80.0f;
        struct Band { float freq, gainDb, q; };
        Band bands[4] {};                // EQ bands 2..5 (boom, box, nasal, presence)
        float airGainDb = 0.0f;          // band 6 high shelf
    };

    // Finds where the "s" energy peaks and how wide it is, then a threshold that
    // trims the louder sibilants by a few dB without touching the soft ones.
    DeEssSuggestion suggestDeEss (const float* x, int n, double fs);

    // Long-term spectrum of voiced speech compared with its own smoothed trend:
    // resonant build-ups in each band's typical range become gentle cut suggestions.
    EqSuggestion suggestEq (const float* x, int n, double fs);
}
