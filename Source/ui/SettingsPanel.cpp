// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#include "SettingsPanel.h"

using namespace params;

// ================================================================ switch
void Switch::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat();
    auto track = r.withSizeKeepingCentre (44.0f, 22.0f).withRightX (r.getRight() - 2.0f);
    const bool on = getToggleState();
    const auto enabled = isEnabled();
    auto fill = on ? accent : theme::border;
    if (! enabled) fill = fill.withAlpha (0.4f);
    else if (over) fill = fill.brighter (0.08f);
    g.setColour (fill);
    g.fillRoundedRectangle (track, 11.0f);
    const float d = 16.0f;
    const float x = on ? track.getRight() - d - 3.0f : track.getX() + 3.0f;
    g.setColour (on ? theme::onAccent : theme::textDim);
    g.fillEllipse (x, track.getCentreY() - d * 0.5f, d, d);

    g.setFont (theme::font (11.5f, true));
    g.setColour (enabled ? (on ? theme::text : theme::textDim) : theme::textFaint);
    g.drawText (on ? "ON" : "OFF", r.withRight (track.getX() - 8.0f), juce::Justification::centredRight);
}

// ================================================================ settings panel
namespace
{
    constexpr int scaleSteps[] { 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150 };
    enum { sizeFit = 1, sizeFirst = 10 };
}

SettingsPanel::SettingsPanel (LiveVoiceProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (true, true);
    sectionNames = { "DISPLAY", "OUTPUTS", "FEEDBACK (ADVANCED)", "DETECTOR FILTERS" };

    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeBtn);

    // ---- display
    lightSwitch.onClick = [this] { if (onThemeToggle) onThemeToggle(); };
    addRow (0, "Light theme", "Bright rooms, daylight services", lightSwitch);

    sizeBox.addItem ("Fit to this screen", sizeFit);
    for (int i = 0; i < (int) std::size (scaleSteps); ++i) sizeBox.addItem (juce::String (scaleSteps[i]) + " %", sizeFirst + i);
    sizeBox.onChange = [this]
    {
        const int id = sizeBox.getSelectedId();
        if (id == 0 || onSetScale == nullptr) return;
        onSetScale (id == sizeFit ? 0.0f : (float) scaleSteps[id - sizeFirst] / 100.0f);
    };
    addRow (0, "Window size", "Also: drag the window corner", sizeBox);

   #if LIVEVOICE_USE_OPENGL
    gpuSwitch.onClick = [this] { if (setGpu) setGpu (gpuSwitch.getToggleState()); };
    addRow (0, "GPU drawing", "Much lower CPU for the RTA and meters", gpuSwitch);
   #endif

    spanBox.addItemList ({ "10 seconds", "25 seconds", "60 seconds" }, 1);
    spanBox.onChange = [this] { if (spanBox.getSelectedId() > 0) proc.historySpan = spanBox.getSelectedId() - 1; };
    addRow (0, "History length", "De-ess and compressor strips", spanBox);

    // ---- outputs
    monitorBox.addItemList ({ "Processed (FX)", "Dry input" }, 1);
    monitorBox.onChange = [this] { if (monitorBox.getSelectedId() > 0) proc.monitorDry = monitorBox.getSelectedId() == 2; };
    addRow (1, "Output 2 (Monitor) plays", "Same as MON A/B in the header", monitorBox);

    soloBox.addItemList ({ "Output 2 (Monitor)", "Output 1 (FOH) - replaces it" }, 1);
    soloBox.onChange = [this] { if (soloBox.getSelectedId() > 0) proc.soloToMain = soloBox.getSelectedId() == 2; };
    addRow (1, "SOLO goes to", "FOH only if output 2 isn't wired", soloBox);

    deEssSoloBox.addItemList ({ "Sibilance band", "What it removes" }, 1);
    deEssSoloBox.onChange = [this] { if (deEssSoloBox.getSelectedId() > 0) proc.deEssListen = deEssSoloBox.getSelectedId() - 1; };
    addRow (1, "De-esser SOLO plays", "Band = tune Freq; removed = check it", deEssSoloBox);

    // ---- feedback
    widthBox.attach (proc.apvts, fbWidth, { "Wide 1/5 oct", "Normal 1/10 oct", "Narrow 1/20 oct", "Ultra 1/40 oct" });
    addRow (2, "Notch width", "Narrower = less tone change", widthBox);
    setupSlider (maxFixed, fbMaxFixed, theme::feedback);
    addRow (2, "Max fixed notches", "Set by RING-OUT LEARN", maxFixed);
    setupSlider (maxFloat, fbMaxFloat, theme::feedback);
    addRow (2, "Max floating notches", "Catch feedback during the service", maxFloat);
    setupSlider (loFreq, fbLoFreq, theme::feedback);
    addRow (2, "Detect from", "Lowest frequency watched", loFreq);
    setupSlider (hiFreq, fbHiFreq, theme::feedback);
    addRow (2, "Detect up to", "Highest frequency watched", hiFreq);

    // ---- detector filters
    setupSlider (pseHp, pseScHp, theme::pse);
    addRow (3, "PSE key high-pass", "Ignore rumble and stage thump", pseHp);
    setupSlider (pseLp, pseScLp, theme::pse);
    addRow (3, "PSE key low-pass", "Ignore cymbals and hiss", pseLp);
    setupSlider (compHp, compScHp, theme::comp);
    addRow (3, "Compressor key high-pass", "Less pumping on plosives and lows", compHp);
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::addRow (int section, const juce::String& label, const juce::String& hint, juce::Component& c)
{
    rows.push_back ({ label, hint, &c, section });
    addAndMakeVisible (c);
}

