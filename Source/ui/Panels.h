// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "Theme.h"
#include "../dsp/VoiceAnalysis.h"

using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

// ---------------------------------------------------------------- small widgets
class Knob : public juce::Component
{
public:
    Knob (const juce::String& name, juce::Colour accent);
    void attach (juce::AudioProcessorValueTreeState& state, const juce::String& paramID);
    void resized() override;

    juce::Slider slider;
    juce::Label label;

private:
    std::unique_ptr<SliderAttachment> attachment;
};

struct ToggleBtn : public juce::TextButton
{
    ToggleBtn (const juce::String& text, juce::Colour onColour);
    void attach (juce::AudioProcessorValueTreeState& state, const juce::String& paramID);
    std::unique_ptr<ButtonAttachment> attachment;
};

struct Combo : public juce::ComboBox
{
    void attach (juce::AudioProcessorValueTreeState& state, const juce::String& paramID, const juce::StringArray& items);
    std::unique_ptr<ComboAttachment> attachment;
};

// Gain-reduction meter with a dB scale. Vertical meters hang from the top with
// tick labels; horizontal ones fill from the left and print the value.
class GainReductionMeter : public juce::Component
{
public:
    GainReductionMeter (const juce::Colour& c, float maxDb) : colour (&c), maxReduction (maxDb) {}
    void setValue (float grDb) { if (std::abs (grDb - value) > 0.05f) { value = grDb; repaint(); } }
    void paint (juce::Graphics&) override;
private:
    const juce::Colour* colour;
    float maxReduction, value = 0.0f;
};

// Vertical input/output peak meters beside the RTA, with a colour-zoned scale
// that shows the target range for healthy gain staging.
class IoMeters : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit IoMeters (LiveVoiceProcessor& p);
    void update();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;   // click resets peak hold

    static constexpr float minDb = -60.0f, targetLo = -18.0f, targetHi = -6.0f, warnDb = -3.0f;

private:
    struct Channel { float level = -100.0f, peak = -100.0f; int hold = 0; };
    void ballistics (Channel& c, float v);
    LiveVoiceProcessor& proc;
    Channel in, out;
};

// Base for the bottom panels: rounded card with a title, accent and power button.
class ModulePanel : public juce::Component
{
public:
    ModulePanel (const juce::String& title, const juce::Colour& accent);
    void paint (juce::Graphics&) override;
    juce::Rectangle<int> getContentBounds() const;
    void layoutHeader();

protected:
    juce::String title;
    const juce::Colour& accent;
    ToggleBtn power { "ON", accent };
};

// ---------------------------------------------------------------- history strips
// Scrolling history over 10 / 25 / 60 s (click the span label to change it).
//   De-ess:     sibilance level bars, pink reduction hanging from the top, threshold.
//   Compressor: input level (grey), compressed level (amber), makeup stacked on top (teal).
class HistoryStrip : public juce::Component
{
public:
    explicit HistoryStrip (LiveVoiceProcessor& p) : proc (p) { setOpaque (true); }
    void update();
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::function<void()> onSpanChanged;

    static constexpr int spanSeconds[] { 10, 25, 60 };

protected:
    struct Config
    {
        juce::String title, thresholdText;
        const juce::Colour* accent = nullptr;
        bool on = true, stacked = false;
        float lo = -60.0f, hi = 0.0f, threshold = -30.0f, grMax = 12.0f, makeup = 0.0f;
    };
    virtual Config getConfig() = 0;
    virtual void readPoints (dsp::LevelHistory::Point* dest, int count) = 0;

    LiveVoiceProcessor& proc;

private:
    std::vector<dsp::LevelHistory::Point> rawCols, points;
    juce::Rectangle<float> spanArea;
};

class DeEssHistory : public HistoryStrip
{
public:
    using HistoryStrip::HistoryStrip;
protected:
    Config getConfig() override;
    void readPoints (dsp::LevelHistory::Point* d, int n) override { proc.deEsser.readHistory (d, n); }
};

