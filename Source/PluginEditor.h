#pragma once

#include "PluginProcessor.h"
#include "ui/Theme.h"
#include "ui/RtaDisplay.h"
#include "ui/Panels.h"

class LiveVoiceEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LiveVoiceEditor (LiveVoiceProcessor&);
    ~LiveVoiceEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshNow() { timerCallback(); }   // used by the snapshot tool

private:
    void timerCallback() override;

    LiveVoiceProcessor& proc;
    theme::LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    HeaderBar header { proc };
    RtaDisplay rta { proc };
    DeEssHistogram histogram { proc };
    FeedbackPanel feedbackPanel { proc };
    EqPanel eqPanel { proc };
    DeEssPanel deEssPanel { proc };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveVoiceEditor)
};
