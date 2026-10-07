#include "Panels.h"

using namespace params;

// ================================================================ widgets
Knob::Knob (const juce::String& name, juce::Colour accent)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setMouseDragSensitivity (220);
    addAndMakeVisible (slider);

    label.setText (name, juce::dontSendNotification);
    label.setFont (theme::font (11.5f));
    label.setColour (juce::Label::textColourId, theme::textDim);
    label.setJustificationType (juce::Justification::centred);
    label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (label);
}

void Knob::attach (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    attachment.reset();
    attachment = std::make_unique<SliderAttachment> (state, id, slider);
    if (auto* p = state.getParameter (id))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (14));
    slider.setBounds (r);
}

ToggleBtn::ToggleBtn (const juce::String& buttonText, juce::Colour onColour) : juce::TextButton (buttonText)
{
    setClickingTogglesState (true);
    setColour (juce::TextButton::buttonOnColourId, onColour);
    setColour (juce::TextButton::textColourOnId, theme::bg);
}

void ToggleBtn::attach (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    attachment.reset();
    attachment = std::make_unique<ButtonAttachment> (state, id, *this);
}

void Combo::attach (juce::AudioProcessorValueTreeState& state, const juce::String& id, const juce::StringArray& items)
{
    attachment.reset();
    clear (juce::dontSendNotification);
    addItemList (items, 1);
    attachment = std::make_unique<ComboAttachment> (state, id, *this);
}

void LevelMeter::update()
{
    const float v = src.exchange (-100.0f);
    level = v > level ? v : std::max (v, level - 1.5f);
    if (v >= peakHold) { peakHold = v; holdFrames = 45; }
    else if (--holdFrames < 0) peakHold = std::max (-100.0f, peakHold - 1.0f);
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (r, 2.0f);
    auto toX = [&] (float db) { return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };

    const float x = toX (level);
    juce::ColourGradient grad (theme::good, r.getX(), 0, theme::danger, r.getRight(), 0, false);
    grad.addColour ((toX (-12.0f) - r.getX()) / r.getWidth(), theme::good);
    grad.addColour ((toX (-3.0f) - r.getX()) / r.getWidth(), theme::warn);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r.withRight (x), 2.0f);

    g.setColour (peakHold > -0.1f ? theme::danger : theme::text.withAlpha (0.8f));
    g.fillRect (juce::Rectangle<float> (toX (peakHold) - 1.0f, r.getY(), 2.0f, r.getHeight()));
}

void GainReductionMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (r, 2.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, std::abs (value) / maxReduction);
    g.setColour (colour);
    if (r.getHeight() > r.getWidth())
        g.fillRoundedRectangle (r.withHeight (r.getHeight() * frac), 2.0f);   // hangs from the top
    else
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * frac), 2.0f);
}

// ================================================================ panel base
ModulePanel::ModulePanel (const juce::String& t, juce::Colour a) : title (t), accent (a)
{
    addAndMakeVisible (power);
}

void ModulePanel::layoutHeader()
{
    power.setBounds (getWidth() - 52, 7, 42, 20);
}

juce::Rectangle<int> ModulePanel::getContentBounds() const
{
    return getLocalBounds().reduced (10).withTrimmedTop (26);
}

void ModulePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);

    g.setColour (accent);
    g.fillRoundedRectangle (12.0f, 12.0f, 3.0f, 12.0f, 1.5f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText (title, juce::Rectangle<int> (22, 6, getWidth() - 90, 24), juce::Justification::centredLeft);
}

// ================================================================ histogram
void DeEssHistogram::update()
{
    const int cols = std::max (1, getWidth() / 2);
    points.resize ((size_t) std::min (cols, dsp::DeEsser::historySize));
    proc.deEsser.readHistory (points.data(), (int) points.size());
    repaint();
}

