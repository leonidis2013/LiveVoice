#pragma once

#include "PluginProcessor.h"
#include "ui/Theme.h"
#include "ui/RtaDisplay.h"
#include "ui/Panels.h"

// The whole UI is laid out once at a fixed design size and scaled as a
// unit, so it stays a single page at any window size.
class LiveVoiceEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int designWidth = 1600, designHeight = 940;

    explicit LiveVoiceEditor (LiveVoiceProcessor&);
    ~LiveVoiceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshNow() { timerCallback(); }   // used by the snapshot tool

private:
    void timerCallback() override;
    void layoutCanvas();

    // background that draws the signal-flow arrows between the bottom panels
    struct Canvas : public juce::Component
    {
        std::vector<juce::Rectangle<int>> gaps;
        void paint (juce::Graphics&) override;
    };

    LiveVoiceProcessor& proc;
    theme::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    Canvas canvas;
    HeaderBar header { proc };
    RtaDisplay rta { proc };
    DeEssHistogram histogram { proc };
    PsePanel psePanel { proc };
    FeedbackPanel feedbackPanel { proc };
    EqPanel eqPanel { proc };
    DeEssPanel deEssPanel { proc };
    CompPanel compPanel { proc };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveVoiceEditor)
};
