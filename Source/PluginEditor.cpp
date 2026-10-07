#include "PluginEditor.h"

LiveVoiceEditor::LiveVoiceEditor (LiveVoiceProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    for (auto* c : { (juce::Component*) &header, (juce::Component*) &rta, (juce::Component*) &histogram,
                     (juce::Component*) &feedbackPanel, (juce::Component*) &eqPanel, (juce::Component*) &deEssPanel })
        addAndMakeVisible (c);

    // selecting a band on the RTA and in the EQ panel stay in sync
    rta.onBandSelected = [this] (int b) { eqPanel.selectBand (b); };
    eqPanel.onBandSelected = [this] (int b) { rta.setSelectedBand (b); };
    rta.setSelectedBand (proc.selectedBand.load());

    setLookAndFeel (&lnf);   // after the children exist, so they all pick it up

    setResizable (true, true);
    setResizeLimits (1280, 800, 2400, 1500);
    setSize (1280, 860);
    startTimerHz (30);
}

LiveVoiceEditor::~LiveVoiceEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LiveVoiceEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void LiveVoiceEditor::resized()
{
    auto r = getLocalBounds();
    header.setBounds (r.removeFromTop (52));
    r.reduce (10, 8);

    const int bottomH = std::max (330, (int) (r.getHeight() * 0.42f));
    auto bottom = r.removeFromBottom (bottomH);
    r.removeFromBottom (8);

    const int histH = std::max (72, (int) (r.getHeight() * 0.18f));
    histogram.setBounds (r.removeFromBottom (histH));
    r.removeFromBottom (8);
    rta.setBounds (r);

    const int w = bottom.getWidth() - 16;
    feedbackPanel.setBounds (bottom.removeFromLeft ((int) (w * 0.35f)));
    bottom.removeFromLeft (8);
    deEssPanel.setBounds (bottom.removeFromRight ((int) (w * 0.22f)));
    bottom.removeFromRight (8);
    eqPanel.setBounds (bottom);
}

void LiveVoiceEditor::timerCallback()
{
    header.update();
    rta.update();
    histogram.update();
    feedbackPanel.update();
    eqPanel.update();
    deEssPanel.update();
}
