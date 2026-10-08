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
// Scrolling history: level bars from the bottom, gain reduction hanging from the
// top, threshold line, and dB scales on both sides.
class HistoryStrip : public juce::Component
{
public:
    explicit HistoryStrip (LiveVoiceProcessor& p) : proc (p) { setOpaque (true); }
    void update();
    void paint (juce::Graphics&) override;

protected:
    struct Config
    {
        juce::String title, thresholdText;
        const juce::Colour* accent = nullptr;
        bool on = true;
        float lo = -60.0f, hi = 0.0f, threshold = -30.0f, grMax = 12.0f, currentGr = 0.0f;
    };
    virtual Config getConfig() = 0;
    virtual void readPoints (dsp::LevelHistory::Point* dest, int count) = 0;

    LiveVoiceProcessor& proc;

private:
    std::vector<dsp::LevelHistory::Point> points;
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
    float key = -100.0f;
};

// Dynamic EQ activity for the selected band: detector level vs threshold, and
// the dynamic gain it produces right now.
class DynActivity : public juce::Component
{
public:
    explicit DynActivity (LiveVoiceProcessor& p) : proc (p) {}
    void setBand (int b) { band = b; repaint(); }
    void update();
    void paint (juce::Graphics&) override;
private:
    LiveVoiceProcessor& proc;
    int band = 0;
    float level = -100.0f, gain = 0.0f;
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
    std::function<void()> onThemeToggle;

private:
    void presetChosen();
    void savePreset();
    void deletePreset();

    LiveVoiceProcessor& proc;
    juce::ComboBox presetBox;
    juce::TextButton saveBtn { "SAVE" }, deleteBtn { "DELETE" };
    juce::TextButton soloWarning { "SOLO ON - CLICK TO CLEAR" };
    Knob inGain { "INPUT", theme::output }, outGain { "OUTPUT", theme::output }, ceiling { "CEILING", theme::danger };
    ToggleBtn limit { "LIMIT", theme::danger }, bypass { "BYPASS", theme::warn }, lightBtn { "LIGHT", theme::text };
    GainReductionMeter limMeter { theme::danger, 12.0f };
    juce::StringArray userNames;
    std::unique_ptr<juce::AlertWindow> dialog;
    bool updatingList = false;
    int flash = 0;
};