void DeEssHistogram::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    auto labelArea = r.removeFromLeft (118.0f);
    auto area = r.reduced (4.0f, 6.0f);

    const auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const bool on = v (dsOn) > 0.5f;
    const float thr = v (dsThr), rangeDb = std::max (1.0f, v (dsRange));

    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), 0.0f, -60.0f, area.getY(), area.getBottom()); };

    // grid
    g.setColour (theme::grid);
    for (float db = 0.0f; db >= -60.0f; db -= 12.0f) g.drawHorizontalLine ((int) yFor (db), area.getX(), area.getRight());

    const int n = (int) points.size();
    const float colW = area.getWidth() / (float) std::max (1, n);
    for (int i = 0; i < n; ++i)
    {
        const auto& p = points[(size_t) i];
        const float x = area.getX() + (float) i * colW;
        const float y = yFor (p.levelDb);
        const bool active = on && p.grDb > 0.3f;
        const float amount = juce::jlimit (0.0f, 1.0f, p.grDb / rangeDb);

        g.setColour (active ? theme::deess.withAlpha (0.45f + 0.55f * amount) : theme::deess.withAlpha (0.18f));
        g.fillRect (x, y, std::max (1.0f, colW - 0.5f), area.getBottom() - y);

        if (active)   // reduction hangs from the top in pink
        {
            g.setColour (juce::Colour (0xfff472b6).withAlpha (0.65f));
            g.fillRect (x, area.getY(), std::max (1.0f, colW - 0.5f), area.getHeight() * 0.5f * amount);
        }
    }

    // threshold
    const float ty = yFor (thr);
    g.setColour (on ? theme::text.withAlpha (0.7f) : theme::textFaint);
    const float dash[] { 5.0f, 3.0f };
    g.drawDashedLine ({ area.getX(), ty, area.getRight(), ty }, dash, 2, 1.0f);

    // label column
    g.setColour (theme::deess);
    g.fillRoundedRectangle (labelArea.getX() + 10.0f, labelArea.getY() + 11.0f, 3.0f, 12.0f, 1.5f);
    g.setColour (theme::text);
    g.setFont (theme::font (12.5f, true));
    g.drawText ("DE-ESS", labelArea.withTrimmedLeft (20.0f).removeFromTop (34.0f), juce::Justification::centredLeft);
    g.setFont (theme::font (11.5f));
    g.setColour (theme::textDim);
    const float gr = proc.deEsser.getCurrentGainReductionDb();
    g.drawText (on ? ("GR " + juce::String (gr, 1) + " dB") : "OFF", labelArea.withTrimmedLeft (20.0f).withTrimmedTop (30.0f).removeFromTop (18.0f),
                juce::Justification::centredLeft);
    g.drawText ("thr " + juce::String (thr, 0) + " dB", labelArea.withTrimmedLeft (20.0f).withTrimmedTop (48.0f).removeFromTop (18.0f),
                juce::Justification::centredLeft);

    g.setColour (theme::border);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);
}

// ================================================================ feedback panel
FeedbackPanel::FeedbackPanel (LiveVoiceProcessor& p) : ModulePanel ("FEEDBACK SUPPRESSION", theme::feedback), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, fbOn);

    mode.attach (s, fbMode, { "Speech", "Singing", "Multipurpose" });
    width.attach (s, fbWidth, { "Wide 1/5 oct", "Normal 1/10 oct", "Narrow 1/20 oct", "Ultra 1/40 oct" });
    for (auto* c : { &mode, &width }) addAndMakeVisible (c);

    for (auto [lbl, txt] : { std::pair { &modeLabel, "Mode" }, std::pair { &widthLabel, "Notch width" } })
    {
        lbl->setText (txt, juce::dontSendNotification);
        lbl->setFont (theme::font (11.5f));
        lbl->setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (lbl);
    }

    learn.attach (s, fbLearn);
    freeze.attach (s, fbFreeze);
    learn.setTooltip ("Ring-out: raise the gain slowly before the service. Notches found while Learn is on become fixed and are saved with the session.");
    freeze.setTooltip ("Stop detecting and hold every notch exactly as it is.");
    addAndMakeVisible (learn);
    addAndMakeVisible (freeze);

    clearFloat.onClick = [this]
    {
        dsp::FeedbackSuppressor::Command c; c.type = dsp::FeedbackSuppressor::Command::Type::clearFloating;
        proc.sendNotchCommand (c);
    };
    clearAll.onClick = [this]
    {
        juce::Component::SafePointer<FeedbackPanel> safe (this);
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Clear all notches?",
            "This removes the fixed notches from your ring-out as well as the floating ones.", "Clear all", "Cancel", this,
            juce::ModalCallbackFunction::create ([safe] (int r)
            {
                if (safe == nullptr || r == 0) return;
                dsp::FeedbackSuppressor::Command c; c.type = dsp::FeedbackSuppressor::Command::Type::clearAll;
                safe->proc.sendNotchCommand (c);
            }));
    };
    addAndMakeVisible (clearFloat);
    addAndMakeVisible (clearAll);

    sens.attach (s, fbSens); depth.attach (s, fbMaxDepth); hold.attach (s, fbHold);
    maxFixed.attach (s, fbMaxFixed); maxFloat.attach (s, fbMaxFloat); lo.attach (s, fbLoFreq); hi.attach (s, fbHiFreq);
    for (auto* k : { &sens, &depth, &hold, &maxFixed, &maxFloat, &lo, &hi }) addAndMakeVisible (k);

    status.setFont (theme::font (12.0f));
    status.setColour (juce::Label::textColourId, theme::textDim);
    status.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (status);
}

void FeedbackPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();

    auto row = r.removeFromTop (16);
    modeLabel.setBounds (row.removeFromLeft (row.getWidth() / 2));
    widthLabel.setBounds (row);
    row = r.removeFromTop (26);
    mode.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (5));
    width.setBounds (row.withTrimmedLeft (5));

    r.removeFromTop (8);
    row = r.removeFromTop (28);
    const int bw = row.getWidth() / 4;
    learn.setBounds (row.removeFromLeft (bw + 30).withTrimmedRight (4));
    freeze.setBounds (row.removeFromLeft (bw - 18).withTrimmedRight (4));
    clearFloat.setBounds (row.removeFromLeft (bw + 6).withTrimmedRight (4));
    clearAll.setBounds (row);

    r.removeFromTop (6);
    status.setBounds (r.removeFromBottom (18));

    const int knobH = std::min (92, (r.getHeight() - 8) / 2);
    row = r.removeFromTop (knobH);
    const int kw4 = row.getWidth() / 4;
    for (auto* k : { &sens, &depth, &hold, &lo }) k->setBounds (row.removeFromLeft (kw4));
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    for (auto* k : { &hi, &maxFixed, &maxFloat }) k->setBounds (row.removeFromLeft (kw4));
}

void FeedbackPanel::update()
{
    int fixed = 0, floating = 0;
    for (int i = 0; i < dsp::FeedbackSuppressor::maxSlots; ++i)
    {
        const auto n = proc.feedback.getNotch (i);
        if (n.kind == dsp::FeedbackSuppressor::kindFixed) ++fixed;
        else if (n.kind == dsp::FeedbackSuppressor::kindFloating && n.targetDb < -0.01f) ++floating;
    }
    const auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    juce::String txt = "Fixed " + juce::String (fixed) + "/" + juce::String ((int) v (fbMaxFixed))
                     + "   Floating " + juce::String (floating) + "/" + juce::String ((int) v (fbMaxFloat));
    if (v (fbLearn) > 0.5f) txt << "   LEARNING";
    else if (v (fbFreeze) > 0.5f) txt << "   FROZEN";
    status.setText (txt, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, v (fbLearn) > 0.5f ? theme::feedback : theme::textDim);
}

// ================================================================ EQ panel
EqPanel::EqPanel (LiveVoiceProcessor& p) : ModulePanel ("DYNAMIC EQ", theme::eq), proc (p)
{
    power.attach (proc.apvts, eqOn);

    for (int b = 0; b < numEqBands; ++b)
    {
        auto& btn = bandButtons[(size_t) b];
        btn.setClickingTogglesState (false);
        btn.setColour (juce::TextButton::buttonOnColourId, theme::bandColours[b]);
        btn.onClick = [this, b] { selectBand (b); if (onBandSelected) onBandSelected (b); };
        addAndMakeVisible (btn);
    }

    for (auto* c : { (juce::Component*) &bandOn, (juce::Component*) &dynamic, (juce::Component*) &type }) addAndMakeVisible (c);
    for (auto* k : { &freq, &gain, &q, &thr, &range, &ratio, &attack, &release }) addAndMakeVisible (k);

    dynLabel.setFont (theme::font (11.5f));
    dynLabel.setColour (juce::Label::textColourId, theme::textDim);
    dynLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (dynLabel);
    addAndMakeVisible (dynMeter);

    selectBand (proc.selectedBand.load());
}

