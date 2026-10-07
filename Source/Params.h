#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// All parameter IDs and the parameter layout live here so the DSP,
// UI and presets agree on one list.
namespace params
{
    constexpr int numEqBands = 6;
    constexpr int maxFixedNotches = 12;
    constexpr int maxFloatNotches = 12;
    constexpr int maxNotches = maxFixedNotches + maxFloatNotches;

    // ---- global ----
    inline const juce::String inGain   { "inGain" };
    inline const juce::String outGain  { "outGain" };
    inline const juce::String limitOn  { "limitOn" };
    inline const juce::String ceiling  { "ceiling" };
    inline const juce::String bypass   { "bypass" };

    // ---- feedback ----
    inline const juce::String fbOn       { "fbOn" };
    inline const juce::String fbMode     { "fbMode" };
    inline const juce::String fbSens     { "fbSens" };
    inline const juce::String fbLearn    { "fbLearn" };
    inline const juce::String fbFreeze   { "fbFreeze" };
    inline const juce::String fbMaxFixed { "fbMaxFixed" };
    inline const juce::String fbMaxFloat { "fbMaxFloat" };
    inline const juce::String fbMaxDepth { "fbMaxDepth" };
    inline const juce::String fbWidth    { "fbWidth" };
    inline const juce::String fbHold     { "fbHold" };
    inline const juce::String fbLoFreq   { "fbLoFreq" };
    inline const juce::String fbHiFreq   { "fbHiFreq" };

    enum class FbMode { speech = 0, singing, multipurpose };

    // ---- dynamic EQ ----
    inline const juce::String eqOn { "eqOn" };

    inline juce::String band (int b, const char* suffix)  { return "b" + juce::String (b + 1) + "_" + suffix; }
    inline juce::String bOn    (int b) { return band (b, "on"); }
    inline juce::String bType  (int b) { return band (b, "type"); }   // bands 1 and 6 only
    inline juce::String bFreq  (int b) { return band (b, "freq"); }
    inline juce::String bGain  (int b) { return band (b, "gain"); }
    inline juce::String bQ     (int b) { return band (b, "q"); }
    inline juce::String bDyn   (int b) { return band (b, "dyn"); }
    inline juce::String bThr   (int b) { return band (b, "thr"); }
    inline juce::String bRange (int b) { return band (b, "range"); }
    inline juce::String bRatio (int b) { return band (b, "ratio"); }
    inline juce::String bAtt   (int b) { return band (b, "att"); }
    inline juce::String bRel   (int b) { return band (b, "rel"); }

    // Band types. Band 1 offers {Bell, LowShelf, HP12, HP24}; band 6 {Bell, HighShelf, LP12, LP24}.
    enum class BandType { bell = 0, lowShelf, highShelf, highPass12, highPass24, lowPass12, lowPass24 };

    inline BandType bandTypeFromChoice (int band, int choice)
    {
        if (band == 0)
        {
            const BandType t[] { BandType::bell, BandType::lowShelf, BandType::highPass12, BandType::highPass24 };
            return t[juce::jlimit (0, 3, choice)];
        }
        if (band == numEqBands - 1)
        {
            const BandType t[] { BandType::bell, BandType::highShelf, BandType::lowPass12, BandType::lowPass24 };
            return t[juce::jlimit (0, 3, choice)];
        }
        return BandType::bell;
    }

    inline bool isPassType (BandType t)
    {
        return t == BandType::highPass12 || t == BandType::highPass24 || t == BandType::lowPass12 || t == BandType::lowPass24;
    }

    inline constexpr float defaultBandFreq[numEqBands] { 80.0f, 220.0f, 500.0f, 1500.0f, 4000.0f, 10000.0f };

    // ---- de-esser ----
    inline const juce::String dsOn    { "dsOn" };
    inline const juce::String dsFreq  { "dsFreq" };
    inline const juce::String dsThr   { "dsThr" };
    inline const juce::String dsRange { "dsRange" };
    inline const juce::String dsMode  { "dsMode" };
    inline const juce::String dsRel   { "dsRel" };

    enum class DsMode { split = 0, wideband };

