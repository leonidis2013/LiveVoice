#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"
#include "PresetManager.h"
#include "dsp/DynamicEq.h"
#include "dsp/DeEsser.h"
#include "dsp/FeedbackSuppressor.h"
#include "dsp/FeedbackDetector.h"
#include "dsp/PrimarySourceExpander.h"
#include "dsp/VocalCompressor.h"
#include "dsp/LevelTone.h"
#include "dsp/HighPass.h"

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
    juce::UndoManager undoManager { 30000, 50 };
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets;
    ScopeTap inputTap, outputTap;

    dsp::HighPass highPass;
    dsp::FeedbackSuppressor feedback;
    dsp::FeedbackDetector detector { feedback };
    dsp::DynamicEq dynEq;
    dsp::DeEsser deEsser;
    dsp::PrimarySourceExpander pse;
    dsp::VocalCompressor comp;
    dsp::ProximityComp proximity;
    dsp::AutoLeveler leveler;
    dsp::Warmth warmthFx;

    // Solo / listen. Never saved or automated, so a session can't come back soloed.
    static constexpr int soloNone = -1, soloDeEss = 100;   // 0..5 = EQ band
    std::atomic<int> soloTarget { soloNone };

    // Monitor output (output channel 2 when the input is mono): carries FOH, the dry input (A/B), or the solo.
    // soloToMain: fallback for hosts without monitor routing - solo replaces the main output.
    std::atomic<bool> monitorDry { false }, soloToMain { false };
    std::atomic<int> deEssListen { 0 };   // de-ess solo plays: 0 = sibilance band, 1 = what's removed

    // Level match: short-term loudness of the plugin's input (before INPUT trim) and output,
    // only measured while someone is talking. outputVsInputDb = output - input.
    std::atomic<float> loudIn { -100.0f }, loudOut { -100.0f };
    float getOutputVsInputDb() const noexcept { return loudOut.load() - loudIn.load(); }
    bool levelMatchValid() const noexcept { return loudIn.load() > -70.0f && loudOut.load() > -70.0f; }
    void matchOutputToInput();   // message thread: sets OUTPUT so output loudness == input
    bool isMonitorBusActive() const;

    // ---- Suggest (voice capture for the de-esser / EQ assistants), message thread ----
    enum class CaptureTarget { none = 0, deEss, eq };
    void startCapture (CaptureTarget t, double seconds);
    void cancelCapture();
    CaptureTarget getCaptureTarget() const noexcept { return (CaptureTarget) captureTarget.load(); }
    float getCaptureProgress() const noexcept;             // 0..1
    bool captureFinished() const noexcept { return captureState.load() == 2; }
    std::vector<float> takeCapture();                       // after captureFinished()

    // ---- feedback log, message thread ----
    struct LogEntry { juce::Time time; float freq; bool fixed; };
    void pollFeedbackLog();
    const std::vector<LogEntry>& getFeedbackLog() const { return feedbackLog; }
    void clearFeedbackLog() { feedbackLog.clear(); }

    // ---- A/B settings compare, message thread ----
    int getActiveSlot() const { return activeSlot; }
    void selectSlot (int slot);
    void copyActiveToOther();

    std::atomic<float> inputPeakDb { -100.0f }, outputPeakDb { -100.0f };
    std::atomic<double> currentSampleRate { 48000.0 };

    void sendNotchCommand (const dsp::FeedbackSuppressor::Command& c);   // message thread
    float getNotchQ() const;

    // UI state that should survive closing the editor / reloading the session
    std::atomic<int> selectedBand { 0 };
    std::atomic<float> uiScale { 0.85f };
    bool uiScaleRestored = false;                         // false until a session gave us a size
    int rtaView = 2, rtaSmoothing = 1, historySpan = 1;   // RTA: 0 in, 1 out, 2 both; smoothing idx; span idx
    bool rtaTilt = false;

private:
    void updateSettings();
    bool applyEqSolo (const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst, int numSamples, int solo);

    struct P
    {
        std::atomic<float>* inGain; std::atomic<float>* outGain; std::atomic<float>* bypass;
        std::atomic<float>* fbOn; std::atomic<float>* fbMode; std::atomic<float>* fbSens; std::atomic<float>* fbLearn; std::atomic<float>* fbFreeze;
        std::atomic<float>* fbMaxFixed; std::atomic<float>* fbMaxFloat; std::atomic<float>* fbMaxDepth; std::atomic<float>* fbWidth; std::atomic<float>* fbHold;
        std::atomic<float>* fbLo; std::atomic<float>* fbHi;
        std::atomic<float>* hpfOn; std::atomic<float>* hpfFreq; std::atomic<float>* hpfSlope;
        std::atomic<float>* eqOn;
        struct Band { std::atomic<float>* on; std::atomic<float>* type; std::atomic<float>* freq; std::atomic<float>* gain; std::atomic<float>* q;
                      std::atomic<float>* dyn; std::atomic<float>* thr; std::atomic<float>* range; std::atomic<float>* ratio;
                      std::atomic<float>* att; std::atomic<float>* rel; };
        std::array<Band, params::numEqBands> bands;
        std::atomic<float>* dsOn; std::atomic<float>* dsFreq; std::atomic<float>* dsThr; std::atomic<float>* dsRange; std::atomic<float>* dsMode; std::atomic<float>* dsRel;
        std::atomic<float>* dsRelative; std::atomic<float>* dsRelThr; std::atomic<float>* dsQ;
        std::atomic<float>* pseOn; std::atomic<float>* pseThr; std::atomic<float>* pseRange; std::atomic<float>* pseAttack;
        std::atomic<float>* pseHold; std::atomic<float>* pseRelease; std::atomic<float>* pseScHp; std::atomic<float>* pseScLp;
        std::atomic<float>* compOn; std::atomic<float>* compThr; std::atomic<float>* compRatio; std::atomic<float>* compKnee;
        std::atomic<float>* compAttack; std::atomic<float>* compRelease; std::atomic<float>* compAuto; std::atomic<float>* compMakeup;
        std::atomic<float>* compMix; std::atomic<float>* compScHp; std::atomic<float>* compAutoGain;
        std::atomic<float>* lvlOn; std::atomic<float>* lvlTarget; std::atomic<float>* lvlRange; std::atomic<float>* lvlSpeed;
        std::atomic<float>* proxOn; std::atomic<float>* proxAmount; std::atomic<float>* warmth;
    } p {};

    dsp::SvfCoeffs soloCoeffs;
    std::array<dsp::SvfState, 2> soloState;
    int lastSoloBand = -1;
    juce::AudioBuffer<float> dryBuf, soloBuf;
    int lastDetectCount = 0;
    double powIn = 0.0, powOut = 0.0;   // gated short-term power for level match
    std::atomic<float> levelAdjustDb { 0.0f };

    // capture
    std::vector<float> captureData;
    std::atomic<int> captureState { 0 }, captureTarget { 0 }, capturePos { 0 };   // state 0 idle, 1 recording, 2 done
    void feedCapture (const juce::AudioBuffer<float>& b, int numSamples, CaptureTarget at) noexcept;

    std::vector<LogEntry> feedbackLog;
    std::array<PresetManager::Values, 2> slots;
    int activeSlot = 0;


    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inGainSmooth, outGainSmooth;
    std::vector<float> monoScratch;
    juce::SpinLock uiCommandLock;
    int currentProgram = 0;
    bool eqWasOn = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveVoiceProcessor)
};
