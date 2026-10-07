#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "Theme.h"

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

class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter (std::atomic<float>& source) : src (source) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    std::atomic<float>& src;
    float level = -100.0f, peakHold = -100.0f;
    int holdFrames = 0;
};

class GainReductionMeter : public juce::Component
{
public:
    GainReductionMeter (juce::Colour c, float maxDb) : colour (c), maxReduction (maxDb) {}
    void setValue (float grDb) { if (std::abs (grDb - value) > 0.05f) { value = grDb; repaint(); } }
    void paint (juce::Graphics&) override;
private:
    juce::Colour colour;
    float maxReduction, value = 0.0f;
};

// Base for the bottom panels: rounded card with a title, accent and power button.
class ModulePanel : public juce::Component
{
public:
    ModulePanel (const juce::String& title, juce::Colour accent);
    void paint (juce::Graphics&) override;
    juce::Rectangle<int> getContentBounds() const;
    void layoutHeader();

protected:
    juce::String title;
    juce::Colour accent;
    ToggleBtn power { "ON", accent };
};

// ---------------------------------------------------------------- de-esser histogram
class DeEssHistogram : public juce::Component
{
public:
    explicit DeEssHistogram (LiveVoiceProcessor& p) : proc (p) { setOpaque (true); }
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    std::vector<dsp::DeEsser::HistoryPoint> points;
};

// Big readout of gain-before-feedback gained in the ring-out.
class GbfTile : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit GbfTile (LiveVoiceProcessor& p) : proc (p) {}
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    float shown = -1000.0f;
    int state = -1;
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
    float key = -100.0f;
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
    Combo mode, width;
    ToggleBtn learn { "RING-OUT LEARN", theme::feedback }, freeze { "FREEZE", theme::warn };
    juce::TextButton clearFloat { "CLEAR FLOATING" }, clearAll { "CLEAR ALL" };
    Knob sens { "Sensitivity", theme::feedback }, depth { "Max Depth", theme::feedback }, hold { "Float Hold", theme::feedback },
         maxFixed { "Fixed Max", theme::feedback }, maxFloat { "Floating Max", theme::feedback },
         lo { "Low Limit", theme::feedback }, hi { "High Limit", theme::feedback };
    juce::Label modeLabel, widthLabel, status;
    GbfTile gbfTile { proc };
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
    ToggleBtn bandOn { "BAND ON", theme::eq }, dynamic { "DYNAMIC", theme::eq };
    Combo type;
    Knob freq { "Freq", theme::eq }, gain { "Gain", theme::eq }, q { "Q", theme::eq },
         thr { "Threshold", theme::eq }, range { "Range", theme::eq }, ratio { "Ratio", theme::eq },
         attack { "Attack", theme::eq }, release { "Release", theme::eq };
    juce::Label dynLabel;
    GainReductionMeter dynMeter { theme::eq, 18.0f };
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
    ToggleBtn relative { "RELATIVE", theme::deess };
    Knob freq { "Freq", theme::deess }, thr { "Threshold", theme::deess }, range { "Range", theme::deess }, rel { "Release", theme::deess };
    GainReductionMeter grMeter { theme::deess, 24.0f };
    juce::Label grLabel;
    int boundRelative = -1;
};

class PsePanel : public ModulePanel
{
public:
    explicit PsePanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();
private:
    LiveVoiceProcessor& proc;
    PseKeyMeter keyMeter { proc };
    Knob thr { "Threshold", theme::pse }, range { "Range", theme::pse }, attack { "Attack", theme::pse },
         hold { "Hold", theme::pse }, release { "Release", theme::pse },
         scHp { "Key HP", theme::pse }, scLp { "Key LP", theme::pse };
};

class CompPanel : public ModulePanel
{
public:
    explicit CompPanel (LiveVoiceProcessor& p);
    void resized() override;
    void update();
private:
    LiveVoiceProcessor& proc;
    ToggleBtn autoRel { "AUTO RELEASE", theme::comp };
    Knob thr { "Threshold", theme::comp }, ratio { "Ratio", theme::comp }, knee { "Knee", theme::comp },
         attack { "Attack", theme::comp }, release { "Release", theme::comp }, makeup { "Makeup", theme::comp },
         mix { "Mix", theme::comp }, scHp { "Key HP", theme::comp };
    GainReductionMeter grMeter { theme::comp, 18.0f };
    juce::Label grLabel, grValue;
};

class HeaderBar : public juce::Component
{
public:
    explicit HeaderBar (LiveVoiceProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void update();
    void refreshPresetList();

private:
    void presetChosen();
    void savePreset();
    void deletePreset();

    LiveVoiceProcessor& proc;
    juce::ComboBox presetBox;
    juce::TextButton saveBtn { "SAVE" }, deleteBtn { "DELETE" };
    Knob inGain { "INPUT", theme::output }, outGain { "OUTPUT", theme::output }, ceiling { "CEILING", theme::danger };
    LevelMeter inMeter { proc.inputPeakDb }, outMeter { proc.outputPeakDb };
    ToggleBtn limit { "LIMIT", theme::danger }, bypass { "BYPASS", theme::warn };
    GainReductionMeter limMeter { theme::danger, 12.0f };
    juce::StringArray userNames;
    std::unique_ptr<juce::AlertWindow> dialog;
    bool updatingList = false;
};
