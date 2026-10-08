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

    // Band types. Band 1 offers Bell, Low Shelf and high-pass 6..48 dB/oct;
    // band 6 offers Bell, High Shelf and low-pass 6..48 dB/oct.
    enum class BandType { bell = 0, lowShelf, highShelf, highPass, lowPass };

    inline const juce::StringArray band1Choices { "Bell", "Low Shelf", "HP 6 dB/oct", "HP 12 dB/oct", "HP 18 dB/oct",
                                                  "HP 24 dB/oct", "HP 36 dB/oct", "HP 48 dB/oct" };
    inline const juce::StringArray band6Choices { "Bell", "High Shelf", "LP 6 dB/oct", "LP 12 dB/oct", "LP 18 dB/oct",
                                                  "LP 24 dB/oct", "LP 36 dB/oct", "LP 48 dB/oct" };
    inline constexpr int slopeDbPerOct[] { 6, 12, 18, 24, 36, 48 };

    struct BandShape { BandType type = BandType::bell; int slope = 12; };   // slope only for pass types

    inline BandShape bandShapeFromChoice (int band, int choice)
    {
        const bool first = band == 0, last = band == numEqBands - 1;
        if (! first && ! last) return {};
        choice = juce::jlimit (0, 7, choice);
        if (choice == 0) return {};
        if (choice == 1) return { first ? BandType::lowShelf : BandType::highShelf, 12 };
        return { first ? BandType::highPass : BandType::lowPass, slopeDbPerOct[choice - 2] };
    }

    inline BandType bandTypeFromChoice (int band, int choice) { return bandShapeFromChoice (band, choice).type; }

    inline bool isPassType (BandType t) { return t == BandType::highPass || t == BandType::lowPass; }

    // Defaults sit on the classic trouble/tone spots of the human voice:
    // 80 Hz rumble (HPF), 200 Hz boom/proximity, 450 Hz boxiness, 1 kHz nasal honk,
    // 3 kHz presence/harshness, 10 kHz air (high shelf). Sibilance (5-8 kHz) is the de-esser's job.
    inline constexpr float defaultBandFreq[numEqBands] { 80.0f, 200.0f, 450.0f, 1000.0f, 3000.0f, 10000.0f };
    inline constexpr float defaultBandQ[numEqBands]    { 0.71f, 1.0f, 1.4f, 1.4f, 1.2f, 0.71f };
    inline const char* const bandRoleNames[numEqBands] { "Rumble (HPF)", "Boom / proximity", "Boxiness", "Nasal / honk",
                                                         "Presence / harshness", "Air" };

    // ---- de-esser ----
    inline const juce::String dsOn    { "dsOn" };
    inline const juce::String dsFreq  { "dsFreq" };
    inline const juce::String dsThr   { "dsThr" };
    inline const juce::String dsRange { "dsRange" };
    inline const juce::String dsMode  { "dsMode" };
    inline const juce::String dsRel   { "dsRel" };

    inline const juce::String dsRelative { "dsRelative" };    // relative detection on/off
    inline const juce::String dsRelThr { "dsRelThr" };     // threshold used in relative mode
    inline const juce::String dsQ      { "dsQ" };          // width of the sibilance band

    // Band: dynamic bell on just the sibilance band (narrow). Shelf: dynamic high shelf (all highs).
    // Wideband: whole signal ducks.
    enum class DsMode { band = 0, shelf, wideband };

    // ---- PSE (primary source expander) ----
    inline const juce::String pseOn      { "pseOn" };
    inline const juce::String pseThr     { "pseThr" };
    inline const juce::String pseRange   { "pseRange" };
    inline const juce::String pseAttack  { "pseAttack" };
    inline const juce::String pseHold    { "pseHold" };
    inline const juce::String pseRelease { "pseRelease" };
    inline const juce::String pseScHp    { "pseScHp" };
    inline const juce::String pseScLp    { "pseScLp" };

    // ---- vocal compressor ----
    inline const juce::String compOn      { "compOn" };
    inline const juce::String compThr     { "compThr" };
    inline const juce::String compRatio   { "compRatio" };
    inline const juce::String compKnee    { "compKnee" };
    inline const juce::String compAttack  { "compAttack" };
    inline const juce::String compRelease { "compRelease" };
    inline const juce::String compAuto    { "compAutoRel" };
    inline const juce::String compMakeup  { "compMakeup" };
    inline const juce::String compMix     { "compMix" };
    inline const juce::String compScHp    { "compScHp" };

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
            addB (bOn (b), n + "On", true);   // all on at 0 dB = transparent, handles ready to use
            if (b == 0)                addC (bType (b), n + "Type", band1Choices, 5);   // HP 24 dB/oct
            if (b == numEqBands - 1)   addC (bType (b), n + "Type", band6Choices, 1);   // high shelf
            addF (bFreq (b),  n + "Freq",      freqRange (20.0f, 20000.0f), defaultBandFreq[b], hz);
            addF (bGain (b),  n + "Gain",      { -18.0f, 18.0f, 0.1f }, 0.0f, db);
            addF (bQ (b),     n + "Q",         timeRange (0.3f, 10.0f, 1.5f), defaultBandQ[b], qa);
            addB (bDyn (b),   n + "Dynamic", false);
            addF (bThr (b),   n + "Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f, db);
            addF (bRange (b), n + "Range",     { -18.0f, 18.0f, 0.1f }, -6.0f, db);
            addF (bRatio (b), n + "Ratio",     timeRange (1.0f, 10.0f, 3.0f), 2.0f, rat);
            addF (bAtt (b),   n + "Attack",    timeRange (0.5f, 200.0f, 15.0f), 10.0f, ms);
            addF (bRel (b),   n + "Release",   timeRange (10.0f, 2000.0f, 200.0f), 120.0f, ms);
        }

        // De-esser
        addB (dsOn,    "De-Esser On", true);
        addF (dsFreq,  "De-Ess Freq", freqRange (2000.0f, 12000.0f), 6000.0f, hz);
        addF (dsThr,   "De-Ess Threshold", { -60.0f, 0.0f, 0.1f }, -30.0f, db);
        addF (dsRange, "De-Ess Range", { 0.0f, 24.0f, 0.1f }, 8.0f, db);
        addC (dsMode,  "De-Ess Mode", { "Band (narrow)", "Shelf (all highs)", "Wideband" }, 0);
        addF (dsQ,     "De-Ess Width", timeRange (1.0f, 8.0f, 3.0f), 3.0f, qa);
        addF (dsRel,   "De-Ess Release", timeRange (20.0f, 500.0f, 80.0f), 80.0f, ms);
        addB (dsRelative,  "De-Ess Relative Detection", false);
        addF (dsRelThr, "De-Ess Relative Threshold", { -40.0f, 6.0f, 0.1f }, -18.0f, db);

        // PSE
        addB (pseOn,      "PSE On", false);
        addF (pseThr,     "PSE Threshold", { -80.0f, 0.0f, 0.1f }, -45.0f, db);
        addF (pseRange,   "PSE Range", { 0.0f, 40.0f, 0.1f }, 12.0f, db);
        addF (pseAttack,  "PSE Attack", timeRange (0.1f, 20.0f, 2.0f), 1.0f, ms);
        addF (pseHold,    "PSE Hold", timeRange (0.0f, 500.0f, 100.0f), 80.0f, ms);
        addF (pseRelease, "PSE Release", timeRange (20.0f, 2000.0f, 300.0f), 300.0f, ms);
        addF (pseScHp,    "PSE Sidechain HP", freqRange (20.0f, 1000.0f), 150.0f, hz);
        addF (pseScLp,    "PSE Sidechain LP", freqRange (1000.0f, 20000.0f), 8000.0f, hz);

        // Vocal compressor
        addB (compOn,      "Comp On", false);
        addF (compThr,     "Comp Threshold", { -50.0f, 0.0f, 0.1f }, -20.0f, db);
        addF (compRatio,   "Comp Ratio", timeRange (1.0f, 10.0f, 3.0f), 3.0f, rat);
        addF (compKnee,    "Comp Knee", { 0.0f, 18.0f, 0.1f }, 10.0f, db);
        addF (compAttack,  "Comp Attack", timeRange (0.3f, 50.0f, 8.0f), 8.0f, ms);
        addF (compRelease, "Comp Release", timeRange (30.0f, 1500.0f, 250.0f), 250.0f, ms);
        addB (compAuto,    "Comp Auto Release", true);
        addF (compMakeup,  "Comp Makeup", { 0.0f, 24.0f, 0.1f }, 0.0f, db);
        addF (compMix,     "Comp Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, pct);
        addF (compScHp,    "Comp Sidechain HP", freqRange (20.0f, 300.0f), 100.0f, hz);

        return layout;
    }
}
