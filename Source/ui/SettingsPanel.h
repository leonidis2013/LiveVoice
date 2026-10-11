// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#pragma once

#include "Panels.h"

// Pill-shaped on/off switch.
class Switch : public juce::ToggleButton
{
public:
    explicit Switch (const juce::Colour& accentColour) : accent (accentColour) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
private:
    const juce::Colour& accent;
};

// SETTINGS: the options you set once and rarely touch during a service.
// Shown as a card over the whole UI; click outside it or CLOSE to dismiss.
class SettingsPanel : public juce::Component
{
public:
    explicit SettingsPanel (LiveVoiceProcessor& p);
    ~SettingsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;   // click on the dimmed area closes
    void update();          // reflect state changed elsewhere (header, RTA)
    void open();

    // wired by the editor
    std::function<void()> onThemeToggle, onClose;
    std::function<void (float)> onSetScale;   // 0 = fit to screen
    std::function<float()> getScale;
    std::function<bool()> isGpuOn;
    std::function<void (bool)> setGpu;

private:
    struct Row
    {
        juce::String label, hint;
        juce::Component* control = nullptr;
        int section = 0;
    };

    LiveVoiceProcessor& proc;
    juce::Rectangle<int> card;
    std::vector<Row> rows;
    juce::StringArray sectionNames;
    std::vector<juce::Rectangle<int>> sectionAreas;

    juce::TextButton closeBtn { "CLOSE" };

    // display
    Switch lightSwitch { theme::output }, gpuSwitch { theme::output };
    juce::ComboBox sizeBox, spanBox;
    // outputs
    juce::ComboBox monitorBox, soloBox, deEssSoloBox;
    // feedback
    Combo widthBox;
    juce::Slider maxFixed, maxFloat, loFreq, hiFreq;
    // detector filters
    juce::Slider pseHp, pseLp, compHp;

    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;

    void addRow (int section, const juce::String& label, const juce::String& hint, juce::Component& c);
    void setupSlider (juce::Slider& s, const juce::String& paramId, const juce::Colour& accent);
};
