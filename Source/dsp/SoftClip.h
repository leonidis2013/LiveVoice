// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

// Output protection that is not a limiter: samples below -1 dBFS pass bit-for-bit unchanged;
// only the top 1 dB is rounded off so nothing ever exceeds full scale (0 dBFS). Zero latency.
namespace dsp
{
    struct SoftClip
    {
        static constexpr float knee = 0.89125094f;   // -1 dBFS

        static float process (float x) noexcept
        {
            const float a = std::abs (x);
            if (a <= knee) return x;
            const float y = knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee));
            return std::copysign (y, x);
        }

        static void process (float* d, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
                if (std::abs (d[i]) > knee) d[i] = process (d[i]);
        }
    };
}
