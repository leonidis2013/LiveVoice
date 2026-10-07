#pragma once

#include <array>
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Svf.h"
#include "LockFreeQueue.h"
#include "../Params.h"

// Bank of zero-latency notch filters. Notches are created and deepened by
// commands from the detector thread (or the UI) and are either:
//   - fixed:    placed during ring-out (Learn); they stay until cleared and
//               are saved with the session.
//   - floating: placed during the show; they release after the hold time if
//               the frequency stops ringing.
namespace dsp
{
    class FeedbackSuppressor
    {
    public:
        static constexpr int maxSlots = params::maxNotches;
        static constexpr int maxChannels = 2;
        static constexpr int controlInterval = 32;
        static constexpr float initialDepthDb = -6.0f;
        static constexpr float deepenStepDb = -3.0f;

        struct Settings
        {
            bool on = true, freeze = false;
            int maxFixed = 8, maxFloat = 6;
            float maxDepthDb = -18.0f, q = 28.9f, holdSeconds = 20.0f;
        };

        struct Command
        {
            enum class Type { detect, setFixed, clearFloating, clearAll, removeSlot, makeFixed };
            Type type = Type::detect;
            float freq = 0.0f;
            float depthDb = 0.0f;
            bool fixed = false;
            int slot = -1;
        };

        enum SlotKind { kindInactive = 0, kindFloating = 1, kindFixed = 2 };

        struct NotchInfo
        {
            int kind = kindInactive;
            float freq = 0.0f, depthDb = 0.0f, targetDb = 0.0f;
        };

        void prepare (double sampleRate, int numChannels);
        void reset();                         // clears filter state, keeps notches
        void setSettings (const Settings& s) noexcept { settings = s; }

        // Thread-safe producers: one queue for the detector thread, one for the UI/message thread.
        bool pushDetectorCommand (const Command& c) noexcept { return detectorQueue.push (c); }
        bool pushUiCommand (const Command& c) noexcept       { return uiQueue.push (c); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept;

        // Lock-free snapshot readable from any thread.
        NotchInfo getNotch (int slot) const noexcept;
        int countActive() const noexcept;

        // Combined magnitude (dB) of all active notches at freq, for drawing.
        static float responseDb (const NotchInfo* notches, int n, float q, double freq, double sampleRate);

    private:
        struct Slot
        {
            int kind = kindInactive;
            float freq = 1000.0f, targetDb = 0.0f, currentDb = 0.0f;
            int64_t samplesSinceTrigger = 0;
            double g0 = 0.1;
            SvfCoeffs coeffs;
            std::array<SvfState, maxChannels> state;
        };

        struct Published
        {
            std::atomic<int> kind { kindInactive };
            std::atomic<float> freq { 0.0f }, depth { 0.0f }, target { 0.0f };
        };

        void handleCommand (const Command& c) noexcept;
        void handleDetect (float freq, bool wantFixed) noexcept;
        int  findMatch (float freq, double octaveTolerance) const noexcept;
        int  allocate (bool fixed) noexcept;
        void activate (int idx, int kind, float freq, float depth, bool immediate) noexcept;
        void updateControl() noexcept;
        void publish() noexcept;

        Settings settings;
        double fs = 48000.0;
        int channels = 2, counter = 0;
        float lastQ = -1.0f;
        std::array<Slot, maxSlots> slots;
        std::array<Published, maxSlots> published;
        LockFreeQueue<Command, 64> detectorQueue, uiQueue;
    };
}