void EqPanel::selectBand (int b)
{
    band = juce::jlimit (0, numEqBands - 1, b);
    proc.selectedBand = band;
    auto& s = proc.apvts;

    for (int i = 0; i < numEqBands; ++i)
        bandButtons[(size_t) i].setToggleState (i == band, juce::dontSendNotification);

    bandOn.attach (s, bOn (band));
    dynamic.attach (s, bDyn (band));
    if (band == 0)                    type.attach (s, bType (band), { "Bell", "Low Shelf", "High Pass 12", "High Pass 24" });
    else if (band == numEqBands - 1)  type.attach (s, bType (band), { "Bell", "High Shelf", "Low Pass 12", "Low Pass 24" });
    else { type.attachment.reset(); type.clear (juce::dontSendNotification); type.addItem ("Bell", 1); type.setSelectedId (1, juce::dontSendNotification); }
    type.setEnabled (band == 0 || band == numEqBands - 1);

    freq.attach (s, bFreq (band)); gain.attach (s, bGain (band)); q.attach (s, bQ (band));
    thr.attach (s, bThr (band)); range.attach (s, bRange (band)); ratio.attach (s, bRatio (band));
    attack.attach (s, bAtt (band)); release.attach (s, bRel (band));

    for (auto* k : { &freq, &gain, &q, &thr, &range, &ratio, &attack, &release })
        k->slider.setColour (juce::Slider::rotarySliderFillColourId, theme::bandColours[band]);
    dynMeter.repaint();
    update();
}

void EqPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();

    auto row = r.removeFromTop (26);
    const int bw = row.getWidth() / numEqBands;
    for (auto& b : bandButtons) b.setBounds (row.removeFromLeft (bw).withTrimmedRight (4));

    r.removeFromTop (8);
    row = r.removeFromTop (26);
    bandOn.setBounds (row.removeFromLeft (84).withTrimmedRight (6));
    type.setBounds (row.removeFromLeft (138).withTrimmedRight (6));
    dynamic.setBounds (row.removeFromLeft (84).withTrimmedRight (6));
    dynMeter.setBounds (row.removeFromRight (60).withSizeKeepingCentre (60, 8));
    dynLabel.setBounds (row.withTrimmedRight (6));

    r.removeFromTop (6);
    const int knobH = std::min (92, (r.getHeight() - 10) / 2);
    row = r.removeFromTop (knobH);
    const int kw = row.getWidth() / 5;
    for (auto* k : { &freq, &gain, &q }) k->setBounds (row.removeFromLeft (kw));
    r.removeFromTop (10);
    row = r.removeFromTop (knobH);
    for (auto* k : { &thr, &range, &ratio, &attack, &release }) k->setBounds (row.removeFromLeft (kw));
}

void EqPanel::update()
{
    const auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };

    for (int b = 0; b < numEqBands; ++b)
    {
        const auto t = (b == 0 || b == numEqBands - 1) ? bandTypeFromChoice (b, (int) v (bType (b))) : BandType::bell;
        juce::String txt (b + 1);
        switch (t)
        {
            case BandType::lowShelf:   txt << " LS"; break;
            case BandType::highShelf:  txt << " HS"; break;
            case BandType::highPass12: case BandType::highPass24: txt << " HP"; break;
            case BandType::lowPass12:  case BandType::lowPass24:  txt << " LP"; break;
            case BandType::bell: default: break;
        }
        if (v (bDyn (b)) > 0.5f && ! isPassType (t)) txt << " *";
        auto& btn = bandButtons[(size_t) b];
        if (btn.getButtonText() != txt) btn.setButtonText (txt);
        btn.setAlpha (v (bOn (b)) > 0.5f ? 1.0f : 0.45f);
    }

    const auto t = (band == 0 || band == numEqBands - 1) ? bandTypeFromChoice (band, (int) v (bType (band))) : BandType::bell;
    const bool pass = isPassType (t);
    const bool dyn = v (bDyn (band)) > 0.5f && ! pass;
    gain.setEnabled (! pass);
    q.setEnabled (t != BandType::highPass24 && t != BandType::lowPass24);
    dynamic.setEnabled (! pass);
    for (auto* k : { &thr, &range, &ratio, &attack, &release }) k->setEnabled (dyn);

    const float dg = dyn ? proc.dynEq.getDynamicGainDb (band) : 0.0f;
    dynLabel.setText (dyn ? "Dyn " + juce::String (dg, 1) + " dB" : (pass ? "Filter" : "Static"), juce::dontSendNotification);
    dynMeter.setValue (dg);
}

