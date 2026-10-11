// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <cmath>
#include <complex>
#include <algorithm>

// Zero-latency trapezoidal state-variable filter (Andrew Simper / Cytomic).
// Stable under fast coefficient modulation, which matters for the dynamic EQ,
// the de-esser and notches that move while audio is playing.
namespace dsp
{
    struct SvfCoeffs
    {
        double g = 0.0, k = 1.0;            // analog-prototype params
        double a1 = 1.0, a2 = 0.0, a3 = 0.0;
        double m0 = 1.0, m1 = 0.0, m2 = 0.0;
        int order = 2;                      // 1 = one-pole (6 dB/oct) stage
        double G1 = 0.0;                    // one-pole coefficient g/(1+g)

        static constexpr double pi = 3.14159265358979323846;

        static double prewarp (double freq, double fs)
        {
            const double f = std::clamp (freq, 5.0, fs * 0.49);
            return std::tan (pi * f / fs);
        }

        void finish()
        {
            order = 2;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        // ---- designs (gainDb only used where it matters) ----
        void bell (double freq, double q, double gainDb, double fs)       { bellFromG (prewarp (freq, fs), q, gainDb); }
        void bellFromG (double g0, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            g = g0; k = 1.0 / (q * A);
            m0 = 1.0; m1 = k * (A * A - 1.0); m2 = 0.0;
            finish();
        }
        void lowShelf (double freq, double q, double gainDb, double fs)   { lowShelfFromG (prewarp (freq, fs), q, gainDb); }
        void lowShelfFromG (double g0, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            g = g0 / std::sqrt (A); k = 1.0 / q;
            m0 = 1.0; m1 = k * (A - 1.0); m2 = A * A - 1.0;
            finish();
        }
        void highShelf (double freq, double q, double gainDb, double fs)  { highShelfFromG (prewarp (freq, fs), q, gainDb); }
        void highShelfFromG (double g0, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            g = g0 * std::sqrt (A); k = 1.0 / q;
            m0 = A * A; m1 = k * (1.0 - A) * A; m2 = 1.0 - A * A;
            finish();
        }
        void lowPass (double freq, double q, double fs)  { g = prewarp (freq, fs); k = 1.0 / q; m0 = 0; m1 = 0;  m2 = 1;  finish(); }
        void highPass (double freq, double q, double fs) { g = prewarp (freq, fs); k = 1.0 / q; m0 = 1; m1 = -k; m2 = -1; finish(); }
        void bandPass (double freq, double q, double fs) { g = prewarp (freq, fs); k = 1.0 / q; m0 = 0; m1 = k;  m2 = 0;  finish(); } // 0 dB peak
        // first-order (6 dB/oct) stages
        void lowPass1 (double freq, double fs)  { g = prewarp (freq, fs); k = 1.0; G1 = g / (1.0 + g); order = 1; m0 = 0; m1 = 0; m2 = 1; }
        void highPass1 (double freq, double fs) { g = prewarp (freq, fs); k = 1.0; G1 = g / (1.0 + g); order = 1; m0 = 1; m1 = 0; m2 = -1; }
        void identity() { g = 0.1; k = 1.0; m0 = 1; m1 = 0; m2 = 0; finish(); }

        // Exact digital response: the TPT SVF is the bilinear transform of the
        // analog SVF, so evaluate the prototype at s = j*tan(pi f/fs)/g.
        std::complex<double> response (double freq, double fs) const
        {
            const std::complex<double> s (0.0, prewarp (freq, fs) / g);
            if (order == 1)
            {
                const auto lp = 1.0 / (s + 1.0);
                return m0 + m2 * lp;
            }
            const auto den = s * s + k * s + 1.0;
            return (m0 * den + m1 * s + m2) / den;
        }

        double magnitudeDb (double freq, double fs) const
        {
            return 20.0 * std::log10 (std::max (1.0e-9, std::abs (response (freq, fs))));
        }
    };

    struct SvfState
    {
        double ic1 = 0.0, ic2 = 0.0;
        void reset() { ic1 = ic2 = 0.0; }

        inline float process (float in, const SvfCoeffs& c) noexcept
        {
            if (c.order == 1)
            {
                const double v = (in - ic1) * c.G1;
                const double lp = v + ic1;
                ic1 = lp + v;
                return (float) (c.m0 * in + c.m2 * lp);
            }
            const double v0 = in;
            const double v3 = v0 - ic2;
            const double v1 = c.a1 * ic1 + c.a2 * v3;
            const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            return (float) (c.m0 * v0 + c.m1 * v1 + c.m2 * v2);
        }
    };

    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
    inline float gainToDb (float g) noexcept  { return 20.0f * std::log10 (std::max (g, 1.0e-6f)); }

    // One-pole smoothing coefficient for a time constant in ms.
    inline float timeCoeff (float ms, double fs) noexcept
    {
        return (float) std::exp (-1.0 / (std::max (0.01, (double) ms) * 0.001 * fs));
    }
}
