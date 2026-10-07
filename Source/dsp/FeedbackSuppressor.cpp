#include "FeedbackSuppressor.h"

namespace dsp
{
    void FeedbackSuppressor::prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        for (auto& s : slots)
            s.g0 = SvfCoeffs::prewarp (s.freq, fs);
        lastQ = -1.0f;
        reset();
    }

    void FeedbackSuppressor::reset()
    {
        for (auto& s : slots)
            for (auto& st : s.state) st.reset();
        counter = 0;
    }

    void FeedbackSuppressor::process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        Command c;
        while (uiQueue.pop (c))       handleCommand (c);
        while (detectorQueue.pop (c)) handleCommand (c);

        const int nCh = std::min (channels, buffer.getNumChannels());
        auto* L = buffer.getWritePointer (0);
        auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            if (counter == 0)
                updateControl();
            counter = (counter + 1) % controlInterval;

            if (! settings.on)
                continue;

            float l = L[i];
            float r = R != nullptr ? R[i] : 0.0f;

            for (auto& s : slots)
            {
                if (s.kind == kindInactive) continue;
                l = s.state[0].process (l, s.coeffs);
                if (R != nullptr) r = s.state[1].process (r, s.coeffs);
            }

            L[i] = l;
            if (R != nullptr) R[i] = r;
        }

        // advance hold timers once per block
        for (auto& s : slots)
            if (s.kind == kindFloating && ! settings.freeze)
                s.samplesSinceTrigger += numSamples;

        publish();
    }

    void FeedbackSuppressor::handleCommand (const Command& c) noexcept
    {
        using T = Command::Type;
        switch (c.type)
        {
            case T::detect:
                handleDetect (c.freq, c.fixed);
                break;

            case T::setFixed:     // restore from saved state
            {
                const int idx = allocate (true);
                if (idx >= 0) activate (idx, kindFixed, c.freq, c.depthDb, true);
                break;
            }

            case T::clearFloating:
                for (auto& s : slots)
                    if (s.kind == kindFloating) s.targetDb = 0.0f;
                break;

            case T::clearAll:
                for (auto& s : slots)
                    if (s.kind != kindInactive) { s.targetDb = 0.0f; if (s.kind == kindFixed) s.kind = kindFloating; s.samplesSinceTrigger = INT64_MAX / 2; }
                break;

            case T::removeSlot:
                if (juce::isPositiveAndBelow (c.slot, maxSlots) && slots[(size_t) c.slot].kind != kindInactive)
                {
                    auto& s = slots[(size_t) c.slot];
                    s.targetDb = 0.0f;
                    s.kind = kindFloating;
                    s.samplesSinceTrigger = INT64_MAX / 2;
                }
                break;

            case T::makeFixed:
                if (juce::isPositiveAndBelow (c.slot, maxSlots) && slots[(size_t) c.slot].kind == kindFloating)
                {
                    int fixedCount = 0;
                    for (auto& s : slots) fixedCount += s.kind == kindFixed ? 1 : 0;
                    if (fixedCount < settings.maxFixed)
                        slots[(size_t) c.slot].kind = kindFixed;
                }
                break;
        }
    }

    int FeedbackSuppressor::findMatch (float freq, double octaveTolerance) const noexcept
    {
        int best = -1;
        double bestDist = octaveTolerance;
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (s.kind == kindInactive) continue;
            const double d = std::abs (std::log2 ((double) freq / s.freq));
            if (d < bestDist) { bestDist = d; best = i; }
        }
        return best;
    }

    void FeedbackSuppressor::handleDetect (float freq, bool wantFixed) noexcept
    {
        if (settings.freeze)
            return;

        // within ~1/3 of the notch bandwidth (min 1/48 octave) counts as the same ring
        const double bwOct = 2.0 * std::asinh (1.0 / (2.0 * settings.q)) / std::log (2.0);
        const int match = findMatch (freq, std::max (1.0 / 48.0, bwOct * 0.6));

        if (match >= 0)
        {
            auto& s = slots[(size_t) match];
            const bool releasing = s.targetDb > -0.01f;
            s.targetDb = releasing ? std::max (settings.maxDepthDb, std::min (s.currentDb, initialDepthDb))
                                   : std::max (settings.maxDepthDb, std::min (s.targetDb, s.currentDb) + deepenStepDb);
            // nudge the centre toward the newest estimate
            s.freq = (float) std::exp (0.7 * std::log ((double) s.freq) + 0.3 * std::log ((double) freq));
            s.g0 = SvfCoeffs::prewarp (s.freq, fs);
            s.samplesSinceTrigger = 0;

            if (wantFixed && s.kind == kindFloating)
            {
                int fixedCount = 0;
                for (auto& o : slots) fixedCount += o.kind == kindFixed ? 1 : 0;
                if (fixedCount < settings.maxFixed) s.kind = kindFixed;
            }
            return;
        }

        int idx = -1, kind = kindFloating;
        if (wantFixed) { idx = allocate (true); kind = kindFixed; }
        if (idx < 0)   { idx = allocate (false); kind = kindFloating; }
        if (idx >= 0)
            activate (idx, kind, freq, std::max (settings.maxDepthDb, initialDepthDb), false);
    }

    int FeedbackSuppressor::allocate (bool fixed) noexcept
    {
        int fixedCount = 0, floatCount = 0, freeIdx = -1;
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (s.kind == kindFixed) ++fixedCount;
            else if (s.kind == kindFloating && s.targetDb < -0.01f) ++floatCount;
            else if (s.kind == kindInactive && freeIdx < 0) freeIdx = i;
        }

        if (fixed)
            return (fixedCount < settings.maxFixed) ? freeIdx : -1;

        if (settings.maxFloat <= 0)
            return -1;

        if (floatCount < settings.maxFloat && freeIdx >= 0)
            return freeIdx;

        // recycle a floating slot: prefer one already releasing, else the oldest
        int best = -1;
        int64_t bestAge = -1;
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& s = slots[(size_t) i];
            if (s.kind != kindFloating) continue;
            const int64_t age = s.targetDb > -0.01f ? INT64_MAX : s.samplesSinceTrigger;
            if (age > bestAge) { bestAge = age; best = i; }
        }
        if (best >= 0)
            for (auto& st : slots[(size_t) best].state) st.reset();
        return best;
    }

    void FeedbackSuppressor::activate (int idx, int kind, float freq, float depth, bool immediate) noexcept
    {
        auto& s = slots[(size_t) idx];
        if (s.kind == kindInactive)
            for (auto& st : s.state) st.reset();
        s.kind = kind;
        s.currentDb = immediate ? depth : 0.0f;   // new notches fade in (click-free)
        s.freq = freq;
        s.g0 = SvfCoeffs::prewarp (freq, fs);
        s.targetDb = depth;
        s.samplesSinceTrigger = 0;
        s.coeffs.bellFromG (s.g0, settings.q, s.currentDb);
    }

    void FeedbackSuppressor::updateControl() noexcept
    {
        const int64_t holdSamples = (int64_t) (settings.holdSeconds * fs);
        const float ctlFs = (float) (fs / controlInterval);
        const float attack = std::exp (-1.0f / (0.015f * ctlFs));  // notch digs in ~15 ms
        const float release = std::exp (-1.0f / (0.5f * ctlFs));   // and lets go over ~0.5 s

        for (auto& s : slots)
        {
            if (s.kind == kindInactive) continue;

            if (s.kind == kindFloating && ! settings.freeze && s.samplesSinceTrigger > holdSamples)
                s.targetDb = 0.0f;

            // keep within the current depth limit
            s.targetDb = std::max (s.targetDb, settings.maxDepthDb);

            const float c = s.targetDb < s.currentDb ? attack : release;
            s.currentDb = c * s.currentDb + (1.0f - c) * s.targetDb;

            if (s.targetDb > -0.01f && s.currentDb > -0.05f)
            {
                s.kind = kindInactive;
                s.currentDb = 0.0f;
                continue;
            }

            s.coeffs.bellFromG (s.g0, settings.q, s.currentDb);
        }
    }

    void FeedbackSuppressor::publish() noexcept
    {
        for (int i = 0; i < maxSlots; ++i)
        {
            const auto& s = slots[(size_t) i];
            auto& p = published[(size_t) i];
            p.freq.store (s.freq, std::memory_order_relaxed);
            p.depth.store (s.currentDb, std::memory_order_relaxed);
            p.target.store (s.targetDb, std::memory_order_relaxed);
            p.kind.store (s.kind, std::memory_order_release);
        }
    }

    FeedbackSuppressor::NotchInfo FeedbackSuppressor::getNotch (int slot) const noexcept
    {
        const auto& p = published[(size_t) slot];
        NotchInfo n;
        n.kind = p.kind.load (std::memory_order_acquire);
        n.freq = p.freq.load (std::memory_order_relaxed);
        n.depthDb = p.depth.load (std::memory_order_relaxed);
        n.targetDb = p.target.load (std::memory_order_relaxed);
        return n;
    }

    int FeedbackSuppressor::countActive() const noexcept
    {
        int n = 0;
        for (auto& p : published) n += p.kind.load (std::memory_order_relaxed) != kindInactive ? 1 : 0;
        return n;
    }

    float FeedbackSuppressor::responseDb (const NotchInfo* notches, int n, float q, double freq, double sampleRate)
    {
        double total = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (notches[i].kind == kindInactive || notches[i].depthDb > -0.05f) continue;
            SvfCoeffs c;
            c.bell (notches[i].freq, q, notches[i].depthDb, sampleRate);
            total += c.magnitudeDb (freq, sampleRate);
        }
        return (float) total;
    }
}