    // -----------------------------------------------------------------------
    inline juce::NormalisableRange<float> freqRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));   // log-like feel
        return r;
    }

    inline juce::NormalisableRange<float> timeRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        auto db  = AudioParameterFloatAttributes().withLabel ("dB")
                     .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });
        auto hz  = AudioParameterFloatAttributes().withLabel ("Hz")
                     .withStringFromValueFunction ([] (float v, int)
                     {
                         return v >= 1000.0f ? String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                             : String (roundToInt (v)) + " Hz";
                     });
        auto ms  = AudioParameterFloatAttributes().withLabel ("ms")
                     .withStringFromValueFunction ([] (float v, int) { return v < 10.0f ? String (v, 1) + " ms" : String (roundToInt (v)) + " ms"; });
        auto sec = AudioParameterFloatAttributes().withLabel ("s")
                     .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " s"; });
        auto pct = AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + "%"; });
        auto qa  = AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([] (float v, int) { return "Q " + String (v, 2); });
        auto rat = AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + ":1"; });

        auto addF = [&] (const String& id, const String& name, NormalisableRange<float> r, float def,
                         AudioParameterFloatAttributes a)
        {
            layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, a));
        };
        auto addB = [&] (const String& id, const String& name, bool def)
        {
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
        };
        auto addC = [&] (const String& id, const String& name, StringArray choices, int def)
        {
            layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
        };
        auto addI = [&] (const String& id, const String& name, int lo, int hi, int def)
        {
            layout.add (std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name, lo, hi, def));
        };

        // Global
        addF (inGain,  "Input Gain",  { -24.0f, 24.0f, 0.1f }, 0.0f, db);
        addF (outGain, "Output Gain", { -24.0f, 24.0f, 0.1f }, 0.0f, db);
        addB (limitOn, "Safety Limiter", true);
        addF (ceiling, "Limiter Ceiling", { -12.0f, 0.0f, 0.1f }, -1.0f, db);
        addB (bypass,  "Bypass", false);

        // Feedback
        addB (fbOn,       "Feedback On", true);
        addC (fbMode,     "Feedback Mode", { "Speech", "Singing", "Multipurpose" }, 0);
        addF (fbSens,     "Feedback Sensitivity", { 0.0f, 100.0f, 1.0f }, 50.0f, pct);
        addB (fbLearn,    "Ring-Out Learn", false);
        addB (fbFreeze,   "Freeze Notches", false);
        addI (fbMaxFixed, "Max Fixed Notches", 0, maxFixedNotches, 8);
        addI (fbMaxFloat, "Max Floating Notches", 0, maxFloatNotches, 6);
        addF (fbMaxDepth, "Max Notch Depth", { -30.0f, -3.0f, 0.5f }, -18.0f, db);
        addC (fbWidth,    "Notch Width", { "Wide 1/5 oct", "Normal 1/10 oct", "Narrow 1/20 oct", "Ultra 1/40 oct" }, 2);
        addF (fbHold,     "Floating Hold", timeRange (2.0f, 120.0f, 20.0f), 20.0f, sec);
        addF (fbLoFreq,   "Detect Low Limit", freqRange (60.0f, 2000.0f), 150.0f, hz);
        addF (fbHiFreq,   "Detect High Limit", freqRange (2000.0f, 16000.0f), 10000.0f, hz);

        // Dynamic EQ
        addB (eqOn, "Dynamic EQ On", true);
        for (int b = 0; b < numEqBands; ++b)
        {
            const String n = "Band " + String (b + 1) + " ";
            addB (bOn (b), n + "On", b == 0);
            if (b == 0)                addC (bType (b), n + "Type", { "Bell", "Low Shelf", "High Pass 12", "High Pass 24" }, 2);
            if (b == numEqBands - 1)   addC (bType (b), n + "Type", { "Bell", "High Shelf", "Low Pass 12", "Low Pass 24" }, 1);
            addF (bFreq (b),  n + "Freq",      freqRange (20.0f, 20000.0f), defaultBandFreq[b], hz);
            addF (bGain (b),  n + "Gain",      { -18.0f, 18.0f, 0.1f }, 0.0f, db);
            addF (bQ (b),     n + "Q",         timeRange (0.3f, 10.0f, 1.5f), 0.9f, qa);
            addB (bDyn (b),   n + "Dynamic", false);
            addF (bThr (b),   n + "Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f, db);
            addF (bRange (b), n + "Range",     { -18.0f, 18.0f, 0.1f }, -6.0f, db);
            addF (bRatio (b), n + "Ratio",     timeRange (1.0f, 10.0f, 3.0f), 3.0f, rat);
            addF (bAtt (b),   n + "Attack",    timeRange (0.5f, 200.0f, 15.0f), 5.0f, ms);
            addF (bRel (b),   n + "Release",   timeRange (10.0f, 2000.0f, 200.0f), 120.0f, ms);
        }

        // De-esser
        addB (dsOn,    "De-Esser On", true);
        addF (dsFreq,  "De-Ess Freq", freqRange (2000.0f, 12000.0f), 6000.0f, hz);
        addF (dsThr,   "De-Ess Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f, db);
        addF (dsRange, "De-Ess Range", { 0.0f, 24.0f, 0.1f }, 8.0f, db);
        addC (dsMode,  "De-Ess Mode", { "Split", "Wideband" }, 0);
        addF (dsRel,   "De-Ess Release", timeRange (20.0f, 500.0f, 80.0f), 80.0f, ms);

        juce::ignoreUnused (pct);
        return layout;
    }
}
