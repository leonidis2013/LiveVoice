#include "FeedbackDetector.h"
#include <algorithm>

namespace dsp
{
    namespace
    {
        struct ModeConfig { float papr, pnpr, phpr, persistSeconds, minGrowthDb, maxDriftCents; };

        // Speech: react fast, a spoken voice rarely sustains a pure tone.
        // Singing: sung notes sustain, so demand a clearly sine-like, growing peak.
        // Multipurpose: in between.
        constexpr ModeConfig modeConfigs[] {
            { 14.0f, 12.0f, 12.0f, 0.22f, -1.0f, 10.0f },   // speech
            { 16.0f, 14.0f, 14.0f, 0.40f,  0.5f,  6.0f },   // singing
            { 15.0f, 13.0f, 13.0f, 0.30f,  0.0f,  8.0f },   // multipurpose
        };
    }

    FeedbackDetector::FeedbackDetector (FeedbackSuppressor& target)
        : juce::Thread ("LiveVoice feedback detector"), suppressor (target)
    {
        fifoData.resize ((size_t) fifo.getTotalSize(), 0.0f);
    }

    FeedbackDetector::~FeedbackDetector()
    {
        release();
    }

    void FeedbackDetector::prepare (double sampleRate)
    {
        release();

        fs = sampleRate;
        fftOrder = fs > 64000.0 ? 14 : 13;           // keep ~5-6 Hz resolution at any rate
        fftSize = 1 << fftOrder;
        hop = fftSize / 4;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

        window.resize ((size_t) fftSize);
        double sum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) fftSize);
            sum += window[(size_t) i];
        }
        windowNorm = (float) (2.0 / sum);

        history.assign ((size_t) fftSize, 0.0f);
        fftData.assign ((size_t) fftSize * 2, 0.0f);
        magDb.assign ((size_t) fftSize / 2 + 1, -140.0f);
        historyWrite = 0;
        samplesSinceFrame = 0;
        tracks.clear();
        fifo.reset();

        prepared = true;
        startThread (juce::Thread::Priority::low);
    }

    void FeedbackDetector::release()
    {
        stopThread (2000);
        prepared = false;
    }

    void FeedbackDetector::pushSamples (const float* mono, int numSamples) noexcept
    {
        if (! prepared.load (std::memory_order_relaxed))
            return;

        int s1, n1, s2, n2;
        fifo.prepareToWrite (numSamples, s1, n1, s2, n2);   // drops samples if the thread falls behind
        if (n1 > 0) std::copy (mono, mono + n1, fifoData.data() + s1);
        if (n2 > 0) std::copy (mono + n1, mono + n1 + n2, fifoData.data() + s2);
        fifo.finishedWrite (n1 + n2);
    }

    void FeedbackDetector::setSettings (const Settings& s) noexcept
    {
        sEnabled = s.enabled; sLearn = s.learn; sFreeze = s.freeze;
        sMode = s.mode; sSens = s.sensitivity; sLo = s.loFreq; sHi = s.hiFreq;
    }

    void FeedbackDetector::run()
    {
        while (! threadShouldExit())
            if (! analyseIfReady())
                wait (5);
    }

    void FeedbackDetector::processPendingForTest()
    {
        while (analyseIfReady()) {}
    }

    bool FeedbackDetector::analyseIfReady()
    {
        const int available = fifo.getNumReady();
        if (available <= 0)
            return false;

        int s1, n1, s2, n2;
        const int toRead = std::min (available, hop - samplesSinceFrame);
        fifo.prepareToRead (toRead, s1, n1, s2, n2);

        auto take = [this] (const float* src, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                history[(size_t) historyWrite] = src[i];
                historyWrite = (historyWrite + 1) % fftSize;
            }
        };
        take (fifoData.data() + s1, n1);
        take (fifoData.data() + s2, n2);
        fifo.finishedRead (n1 + n2);

        samplesSinceFrame += n1 + n2;
        if (samplesSinceFrame >= hop)
        {
            samplesSinceFrame = 0;
            analyseFrame();
        }
        return true;
    }

    void FeedbackDetector::analyseFrame()
    {
        const bool enabled = sEnabled.load(), learn = sLearn.load(), freeze = sFreeze.load();
        if (! enabled || freeze)
        {
            tracks.clear();
            return;
        }

        // ---- spectrum ----
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = history[(size_t) ((historyWrite + i) % fftSize)] * window[(size_t) i];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

        const int numBins = fftSize / 2;
        for (int k = 0; k <= numBins; ++k)
            magDb[(size_t) k] = 20.0f * std::log10 (fftData[(size_t) k] * windowNorm + 1.0e-7f);

        const float binHz = (float) (fs / fftSize);
        const float lo = std::max (20.0f, sLo.load()), hi = std::min ((float) fs * 0.45f, sHi.load());
        const int loBin = std::max (3, (int) (lo / binHz));
        const int hiBin = std::min (numBins - 4, (int) (hi / binHz));
        if (hiBin <= loBin + 4)
            return;

        // ---- thresholds ----
        const int mode = juce::jlimit (0, 2, sMode.load());
        ModeConfig cfg = modeConfigs[mode];
        const float sens = juce::jlimit (0.0f, 100.0f, sSens.load());
        const float sensOffset = (sens - 50.0f) / 50.0f * 6.0f;        // +-6 dB
        float thrShift = -sensOffset;
        if (learn) { thrShift -= 1.0f; cfg.persistSeconds = std::min (cfg.persistSeconds, 0.18f); cfg.minGrowthDb = std::min (cfg.minGrowthDb, -1.0f); }   // quicker, but just as picky

        const float framesPerSecond = (float) (fs / hop);
        const int persistFrames = std::max (2, (int) std::ceil (cfg.persistSeconds * framesPerSecond));
        const int cooldownFrames = std::max (2, (int) std::ceil (0.25f * framesPerSecond));
        const float absoluteFloor = -66.0f + (50.0f - sens) * 0.2f;

        // average level across the detection range
        double meanDb = 0.0;
        for (int k = loBin; k <= hiBin; ++k) meanDb += magDb[(size_t) k];
        meanDb /= (hiBin - loBin + 1);

        // ---- candidate peaks ----
        struct Peak { int bin; float level; };
        std::vector<Peak> peaks;
        for (int k = loBin; k <= hiBin; ++k)
        {
            const float m = magDb[(size_t) k];
            if (m > absoluteFloor && m > magDb[(size_t) k - 1] && m >= magDb[(size_t) k + 1])
                peaks.push_back ({ k, m });
        }
        std::sort (peaks.begin(), peaks.end(), [] (const Peak& a, const Peak& b) { return a.level > b.level; });
        if (peaks.size() > 8) peaks.resize (8);

        auto levelNear = [this, numBins] (float freqBinF)
        {
            const int c = (int) std::round (freqBinF);
            float m = -140.0f;
            for (int k = c - 2; k <= c + 2; ++k)
                if (k > 0 && k < numBins) m = std::max (m, magDb[(size_t) k]);
            return m;
        };

        for (auto& t : tracks) t.seen = false;

        for (const auto& p : peaks)
        {
            const int k = p.bin;

            // parabolic interpolation for a sub-bin frequency estimate
            const float a = magDb[(size_t) k - 1], b = magDb[(size_t) k], c = magDb[(size_t) k + 1];
            const float denom = a - 2.0f * b + c;
            const float delta = std::abs (denom) > 1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
            const float binF = (float) k + delta;
            const float freq = binF * binHz;
            const float peakDb = b - 0.25f * (a - c) * delta;

            // peak-to-average
            const float papr = peakDb - (float) meanDb;
            if (papr < cfg.papr + thrShift) continue;

            // peak-to-neighbourhood
            const int inner = std::max (3, (int) std::round (freq * 0.05f / binHz));
            const int outer = std::max (inner + 4, (int) std::round (freq * 0.25f / binHz));
            double nSum = 0.0; int nCount = 0;
            for (int d = inner; d <= outer; ++d)
            {
                if (k - d > 0)       { nSum += magDb[(size_t) (k - d)]; ++nCount; }
                if (k + d < numBins) { nSum += magDb[(size_t) (k + d)]; ++nCount; }
            }
            const float pnpr = peakDb - (float) (nSum / std::max (1, nCount));
            if (pnpr < cfg.pnpr + thrShift) continue;

            // peak-to-harmonic: a sine has no partials above or below it. Besides 2f, 3f, f/2, f/3,
            // check the neighbouring members of a harmonic series (k+-1)/k: if this peak is the
            // 3rd-6th harmonic of a voice, its neighbours sit at those ratios.
            float strongestRelative = -140.0f;
            const float related[] { 2.0f, 3.0f, 0.5f, 1.0f / 3.0f,
                                    1.5f, 2.0f / 3.0f, 4.0f / 3.0f, 0.75f, 1.25f, 0.8f, 1.2f, 5.0f / 6.0f };
            for (float r : related)
            {
                const float hb = binF * r;
                if (hb * binHz < 50.0f || hb >= (float) numBins - 3.0f) continue;
                strongestRelative = std::max (strongestRelative, levelNear (hb));
            }
            const float phpr = peakDb - strongestRelative;
            if (phpr < cfg.phpr + thrShift) continue;

            // ---- persistence tracking ----
            const float tolBins = std::max (2.0f, binF * (std::pow (2.0f, 1.0f / 24.0f) - 1.0f));
            Track* track = nullptr;
            for (auto& t : tracks)
                if (std::abs (t.freq / binHz - binF) <= tolBins) { track = &t; break; }

            if (track == nullptr)
            {
                tracks.push_back ({});
                track = &tracks.back();
                track->freq = freq;
            }

            if (track->hits == 0) { track->firstLevel = peakDb; track->minFreq = track->maxFreq = freq; }
            track->minFreq = std::min (track->minFreq, freq);
            track->maxFreq = std::max (track->maxFreq, freq);
            track->hits++;
            track->misses = 0;
            track->lastLevel = peakDb;
            track->freq = 0.5f * (track->freq + freq);
            track->seen = true;

            const bool growing = (track->lastLevel - track->firstLevel) >= cfg.minGrowthDb;
            // pitch stability: allow the larger of the mode's cents limit or ~0.15 bin of estimator noise
            const float driftHz = track->maxFreq - track->minFreq;
            const float allowedHz = std::max (0.15f * binHz, freq * (std::pow (2.0f, cfg.maxDriftCents / 1200.0f) - 1.0f));
            if (driftHz > allowedHz)
            {
                track->hits = 0;      // restart: this one wanders like a voice
                continue;
            }

            const bool unmistakable = pnpr >= cfg.pnpr + thrShift + 8.0f && phpr >= cfg.phpr + thrShift + 6.0f;

            // fast path: a pitch-locked peak climbing quickly is a howl starting; don't wait out the full persistence
            const bool runaway = track->hits >= 3 && (track->lastLevel - track->firstLevel) >= 9.0f
                                 && driftHz <= 0.5f * allowedHz;

            if (track->cooldown == 0 && ((track->hits >= persistFrames && (growing || unmistakable)) || runaway))
            {
                FeedbackSuppressor::Command cmd;
                cmd.type = FeedbackSuppressor::Command::Type::detect;
                cmd.freq = track->freq;
                cmd.fixed = learn;
                suppressor.pushDetectorCommand (cmd);
                logQueue.push ({ track->freq, learn });

                lastDetectFreq = track->freq;
                detectCount++;
                track->cooldown = cooldownFrames;
                track->hits = 0;
            }
        }

        for (auto& t : tracks)
        {
            if (t.cooldown > 0) --t.cooldown;
            if (! t.seen && ++t.misses > 2) t.hits = 0;
        }
        tracks.erase (std::remove_if (tracks.begin(), tracks.end(),
                                      [] (const Track& t) { return t.hits == 0 && t.cooldown == 0 && ! t.seen; }),
                      tracks.end());
        if (tracks.size() > 32) tracks.resize (32);
    }
}