// ================================================================ de-esser panel
DeEssPanel::DeEssPanel (LiveVoiceProcessor& p) : ModulePanel ("DE-ESSER", theme::deess), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, dsOn);
    mode.attach (s, dsMode, { "Split (highs only)", "Wideband" });
    addAndMakeVisible (mode);
    freq.attach (s, dsFreq); thr.attach (s, dsThr); range.attach (s, dsRange); rel.attach (s, dsRel);
    for (auto* k : { &freq, &thr, &range, &rel }) addAndMakeVisible (k);
    addAndMakeVisible (grMeter);
    grLabel.setFont (theme::font (11.0f));
    grLabel.setColour (juce::Label::textColourId, theme::textDim);
    grLabel.setJustificationType (juce::Justification::centred);
    grLabel.setText ("GR", juce::dontSendNotification);
    addAndMakeVisible (grLabel);
}

void DeEssPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();
    auto meterCol = r.removeFromRight (30);
    grLabel.setBounds (meterCol.removeFromTop (16));
    grMeter.setBounds (meterCol.withSizeKeepingCentre (10, meterCol.getHeight() - 4));
    r.removeFromRight (6);

    mode.setBounds (r.removeFromTop (26));
    r.removeFromTop (12);
    const int knobH = std::min (92, (r.getHeight() - 10) / 2);
    auto row = r.removeFromTop (knobH);
    freq.setBounds (row.removeFromLeft (row.getWidth() / 2));
    thr.setBounds (row);
    r.removeFromTop (10);
    row = r.removeFromTop (knobH);
    range.setBounds (row.removeFromLeft (row.getWidth() / 2));
    rel.setBounds (row);
}

void DeEssPanel::update()
{
    grMeter.setValue (proc.deEsser.getCurrentGainReductionDb());
}

// ================================================================ header
HeaderBar::HeaderBar (LiveVoiceProcessor& p) : proc (p)
{
    auto& s = proc.apvts;

    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] { presetChosen(); };
    addAndMakeVisible (presetBox);
    saveBtn.onClick = [this] { savePreset(); };
    deleteBtn.onClick = [this] { deletePreset(); };
    addAndMakeVisible (saveBtn);
    addAndMakeVisible (deleteBtn);

    inGain.attach (s, params::inGain);
    outGain.attach (s, params::outGain);
    ceiling.attach (s, params::ceiling);
    limit.attach (s, params::limitOn);
    bypass.attach (s, params::bypass);
    for (auto* k : { &inGain, &outGain, &ceiling })
    {
        k->slider.setSliderStyle (juce::Slider::LinearHorizontal);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 18);
        k->label.setJustificationType (juce::Justification::centredLeft);
        k->label.setFont (theme::font (11.5f, true));
        addAndMakeVisible (k);
    }
    for (auto* c : { (juce::Component*) &inMeter, (juce::Component*) &outMeter, (juce::Component*) &limit,
                     (juce::Component*) &bypass, (juce::Component*) &limMeter })
        addAndMakeVisible (c);

    refreshPresetList();
}

void HeaderBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ("LIVEVOICE", juce::Rectangle<int> (14, 0, 110, getHeight()), juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f));
    g.drawText ("zero latency", juce::Rectangle<int> (104, 0, 64, getHeight()), juce::Justification::centredLeft);
    g.setColour (theme::border);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void HeaderBar::resized()
{
    auto r = getLocalBounds().reduced (8, 0).withTrimmedLeft (168);
    const int h = 26, cy = getHeight() / 2;
    auto centreRow = [&] (juce::Rectangle<int> b) { return b.withSizeKeepingCentre (b.getWidth(), h).withY (cy - h / 2); };

    presetBox.setBounds (centreRow (r.removeFromLeft (220)));
    r.removeFromLeft (6);
    saveBtn.setBounds (centreRow (r.removeFromLeft (58)));
    r.removeFromLeft (4);
    deleteBtn.setBounds (centreRow (r.removeFromLeft (66)));

    bypass.setBounds (centreRow (r.removeFromRight (76)));
    r.removeFromRight (14);

    // [label][slider........][value]   with a meter under the slider
    auto gainBlock = [&] (Knob& k, juce::Component* meter, int width)
    {
        auto b = r.removeFromRight (width);
        k.setBounds (b.withHeight (28).withY (cy - 16));
        auto kb = k.getLocalBounds();
        k.label.setBounds (kb.removeFromLeft (50));
        k.slider.setBounds (kb);
        if (meter != nullptr)
            meter->setBounds (b.getX() + 56, cy + 12, b.getWidth() - 56 - 64, 5);
        r.removeFromRight (14);
    };

    gainBlock (ceiling, &limMeter, 172);
    limit.setBounds (centreRow (r.removeFromRight (60)));
    r.removeFromRight (14);
    gainBlock (outGain, &outMeter, 184);
    gainBlock (inGain, &inMeter, 184);
}

