#include "PluginEditor.h"

LiveVoiceEditor::LiveVoiceEditor (LiveVoiceProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    addAndMakeVisible (canvas);
    canvas.setSize (designWidth, designHeight);
    for (auto* c : { (juce::Component*) &header, (juce::Component*) &rta, (juce::Component*) &ioMeters,
                     (juce::Component*) &deEssHistory, (juce::Component*) &compHistory, (juce::Component*) &compGraph,
                     (juce::Component*) &psePanel, (juce::Component*) &feedbackPanel, (juce::Component*) &eqPanel,
                     (juce::Component*) &deEssPanel, (juce::Component*) &compPanel })
        canvas.addAndMakeVisible (c);
    canvas.addChildComponent (settings);   // overlay, on top of everything
    layoutCanvas();

    // selecting a band on the RTA and in the EQ panel stay in sync
    rta.onBandSelected = [this] (int b) { eqPanel.selectBand (b); };
    eqPanel.onBandSelected = [this] (int b) { rta.setSelectedBand (b); };
    rta.setSelectedBand (proc.selectedBand.load());

    header.onOpenSettings = [this] { openSettings(); };
    settings.onClose = [this] { settings.setVisible (false); };
    settings.onThemeToggle = [this] { toggleTheme(); settings.update(); };
    settings.onSetScale = [this] (float sc) { setScale (sc); };
    settings.getScale = [this] { return (float) getWidth() / (float) designWidth; };
   #if LIVEVOICE_USE_OPENGL
    settings.isGpuOn = [this] { return glContext.isAttached(); };
    settings.setGpu = [this] (bool on) { setGpu (on); };
   #endif
    deEssHistory.onSpanChanged = [this] { compHistory.update(); };
    compHistory.onSpanChanged = [this] { deEssHistory.update(); };
    setLookAndFeel (&lnf);   // after the children exist, so they all pick it up

    setResizable (true, true);
    setResizeLimits ((int) (designWidth * 0.5), (int) (designHeight * 0.5), (int) (designWidth * 1.5), (int) (designHeight * 1.5));
    getConstrainer()->setFixedAspectRatio ((double) designWidth / designHeight);
    // first time: auto-fit to the screen; afterwards: the size you chose last
    const float s = proc.uiScaleRestored ? juce::jlimit (0.5f, 1.5f, proc.uiScale.load()) : fitScale();
    proc.uiScaleRestored = true;
    setSize (juce::roundToInt (designWidth * s), juce::roundToInt (designHeight * s));

   #if LIVEVOICE_USE_OPENGL
    if (theme::loadGpuPreference()) setGpu (true);
   #endif
    startTimerHz (30);
}

LiveVoiceEditor::~LiveVoiceEditor()
{
    stopTimer();
   #if LIVEVOICE_USE_OPENGL
    glContext.detach();
   #endif
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
    settings.setBounds (r);
    header.setBounds (r.removeFromTop (52));
    r.reduce (10, 8);

    auto bottom = r.removeFromBottom (408);
    r.removeFromBottom (8);

    // middle row: de-ess history | compressor history | compressor curve
    auto mid = r.removeFromBottom (116);
    compGraph.setBounds (mid.removeFromRight (250));
    mid.removeFromRight (8);
    const int histW = (mid.getWidth() - 8) / 2;
    deEssHistory.setBounds (mid.removeFromLeft (histW));
    mid.removeFromLeft (8);
    compHistory.setBounds (mid);
    r.removeFromBottom (8);

    // RTA with the input/output meters beside it
    ioMeters.setBounds (r.removeFromRight (92));
    r.removeFromRight (8);
    rta.setBounds (r);

    // signal flow, left to right: PSE > Feedback > Dynamic EQ > De-esser > Compressor
    constexpr int gap = 12;
    const int w = bottom.getWidth() - 4 * gap;
    const float share[] { 0.150f, 0.205f, 0.300f, 0.160f, 0.185f };
    juce::Component* panels[] { &psePanel, &feedbackPanel, &eqPanel, &deEssPanel, &compPanel };
    canvas.gaps.clear();
    for (int i = 0; i < 5; ++i)
    {
        const int pw = i == 4 ? bottom.getWidth() : juce::roundToInt ((float) w * share[i]);
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

void LiveVoiceEditor::toggleTheme()
{
    const auto before = theme::currentPalette();
    theme::setLightMode (! theme::lightMode);
    theme::saveLightModePreference (theme::lightMode);
    lnf.applyColours();
    theme::remapColours (*this, before, theme::currentPalette());
    sendLookAndFeelChange();
    rta.invalidateCaches();
    repaint();
}

float LiveVoiceEditor::fitScale() const
{
    auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (display == nullptr) return 0.85f;
    const auto area = display->userArea;
    const float s = std::min ((float) area.getWidth() * 0.92f / (float) designWidth, (float) area.getHeight() * 0.88f / (float) designHeight);
    return juce::jlimit (0.5f, 1.5f, s);
}

void LiveVoiceEditor::setScale (float scale)
{
    const float s = scale <= 0.0f ? fitScale() : juce::jlimit (0.5f, 1.5f, scale);
    setSize (juce::roundToInt (designWidth * s), juce::roundToInt (designHeight * s));
}

void LiveVoiceEditor::openSettings()
{
    settings.open();
}

void LiveVoiceEditor::setGpu (bool on)
{
   #if LIVEVOICE_USE_OPENGL
    if (on && ! glContext.isAttached()) glContext.attachTo (*this);
    else if (! on && glContext.isAttached()) glContext.detach();
    theme::saveGpuPreference (on);
   #else
    juce::ignoreUnused (on);
   #endif
}

void LiveVoiceEditor::timerCallback()
{
    // fast (30 fps): things that move quickly
    rta.update();
    ioMeters.update();
    compPanel.update();

    // slow (15 fps): slowly scrolling strips, panels, header
    if ((++tick & 1) == 0)
    {
        header.update();
        deEssHistory.update();
        compHistory.update();
        compGraph.update();
        psePanel.update();
        feedbackPanel.update();
        eqPanel.update();
        deEssPanel.update();
        proc.pollFeedbackLog();
        if (settings.isVisible()) settings.update();
    }
}