void SettingsPanel::setupSlider (juce::Slider& s, const juce::String& paramId, const juce::Colour& accent)
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 20);
    s.setColour (juce::Slider::rotarySliderFillColourId, accent);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, paramId, s));
    if (auto* prm = proc.apvts.getParameter (paramId))
        s.setDoubleClickReturnValue (true, prm->convertFrom0to1 (prm->getDefaultValue()));
}

void SettingsPanel::open()
{
    update();
    setVisible (true);
    toFront (false);
}

void SettingsPanel::update()
{
    lightSwitch.setToggleState (theme::lightMode, juce::dontSendNotification);
    if (isGpuOn) gpuSwitch.setToggleState (isGpuOn(), juce::dontSendNotification);
    if (getScale)
    {
        const int pct = juce::roundToInt (getScale() * 100.0f);
        int id = 0;
        for (int i = 0; i < (int) std::size (scaleSteps); ++i) if (scaleSteps[i] == pct) id = sizeFirst + i;
        if (id != 0) sizeBox.setSelectedId (id, juce::dontSendNotification);
        else sizeBox.setText (juce::String (pct) + " %", juce::dontSendNotification);
    }
    spanBox.setSelectedId (juce::jlimit (0, 2, proc.historySpan) + 1, juce::dontSendNotification);
    monitorBox.setSelectedId (proc.monitorDry.load() ? 2 : 1, juce::dontSendNotification);
    monitorBox.setEnabled (proc.isMonitorBusActive());
    soloBox.setSelectedId (proc.soloToMain.load() ? 2 : 1, juce::dontSendNotification);
    deEssSoloBox.setSelectedId (juce::jlimit (0, 1, proc.deEssListen.load()) + 1, juce::dontSendNotification);
}

void SettingsPanel::mouseUp (const juce::MouseEvent& e)
{
    if (! card.contains (e.getPosition()) && onClose) onClose();
}

void SettingsPanel::resized()
{
    card = getLocalBounds().withSizeKeepingCentre (940, 600);
    auto r = card.reduced (26, 0);
    auto head = r.removeFromTop (58);
    closeBtn.setBounds (head.removeFromRight (84).withSizeKeepingCentre (84, 28));
    r.removeFromBottom (44);

    constexpr int rowH = 50, sectionH = 34, colGap = 40;
    const int colW = (r.getWidth() - colGap) / 2;
    sectionAreas.assign (4, {});
    juce::Rectangle<int> cols[2] { r.removeFromLeft (colW), r.withTrimmedLeft (colGap) };

    for (int s = 0; s < 4; ++s)
    {
        auto& col = cols[s / 2];
        if (s % 2 == 1) col.removeFromTop (14);
        sectionAreas[(size_t) s] = col.removeFromTop (sectionH);
        for (auto& row : rows)
        {
            if (row.section != s) continue;
            auto line = col.removeFromTop (rowH);
            auto ctl = line.removeFromRight (line.getWidth() * 9 / 20);
            row.control->setBounds (ctl.withSizeKeepingCentre (ctl.getWidth(), 28));
        }
    }
}

void SettingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (theme::lightMode ? 0.28f : 0.5f));

    auto c = card.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRoundedRectangle (c.translated (0.0f, 6.0f).expanded (4.0f), 14.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (c, 12.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (c, 12.0f, 1.0f);

    g.setColour (theme::text);
    g.setFont (theme::font (18.0f, true));
    g.drawText ("SETTINGS", card.getX() + 26, card.getY() + 14, 300, 30, juce::Justification::centredLeft);
    g.setColour (theme::border);
    g.drawHorizontalLine (card.getY() + 58, c.getX() + 1.0f, c.getRight() - 1.0f);

    for (int s = 0; s < 4; ++s)
    {
        auto a = sectionAreas[(size_t) s];
        g.setColour (theme::textFaint);
        g.setFont (theme::font (11.0f, true));
        g.drawText (sectionNames[s], a.withTrimmedTop (10), juce::Justification::centredLeft);
    }

    for (auto& row : rows)
    {
        auto b = row.control->getBounds();
        const int left = sectionAreas[(size_t) row.section].getX();
        auto text = juce::Rectangle<int> (left, b.getCentreY() - 19, b.getX() - left - 10, 38);
        g.setColour (row.control->isEnabled() ? theme::text : theme::textFaint);
        g.setFont (theme::font (13.0f, true));
        g.drawText (row.label, text.removeFromTop (20), juce::Justification::bottomLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f));
        g.drawText (row.hint, text, juce::Justification::topLeft);
        g.setColour (theme::grid);
        g.drawHorizontalLine (b.getCentreY() + 24, (float) left, (float) b.getRight());
    }

    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f));
    g.drawText ("LiveVoice v" LIVEVOICE_VERSION "   -   Feedback and detector settings are saved with presets and the session. "
                "Display settings are remembered on this computer.",
                card.withTop (card.getBottom() - 40).reduced (26, 0), juce::Justification::centredLeft);
}