void HeaderBar::update()
{
    inMeter.update();
    outMeter.update();
    limMeter.setValue (proc.limiter.getGainReductionDb());
}

void HeaderBar::refreshPresetList()
{
    updatingList = true;
    presetBox.clear (juce::dontSendNotification);
    auto factory = proc.presets.getFactoryNames();
    userNames = proc.presets.getUserNames();

    auto* menu = presetBox.getRootMenu();
    menu->addSectionHeader ("Factory");
    for (int i = 0; i < factory.size(); ++i) presetBox.addItem (factory[i], 1 + i);
    menu->addSeparator();
    menu->addSectionHeader ("My presets");
    if (userNames.isEmpty()) menu->addItem (juce::PopupMenu::Item ("(none saved yet)").setEnabled (false));
    for (int i = 0; i < userNames.size(); ++i) presetBox.addItem (userNames[i], 1000 + i);

    // reflect the current name without loading anything
    const auto current = proc.presets.getCurrentName();
    int id = 0;
    for (int i = 0; i < userNames.size() && id == 0; ++i) if (userNames[i] == current) id = 1000 + i;
    for (int i = 0; i < factory.size() && id == 0; ++i) if (factory[i] == current) id = 1 + i;
    if (id != 0) presetBox.setSelectedId (id, juce::dontSendNotification);
    else presetBox.setText (current, juce::dontSendNotification);

    deleteBtn.setEnabled (id >= 1000);
    updatingList = false;
}

void HeaderBar::presetChosen()
{
    if (updatingList) return;
    const int id = presetBox.getSelectedId();
    if (id >= 1000) proc.presets.loadUser (userNames[id - 1000]);
    else if (id >= 1) proc.presets.loadFactory (id - 1);
    deleteBtn.setEnabled (id >= 1000);
}

void HeaderBar::savePreset()
{
    dialog = std::make_unique<juce::AlertWindow> ("Save preset",
        "Saves the feedback, dynamic EQ and de-esser settings.\nGain, limiter and learned notches are not part of presets.",
        juce::MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor ("name", proc.presets.getCurrentName(), "Name:");
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HeaderBar> safe (this);
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr) return;
        if (result == 1)
        {
            const auto name = safe->dialog->getTextEditorContents ("name").trim();
            if (name.isNotEmpty())
            {
                const bool exists = safe->proc.presets.getUserNames().contains (name);
                const bool isFactory = safe->proc.presets.getFactoryNames().contains (name);
                if (isFactory)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save preset",
                                                            "That name belongs to a factory preset. Please pick another name.");
                else if (! safe->proc.presets.saveUser (name))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save preset",
                                                            "The preset could not be written to\n" + PresetManager::getUserPresetDirectory().getFullPathName());
                juce::ignoreUnused (exists);
                safe->refreshPresetList();
            }
        }
        safe->dialog.reset();
    }), false);
}

void HeaderBar::deletePreset()
{
    const int id = presetBox.getSelectedId();
    if (id < 1000) return;
    const auto name = userNames[id - 1000];
    juce::Component::SafePointer<HeaderBar> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete preset",
        "Delete \"" + name + "\"?", "Delete", "Cancel", this,
        juce::ModalCallbackFunction::create ([safe, name] (int r)
        {
            if (safe == nullptr || r == 0) return;
            safe->proc.presets.deleteUser (name);
            safe->proc.presets.setCurrentName ("Default");
            safe->refreshPresetList();
        }));
}
