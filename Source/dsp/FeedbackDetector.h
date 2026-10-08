#pragma once

#include <atomic>
#include <vector>
#include <juce_dsp/juce_dsp.h>
#include "FeedbackSuppressor.h"

// Background-thread feedback detector. The audio thread only copies samples
// into a FIFO; all FFT analysis happens here, so detection adds no latency
// to the audio path.
//
// A spectral peak is treated as feedback when, for long enough, it is:
//   - far above the overall spectrum           (peak-to-average, PAPR)
//   - much sharper than its neighbourhood      (peak-to-neighbour, PNPR)
//   - not part of a harmonic series            (peak-to-harmonic, PHPR) — voices
//     and sung notes have harmonics, a feedback ring is a near-pure sine
//   - rock-steady in pitch                     (voices drift and wobble; a ring doesn't)
//   - steady or growing in level               (persistence + trend)
namespace dsp
{
    class FeedbackDetector : private juce::Thread
    {
    public:
        struct Settings
        {
            bool enabled = true, learn = false, freeze = false;
            int mode = 0;                 // params::FbMode
            float sensitivity = 50.0f;    // 0..100
            float loFreq = 150.0f, hiFreq = 10000.0f;
        };

        explicit FeedbackDetector (FeedbackSuppressor& target);
        ~FeedbackDetector() override;

        void prepare (double sampleRate);
        void release();

        // audio thread
        void pushSamples (const float* mono, int numSamples) noexcept;

        // any thread
        void setSettings (const Settings& s) noexcept;

        // UI feedback: frequency of the most recent detection and a counter that increments on each one
        float getLastDetectionFreq() const noexcept { return lastDetectFreq.load(); }
        int getDetectionCount() const noexcept { return detectCount.load(); }

        // Feedback log: one event per detection (detector thread -> message thread).
        struct LogEvent { float freq = 0.0f; bool fixed = false; };
        bool popLogEvent (LogEvent& e) noexcept { return logQueue.pop (e); }

        // For offline tests: run analysis synchronously instead of on the thread.
        void processPendingForTest();
        void stopThreadForTest() { stopThread (2000); }

    private:
        void run() override;
        bool analyseIfReady();
        void analyseFrame();

        struct Track
        {
            float freq = 0.0f;
            int hits = 0, misses = 0, cooldown = 0;
            float firstLevel = 0.0f, lastLevel = 0.0f;
            float minFreq = 0.0f, maxFreq = 0.0f;
            bool seen = false;
        };

        FeedbackSuppressor& suppressor;
        double fs = 48000.0;
        int fftOrder = 13, fftSize = 8192, hop = 2048;

        juce::AbstractFifo fifo { 1 << 16 };
        std::vector<float> fifoData;

        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, history, fftData, magDb;
        int historyWrite = 0, samplesSinceFrame = 0;
        float windowNorm = 1.0f;

        std::vector<Track> tracks;

        std::atomic<bool> sEnabled { true }, sLearn { false }, sFreeze { false };
        std::atomic<int> sMode { 0 };
        std::atomic<float> sSens { 50.0f }, sLo { 150.0f }, sHi { 10000.0f };

        LockFreeQueue<LogEvent, 256> logQueue;
        std::atomic<float> lastDetectFreq { 0.0f };
        std::atomic<int> detectCount { 0 };
        std::atomic<bool> prepared { false };
    };
}