class CompHistory : public HistoryStrip
{
public:
    using HistoryStrip::HistoryStrip;
protected:
    Config getConfig() override;
    void readPoints (dsp::LevelHistory::Point* d, int n) override { proc.comp.readHistory (d, n); }
};

// Compressor level meter: input (grey) beside compressed level (amber) with the
// makeup gain stacked on top in teal, on one dB scale.
class CompLevelMeter : public juce::Component
{
public:
    explicit CompLevelMeter (LiveVoiceProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    float in = -100.0f, gr = 0.0f, makeup = 0.0f;
};

// Compressor transfer curve with a live dot for the current input level.
class CompGraph : public juce::Component
{
public:
    explicit CompGraph (LiveVoiceProcessor& p) : proc (p) { setOpaque (true); }
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    float inDb = -100.0f, grDb = 0.0f;
};

// PSE key level vs threshold, with an open/closed lamp.
class PseKeyMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PseKeyMeter (LiveVoiceProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    float key = -100.0f, paintedKey = 1000.0f, paintedGr = 1000.0f;
    int paintedState = -1;
};

// Dynamic EQ activity for the selected band: detector level vs threshold, and
// the dynamic gain it produces right now.
class DynActivity : public juce::Component
{
public:
    explicit DynActivity (LiveVoiceProcessor& p) : proc (p) {}
    void setBand (int b) { band = b; paintedLevel = 1000.0f; repaint(); }
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    int band = 0;
    float level = -100.0f, gain = 0.0f, paintedLevel = 1000.0f, paintedGain = 1000.0f;
};

// Result of a Suggest: what was set, with KEEP and REVERT right next to it.
// REVERT puts back exactly the values that were there before the suggestion.
class SuggestionBar : public juce::Component
{
public:
    SuggestionBar (LiveVoiceProcessor& p, const juce::Colour& accent);
    void show (const juce::String& text, std::vector<std::pair<juce::String, float>> before);
    void showMessage (const juce::String& text);   // e.g. "not enough voice heard"
    void clear();
    void resized() override;
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    const juce::Colour& accent;
    juce::String message;
    std::vector<std::pair<juce::String, float>> previous;
    juce::TextButton keep { "KEEP" }, revert { "REVERT" };
};

// ---------------------------------------------------------------- panels
class FeedbackPanel : public ModulePanel
{
public:
    explicit FeedbackPanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();

private:
    LiveVoiceProcessor& proc;
    Combo mode;
    ToggleBtn learn { "RING-OUT LEARN", theme::feedback }, freeze { "FREEZE", theme::warn };
    juce::TextButton clearFloat { "CLEAR FLOATING" }, clearAll { "CLEAR ALL" }, logBtn { "LOG" };
    void showLog();
    Knob sens { "Sensitivity", theme::feedback }, depth { "Max Depth", theme::feedback }, hold { "Float Hold", theme::feedback };
    juce::Label modeLabel, status;
};

class EqPanel : public ModulePanel
{
public:
    explicit EqPanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();
    void selectBand (int b);
    std::function<void (int)> onBandSelected;

private:
    LiveVoiceProcessor& proc;
    int band = 0;
    std::array<juce::TextButton, params::numEqBands> bandButtons;
    ToggleBtn bandOn { "BAND ON", theme::eq }, dynamic { "DYNAMIC", theme::eq }, solo { "SOLO", theme::warn };
    Combo type;
    Knob freq { "Freq", theme::eq }, gain { "Gain", theme::eq }, q { "Q", theme::eq },
         thr { "Threshold", theme::eq }, range { "Range", theme::eq }, ratio { "Ratio", theme::eq },
         attack { "Attack", theme::eq }, release { "Release", theme::eq };
    DynActivity activity { proc };
    juce::TextButton suggest { "SUGGEST" };
    SuggestionBar suggestion { proc, theme::eq };
    void runSuggestion();
};

class DeEssPanel : public ModulePanel
{
public:
    explicit DeEssPanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();

private:
    LiveVoiceProcessor& proc;
    void bindThreshold();
    Combo mode;
    ToggleBtn relative { "RELATIVE", theme::deess }, solo { "SOLO", theme::warn };
    Knob freq { "Freq", theme::deess }, width { "Width (Q)", theme::deess }, thr { "Threshold", theme::deess },
         range { "Range", theme::deess }, rel { "Release", theme::deess };
    GainReductionMeter grMeter { theme::deess, 24.0f };
    juce::Label grLabel, grValue;
    SuggestionBar suggestion { proc, theme::deess };
    juce::TextButton suggest { "SUGGEST" };
    int boundRelative = -1;
    void runSuggestion();
};

class PsePanel : public ModulePanel
{
public:
    explicit PsePanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    // high-pass (the panel's power button) - first in the chain
    Knob hpfFreq { "Freq", theme::hpf };
    Combo hpfSlope;
    juce::Label slopeLabel;
    // PSE
    ToggleBtn pseOn { "ON", theme::pse };
    PseKeyMeter keyMeter { proc };
    Knob thr { "Threshold", theme::pse }, range { "Range", theme::pse }, attack { "Attack", theme::pse },
         hold { "Hold", theme::pse }, release { "Release", theme::pse };
    int pseY = 0;
};

// Leveler gain (bipolar, centre = 0 dB) with the number beside it.
class LevelerMeter : public juce::Component
{
public:
    explicit LevelerMeter (LiveVoiceProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    float gain = 0.0f, painted = 1000.0f;
    bool on = false, holding = false;
};

// Leveler (power button) plus TONE: proximity compensation and warmth.
class LevelTonePanel : public ModulePanel
{
public:
    explicit LevelTonePanel (LiveVoiceProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void update();
private:
    LiveVoiceProcessor& proc;
    LevelerMeter meter { proc };
    Knob target { "Target", theme::output }, range { "Range", theme::output };
    Combo speed;
    juce::Label speedLabel, proxValue;
    ToggleBtn proximity { "PROXIMITY", theme::output };
    Knob proxAmount { "Max Cut", theme::output }, warmth { "Warmth", theme::output };
    int toneY = 0;
};

class CompPanel : public ModulePanel
{
public:
    explicit CompPanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();
private:
    LiveVoiceProcessor& proc;
    ToggleBtn autoRel { "AUTO RELEASE", theme::comp }, autoGain { "AUTO GAIN", theme::comp };
    Knob thr { "Threshold", theme::comp }, ratio { "Ratio", theme::comp }, knee { "Knee", theme::comp },
         attack { "Attack", theme::comp }, release { "Release", theme::comp }, makeup { "Makeup", theme::comp },
         mix { "Mix", theme::comp };
    CompLevelMeter levelMeter { proc };
};

// Shows whether the output is as loud as the input (both measured while someone talks).
// Click it to set OUTPUT so they match.
class LevelMatch : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LevelMatch (LiveVoiceProcessor& p);
    void update();
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    LiveVoiceProcessor& proc;
    float shown = 1000.0f;
    bool valid = false;
};

class HeaderBar : public juce::Component
{
public:
    explicit HeaderBar (LiveVoiceProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void update();
    void refreshPresetList();
    std::function<void()> onOpenSettings;

private:
    void presetChosen();
    void savePreset();
    void deletePreset();

    LiveVoiceProcessor& proc;
    juce::ComboBox presetBox;
    juce::TextButton saveBtn { "SAVE" }, deleteBtn { "DELETE" };
    juce::TextButton undoBtn { "UNDO" }, redoBtn { "REDO" }, slotA { "A" }, slotB { "B" }, copyBtn { "A>B" };
    juce::TextButton settingsBtn { "SETTINGS" }, monitorBtn { "MON A/B" };
    Knob inGain { "INPUT", theme::output }, outGain { "OUTPUT", theme::output };
    LevelMatch levelMatch { proc };
    ToggleBtn bypass { "BYPASS", theme::warn };
    juce::StringArray userNames;
    std::unique_ptr<juce::AlertWindow> dialog;
    bool updatingList = false;
};

