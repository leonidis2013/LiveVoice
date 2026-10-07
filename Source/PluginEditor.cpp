#include "PluginEditor.h"

LiveVoiceEditor::LiveVoiceEditor (LiveVoiceProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    addAndMakeVisible (canvas);
    canvas.setSize (designWidth, designHeight);
    for (auto* c : { (juce::Component*) &header, (juce::Component*) &rta, (juce::Component*) &histogram,
                     (juce::Component*) &psePanel, (juce::Component*) &feedbackPanel, (juce::Component*) &eqPanel,
                     (juce::Component*) &deEssPanel, (juce::Component*) &compPanel })
        canvas.addAndMakeVisible (c);
    layoutCanvas();

    // selecting a band on the RTA and in the EQ panel stay in sync
    rta.onBandSelected = [this] (int b) { eqPanel.selectBand (b); };
    eqPanel.onBandSelected = [this] (int b) { rta.setSelectedBand (b); };
    rta.setSelectedBand (proc.selectedBand.load());

    setLookAndFeel (&lnf);   // after the children exist, so they all pick it up

    setResizable (true, true);
    setResizeLimits ((int) (designWidth * 0.6), (int) (designHeight * 0.6), (int) (designWidth * 1.5), (int) (designHeight * 1.5));
    getConstrainer()->setFixedAspectRatio ((double) designWidth / designHeight);
    const float s = juce::jlimit (0.6f, 1.5f, proc.uiScale.load());
    setSize (juce::roundToInt (designWidth * s), juce::roundToInt (designHeight * s));
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
    const float s = (float) getWidth() / (float) designWidth;
    canvas.setTransform (juce::AffineTransform::scale (s));
    proc.uiScale = s;
}

void LiveVoiceEditor::layoutCanvas()
{
    auto r = canvas.getLocalBounds();
    header.setBounds (r.removeFromTop (52));
    r.reduce (10, 8);

    auto bottom = r.removeFromBottom (404);
    r.removeFromBottom (8);
    histogram.setBounds (r.removeFromBottom (78));
    r.removeFromBottom (8);
    rta.setBounds (r);

    // signal flow, left to right: PSE > Feedback > Dynamic EQ > De-esser > Compressor
    constexpr int gap = 12;
    const int w = bottom.getWidth() - 4 * gap;
    const float share[] { 0.152f, 0.258f, 0.278f, 0.144f, 0.168f };
    juce::Component* panels[] { &psePanel, &feedbackPanel, &eqPanel, &deEssPanel, &compPanel };
    canvas.gaps.clear();
    for (int i = 0; i < 5; ++i)
    {
        const int pw = i == 4 ? bottom.getWidth() : juce::roundToInt (w * share[i]);
        panels[i]->setBounds (bottom.removeFromLeft (pw));
        if (i < 4) canvas.gaps.push_back (bottom.removeFromLeft (gap));
    }
}

void LiveVoiceEditor::Canvas::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::textFaint);
    for (auto& gp : gaps)
    {
        const float cx = (float) gp.getCentreX(), cy = (float) gp.getY() + 18.0f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 2.5f, cy - 4.5f);
        chevron.lineTo (cx + 2.5f, cy);
        chevron.lineTo (cx - 2.5f, cy + 4.5f);
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void LiveVoiceEditor::timerCallback()
{
    header.update();
    rta.update();
    histogram.update();
    psePanel.update();
    feedbackPanel.update();
    eqPanel.update();
    deEssPanel.update();
    compPanel.update();
}
