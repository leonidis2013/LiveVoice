#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "PresetManager.h"
#include "dsp/DynamicEq.h"
#include "dsp/DeEsser.h"
#include "dsp/SafetyLimiter.h"
#include "dsp/FeedbackSuppressor.h"
#include "dsp/FeedbackDetector.h"

// Lock-free ring the audio thread writes and the UI reads for the RTA.
class ScopeTap
{
public:
    static constexpr int size = 1 << 15;

    void push (const float* data, int n) noexcept
    {
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) { buffer[(size_t) w] = data[i]; w = (w + 1) & (size - 1); }
        writePos.store (w, std::memory_order_release);
    }

    void readLatest (float* dest, int n) const noexcept
    {
        const int w = writePos.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i) dest[i] = buffer[(size_t) ((w - n + i) & (size - 1))];
    }

private:
    std::array<float, size> buffer {};
    std::atomic<int> writePos { 0 };
};

class LiveVoiceProcessor : public juce::AudioProcessor
{
public:
    LiveVoiceProcessor();
    ~LiveVoiceProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    // ---- for the editor ----
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets;
    ScopeTap inputTap, outputTap;

    dsp::FeedbackSuppressor feedback;
    dsp::FeedbackDetector detector { feedback };
    dsp::DynamicEq dynEq;
    dsp::DeEsser deEsser;
    dsp::SafetyLimiter limiter;

    std::atomic<float> inputPeakDb { -100.0f }, outputPeakDb { -100.0f };
    std::atomic<double> currentSampleRate { 48000.0 };

    void sendNotchCommand (const dsp::FeedbackSuppressor::Command& c);   // message thread
    float getNotchQ() const;

    // UI state that should survive closing the editor / reloading the session
    std::atomic<int> selectedBand { 0 };

private:
    void updateSettings();

    struct P
    {
        std::atomic<float>* inGain; std::atomic<float>* outGain; std::atomic<float>* limitOn; std::atomic<float>* ceiling; std::atomic<float>* bypass;
        std::atomic<float>* fbOn; std::atomic<float>* fbMode; std::atomic<float>* fbSens; std::atomic<float>* fbLearn; std::atomic<float>* fbFreeze;
        std::atomic<float>* fbMaxFixed; std::atomic<float>* fbMaxFloat; std::atomic<float>* fbMaxDepth; std::atomic<float>* fbWidth; std::atomic<float>* fbHold;
        std::atomic<float>* fbLo; std::atomic<float>* fbHi;
        std::atomic<float>* eqOn;
        struct Band { std::atomic<float>* on; std::atomic<float>* type; std::atomic<float>* freq; std::atomic<float>* gain; std::atomic<float>* q;
                      std::atomic<float>* dyn; std::atomic<float>* thr; std::atomic<float>* range; std::atomic<float>* ratio;
                      std::atomic<float>* att; std::atomic<float>* rel; };
        std::array<Band, params::numEqBands> bands;
        std::atomic<float>* dsOn; std::atomic<float>* dsFreq; std::atomic<float>* dsThr; std::atomic<float>* dsRange; std::atomic<float>* dsMode; std::atomic<float>* dsRel;
    } p {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inGainSmooth, outGainSmooth;
    std::vector<float> monoScratch;
    juce::SpinLock uiCommandLock;
    int currentProgram = 0;
    bool eqWasOn = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveVoiceProcessor)
};
