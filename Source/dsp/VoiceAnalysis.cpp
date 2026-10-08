#include "VoiceAnalysis.h"
#include "Svf.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dsp::analysis
{
    namespace
    {
        // Short-time spectra. Calls fn(frameStart, power[]) for each Hann-windowed frame.
        template <typename Fn>
        void forEachFrame (const float* x, int n, int order, int hop, Fn&& fn)
        {
            const int size = 1 << order;
            if (n < size) return;
            juce::dsp::FFT fft (order);
            std::vector<float> window ((size_t) size), buf ((size_t) size * 2), power ((size_t) size / 2 + 1);
            for (int i = 0; i < size; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) size);

            for (int start = 0; start + size <= n; start += hop)
            {
                for (int i = 0; i < size; ++i) buf[(size_t) i] = x[start + i] * window[(size_t) i];
                std::fill (buf.begin() + size, buf.end(), 0.0f);
                fft.performFrequencyOnlyForwardTransform (buf.data(), true);
                for (int k = 0; k <= size / 2; ++k) power[(size_t) k] = buf[(size_t) k] * buf[(size_t) k];
                fn (start, power);
            }
        }

        double bandSum (const std::vector<float>& p, double binHz, double lo, double hi)
        {
            const int k0 = std::max (1, (int) std::ceil (lo / binHz)), k1 = std::min ((int) p.size() - 1, (int) std::floor (hi / binHz));
            double s = 0.0;
            for (int k = k0; k <= k1; ++k) s += p[(size_t) k];
            return s;
        }

        // Fractional-octave smoothing of a power spectrum, returned in dB.
        std::vector<float> smoothDb (const std::vector<double>& p, double binHz, double fraction)
        {
            std::vector<double> prefix (p.size() + 1, 0.0);
            for (size_t k = 0; k < p.size(); ++k) prefix[k + 1] = prefix[k] + p[k];
            const double half = std::pow (2.0, 0.5 / fraction);
            std::vector<float> out (p.size(), -200.0f);
            for (size_t k = 1; k < p.size(); ++k)
            {
                const double f = (double) k * binHz;
                const int k0 = std::max (1, (int) std::floor (f / half / binHz));
                const int k1 = std::min ((int) p.size() - 1, std::max (k0, (int) std::ceil (f * half / binHz)));
                const double mean = (prefix[(size_t) k1 + 1] - prefix[(size_t) k0]) / (double) (k1 - k0 + 1);
                out[k] = (float) (10.0 * std::log10 (mean + 1.0e-20));
            }
            return out;
        }

        float percentile (std::vector<float> v, float pct)
        {
            if (v.empty()) return 0.0f;
            std::sort (v.begin(), v.end());
            const size_t idx = (size_t) juce::jlimit (0.0f, (float) v.size() - 1.0f, pct / 100.0f * (float) (v.size() - 1));
            return v[idx];
        }

        int binFor (double f, double binHz, size_t size) { return juce::jlimit (1, (int) size - 1, (int) std::round (f / binHz)); }
    }

    DeEssSuggestion suggestDeEss (const float* x, int n, double fs)
    {
        DeEssSuggestion out;
        constexpr int order = 10, size = 1 << order, hop = size / 2;
        const double binHz = fs / size;

        // pass 1: frame levels, to find what counts as "talking"
        std::vector<double> levels;
        forEachFrame (x, n, order, hop, [&] (int, const std::vector<float>& p) { levels.push_back (bandSum (p, binHz, 100.0, 16000.0)); });
        if (levels.empty()) return out;
        const double maxLevel = *std::max_element (levels.begin(), levels.end());
        if (maxLevel <= 1.0e-12) return out;

        // pass 2: sibilant frames = most of the energy is between 4 and 11 kHz
        std::vector<double> avg ((size_t) size / 2 + 1, 0.0);
        std::vector<int> sibStarts;
        size_t frame = 0;
        forEachFrame (x, n, order, hop, [&] (int start, const std::vector<float>& p)
        {
            const double all = levels[frame++];
            const double hf = bandSum (p, binHz, 4000.0, 11000.0);
            if (all > maxLevel * 1.0e-4 && hf > 0.5 * all)   // within 40 dB of the loudest frame
            {
                for (size_t k = 0; k < avg.size(); ++k) avg[k] += p[k];
                sibStarts.push_back (start);
            }
        });
        out.sibilantFrames = (int) sibStarts.size();
        if (sibStarts.size() < 6) return out;   // not enough "s" sounds heard

        // where the sibilance peaks, and how wide it is (-6 dB points)
        const auto sm = smoothDb (avg, binHz, 6.0);
        const int kLo = binFor (3000.0, binHz, sm.size()), kHi = binFor (11000.0, binHz, sm.size());
        int kPeak = kLo;
        for (int k = kLo; k <= kHi; ++k) if (sm[(size_t) k] > sm[(size_t) kPeak]) kPeak = k;
        const float pk = sm[(size_t) kPeak];
        int kL = kPeak, kR = kPeak;
        while (kL > 1 && sm[(size_t) kL] > pk - 6.0f) --kL;
        while (kR < (int) sm.size() - 1 && sm[(size_t) kR] > pk - 6.0f) ++kR;
        out.freq = (float) (kPeak * binHz);
        out.q = juce::jlimit (1.5f, 6.0f, (float) (out.freq / std::max (binHz, (kR - kL) * binHz)));

        // threshold: run the de-esser's own detectors and look at how loud the sibilants measure
        SvfCoeffs band, full;
        band.bandPass (out.freq, out.q, fs);
        full.highPass (80.0, 0.707, fs);
        SvfState sb, sf;
        const float att = timeCoeff (0.5f, fs), rel = timeCoeff (80.0f, fs);
        float envB = 0.0f, envF = 0.0f;
        std::vector<float> absDb ((size_t) n), relDb ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const float b = sb.process (x[i], band), f = sf.process (x[i], full);
            envB = (b * b > envB ? att : rel) * envB + (1.0f - (b * b > envB ? att : rel)) * b * b;
            envF = (f * f > envF ? att : rel) * envF + (1.0f - (f * f > envF ? att : rel)) * f * f;
            const float bDb = 10.0f * std::log10 (envB + 1.0e-12f) + 3.0103f;
            const float fDb = 10.0f * std::log10 (envF + 1.0e-12f) + 3.0103f;
            absDb[(size_t) i] = bDb;
            relDb[(size_t) i] = bDb - fDb;
        }
        std::vector<float> absPeaks, relPeaks;
        for (int start : sibStarts)
        {
            float a = -200.0f, r = -200.0f;
            for (int i = start; i < std::min (n, start + size); ++i) { a = std::max (a, absDb[(size_t) i]); r = std::max (r, relDb[(size_t) i]); }
            absPeaks.push_back (a); relPeaks.push_back (r);
        }
        // typical "s" gets ~3 dB of reduction, louder ones more (up to the Range)
        out.thresholdDb    = juce::jlimit (-60.0f, 0.0f, percentile (absPeaks, 50.0f) - 4.0f);
        out.relThresholdDb = juce::jlimit (-40.0f, 6.0f, percentile (relPeaks, 50.0f) - 4.0f);
        out.ok = true;
        return out;
    }

    EqSuggestion suggestEq (const float* x, int n, double fs)
    {
        EqSuggestion out;
        constexpr int order = 12, size = 1 << order, hop = size / 2;
        const double binHz = fs / size;

        std::vector<double> levels;
        forEachFrame (x, n, order, hop, [&] (int, const std::vector<float>& p) { levels.push_back (bandSum (p, binHz, 60.0, 16000.0)); });
        if (levels.empty()) return out;
        const double maxLevel = *std::max_element (levels.begin(), levels.end());
        if (maxLevel <= 1.0e-12) return out;

        // long-term spectrum of voiced speech (loud enough, not sibilant)
        std::vector<double> avg ((size_t) size / 2 + 1, 0.0);
        size_t frame = 0;
        int voiced = 0;
        forEachFrame (x, n, order, hop, [&] (int, const std::vector<float>& p)
        {
            const double all = levels[frame++];
            const double hf = bandSum (p, binHz, 4000.0, 11000.0);
            if (all > maxLevel * 1.0e-3 && hf < 0.3 * all)   // within 30 dB of the loudest frame
            {
                for (size_t k = 0; k < avg.size(); ++k) avg[k] += p[k];
                ++voiced;
            }
        });
        out.voicedFrames = voiced;
        if (voiced < 20) return out;   // need ~1 s of actual voice

        const auto fine = smoothDb (avg, binHz, 6.0);    // 1/6 octave
        const auto trend = smoothDb (avg, binHz, 0.67);  // ~1.5 octave

        // bands 2..5: look for a build-up above the voice's own trend within each band's usual range
        const float ranges[4][2] { { 140.0f, 350.0f }, { 300.0f, 700.0f }, { 700.0f, 1600.0f }, { 2000.0f, 5000.0f } };
        const float defaults[4] { 200.0f, 450.0f, 1000.0f, 3000.0f };
        const float qs[4] { 1.2f, 1.4f, 1.4f, 1.2f };
        for (int b = 0; b < 4; ++b)
        {
            const int k0 = binFor (ranges[b][0], binHz, fine.size()), k1 = binFor (ranges[b][1], binHz, fine.size());
            int best = k0;
            for (int k = k0; k <= k1; ++k)
                if (fine[(size_t) k] - trend[(size_t) k] > fine[(size_t) best] - trend[(size_t) best]) best = k;
            const float excess = fine[(size_t) best] - trend[(size_t) best];
            if (excess > 2.5f)
            {
                const float cut = std::round (std::min (4.0f, 0.6f * excess) * 2.0f) / 2.0f;
                out.bands[b] = { (float) (best * binHz), -std::max (1.0f, cut), qs[b] };
            }
            else
                out.bands[b] = { defaults[b], 0.0f, qs[b] };
        }

        // high-pass: just below where the voice's low end really starts
        float body = -200.0f;
        for (int k = binFor (80.0, binHz, fine.size()); k <= binFor (400.0, binHz, fine.size()); ++k) body = std::max (body, fine[(size_t) k]);
        float lowest = 300.0f;
        for (int k = binFor (50.0, binHz, fine.size()); k <= binFor (300.0, binHz, fine.size()); ++k)
            if (fine[(size_t) k] >= body - 12.0f) { lowest = (float) (k * binHz); break; }
        out.hpFreq = juce::jlimit (60.0f, 150.0f, std::round (lowest * 0.7f / 5.0f) * 5.0f);

        // air: only lift the top if the voice is noticeably dull
        auto meanDb = [&] (float lo, float hi)
        {
            double s = 0.0; int c = 0;
            for (int k = binFor (lo, binHz, fine.size()); k <= binFor (hi, binHz, fine.size()); ++k) { s += fine[(size_t) k]; ++c; }
            return (float) (s / std::max (1, c));
        };
        const float tilt = meanDb (8000.0f, 12000.0f) - meanDb (2000.0f, 4000.0f);
        out.airGainDb = tilt < -22.0f ? 2.0f : (tilt < -16.0f ? 1.0f : 0.0f);
        out.ok = true;
        return out;
    }
}
