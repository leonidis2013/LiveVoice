// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#include "Panels.h"

using namespace params;

namespace
{
    float raw (LiveVoiceProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

    juce::String dbText (float db, int decimals = 1)
    {
        return (db > 0.05f ? "+" : "") + juce::String (db, decimals);
    }

    const juce::Colour reductionColour { 0xfff472b6 };   // pink: gain reduction in histograms

    void setReal (LiveVoiceProcessor& p, const juce::String& id, float v)
    {
        if (auto* rp = p.apvts.getParameter (id))
        {
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->convertTo0to1 (v));
            rp->endChangeGesture();
        }
    }

    constexpr double suggestSeconds = 20.0;

    // Shared Suggest button behaviour: start / show progress / cancel.
    // Returns true when a finished capture for `target` is ready to analyse.
    bool driveSuggestButton (LiveVoiceProcessor& p, juce::TextButton& b, LiveVoiceProcessor::CaptureTarget target)
    {
        const bool mine = p.getCaptureTarget() == target;
        if (mine && p.captureFinished()) { b.setButtonText ("SUGGEST"); return true; }
        if (mine)
        {
            const int left = juce::jmax (0, (int) std::ceil (suggestSeconds * (1.0 - p.getCaptureProgress())));
            const juce::String t = "LISTENING " + juce::String (left) + "s";
            if (b.getButtonText() != t) b.setButtonText (t);
            b.setToggleState (true, juce::dontSendNotification);
        }
        else
        {
            if (b.getButtonText() != "SUGGEST") b.setButtonText ("SUGGEST");
            b.setToggleState (false, juce::dontSendNotification);
        }
        return false;
    }

    void clickSuggest (LiveVoiceProcessor& p, LiveVoiceProcessor::CaptureTarget target)
    {
        if (p.getCaptureTarget() == target) p.cancelCapture();
        else p.startCapture (target, suggestSeconds);
    }
}

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
    setColour (juce::TextButton::textColourOnId, theme::onAccent);
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

void GainReductionMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const float frac = juce::jlimit (0.0f, 1.0f, std::abs (value) / maxReduction);
    const bool vertical = r.getHeight() > r.getWidth();

    if (vertical)
    {
        r.reduce (0.0f, 6.0f);
        auto bar = r.removeFromLeft (std::min (10.0f, r.getWidth()));
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (*colour);
        g.fillRoundedRectangle (bar.withHeight (bar.getHeight() * frac), 2.0f);   // hangs from the top

        if (r.getWidth() >= 14.0f)   // dB scale
        {
            const float step = maxReduction > 18.0f ? 6.0f : 3.0f;
            g.setFont (theme::font (9.5f));
            for (float db = 0.0f; db <= maxReduction + 0.01f; db += step)
            {
                const float y = bar.getY() + bar.getHeight() * db / maxReduction;
                const bool major = std::fmod (db, step * 2.0f) < 0.01f || db >= maxReduction - 0.01f;
                g.setColour (theme::textFaint);
                if (! major) g.fillRect (r.getX() + 2.0f, y - 0.5f, 4.0f, 1.0f);
                else
                {
                    g.setColour (theme::textDim);
                    g.drawText (db < 0.01f ? "0" : "-" + juce::String ((int) db),
                                juce::Rectangle<float> (r.getX() + 2.0f, y - 6.0f, r.getWidth() - 2.0f, 12.0f),
                                juce::Justification::centredLeft);
                }
            }
        }
    }
    else
    {
        const bool withText = r.getWidth() >= 90.0f;
        auto textArea = withText ? r.removeFromRight (56.0f) : juce::Rectangle<float>();
        auto bar = r.withSizeKeepingCentre (r.getWidth(), std::min (r.getHeight(), 6.0f));
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (*colour);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 2.0f);
        g.setColour (theme::textFaint);
        for (float db = 0.0f; db <= maxReduction + 0.01f; db += maxReduction / 4.0f)
            g.fillRect (bar.getX() + bar.getWidth() * db / maxReduction - 0.5f, bar.getBottom() + 1.0f, 1.0f, 2.0f);
        if (withText)
        {
            g.setColour (value > 0.05f ? *colour : theme::textDim);
            g.setFont (theme::font (11.0f, true));
            g.drawText ("GR " + juce::String (-std::abs (value), 1), textArea, juce::Justification::centredRight);
        }
    }
}

// ================================================================ I/O meters
IoMeters::IoMeters (LiveVoiceProcessor& p) : proc (p)
{
    setTooltip ("Input (after the INPUT trim) and output peak levels. Aim for peaks in the green TARGET zone "
                "(-18 to -6 dBFS): lower wastes headroom against noise, higher risks clipping. Click to reset the peak hold.");
}

void IoMeters::ballistics (Channel& c, float v)
{
    c.level = v > c.level ? v : std::max (v, c.level - 1.5f);       // ~45 dB/s fall
    if (v >= c.peak) { c.peak = v; c.hold = 60; }                     // 2 s hold
    else if (--c.hold < 0) c.peak = std::max (-100.0f, c.peak - 0.75f);
}

void IoMeters::update()
{
    ballistics (in, proc.inputPeakDb.exchange (-100.0f));
    ballistics (out, proc.outputPeakDb.exchange (-100.0f));
    repaint();
}

void IoMeters::mouseDown (const juce::MouseEvent&)
{
    in.peak = in.level; out.peak = out.level;
    repaint();
}

void IoMeters::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

    auto inner = r.reduced (6.0f, 6.0f);
    auto header = inner.removeFromTop (30.0f);
    auto footer = inner.removeFromBottom (14.0f);
    inner.removeFromTop (4.0f);
    auto scale = inner.removeFromLeft (24.0f);
    const float barW = (inner.getWidth() - 6.0f) / 2.0f;
    auto inBar = inner.removeFromLeft (barW);
    inner.removeFromLeft (6.0f);
    auto outBar = inner.removeFromLeft (barW);

    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (minDb, 0.0f, db), 0.0f, minDb, inBar.getY(), inBar.getBottom()); };

    // zone colours (bottom -> top): too low, OK-quiet, TARGET, hot, clip risk
    auto zoneColour = [] (float db)
    {
        if (db < -30.0f)     return theme::input;
        if (db < targetLo)   return theme::output;
        if (db < targetHi)   return theme::good;
        if (db < warnDb)     return theme::warn;
        return theme::danger;
    };

    auto gradientFor = [&] (juce::Rectangle<float> bar)
    {
        juce::ColourGradient grad (theme::input, 0.0f, bar.getBottom(), theme::danger, 0.0f, bar.getY(), false);
        auto pos = [&] (float db) { return (double) juce::jlimit (0.0f, 1.0f, (db - minDb) / -minDb); };
        grad.addColour (pos (-30.5f), theme::input);
        grad.addColour (pos (-29.5f), theme::output);
        grad.addColour (pos (targetLo - 0.5f), theme::output);
        grad.addColour (pos (targetLo + 0.5f), theme::good);
        grad.addColour (pos (targetHi - 0.5f), theme::good);
        grad.addColour (pos (targetHi + 0.5f), theme::warn);
        grad.addColour (pos (warnDb - 0.3f), theme::warn);
        grad.addColour (pos (warnDb + 0.3f), theme::danger);
        return grad;
    };

    // target bracket behind both bars
    const float ty0 = yFor (targetHi), ty1 = yFor (targetLo);
    g.setColour (theme::good.withAlpha (0.10f));
    g.fillRect (juce::Rectangle<float> (inBar.getX() - 2.0f, ty0, outBar.getRight() - inBar.getX() + 4.0f, ty1 - ty0));

    for (auto [bar, ch, name] : { std::tuple { inBar, &in, "IN" }, std::tuple { outBar, &out, "OUT" } })
    {
        // track with faint zone tint
        g.setColour (theme::bg);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setGradientFill (gradientFor (bar));
        g.setOpacity (0.12f);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setOpacity (1.0f);

        // level
        const float y = yFor (ch->level);
        g.setGradientFill (gradientFor (bar));
        g.fillRoundedRectangle (bar.withTop (y), 2.0f);

        // peak hold
        if (ch->peak > minDb)
        {
            g.setColour (zoneColour (ch->peak));
            g.fillRect (bar.getX(), yFor (ch->peak) - 1.0f, bar.getWidth(), 2.0f);
        }

        // header: name + held peak value
        auto h = header.withX (bar.getX() - 4.0f).withWidth (bar.getWidth() + 8.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (name, h.removeFromTop (14.0f), juce::Justification::centred);
        g.setColour (ch->peak > minDb ? zoneColour (ch->peak) : theme::textFaint);
        g.setFont (theme::font (11.0f, true));
        g.drawText (ch->peak > -99.0f ? juce::String (ch->peak, 1) : "-inf", h, juce::Justification::centred);
    }

    // scale
    g.setFont (theme::font (9.5f));
    for (float db : { 0.0f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f, -30.0f, -40.0f, -50.0f, -60.0f })
    {
        const float y = yFor (db);
        const int dbi = (int) db;
        const bool keyMark = dbi == (int) targetLo || dbi == (int) targetHi || dbi == (int) warnDb;
        g.setColour (keyMark ? zoneColour (db + 0.1f) : theme::textFaint);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (scale.getX(), y - 6.0f, scale.getWidth() - 4.0f, 12.0f),
                    juce::Justification::centredRight);
    }

    g.setColour (theme::good);
    g.setFont (theme::font (9.5f, true));
    g.drawText ("TARGET", footer, juce::Justification::centred);
}

// ================================================================ panel base
ModulePanel::ModulePanel (const juce::String& t, const juce::Colour& a) : title (t), accent (a)
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

// ================================================================ history strips
void HistoryStrip::update()
{
    const int span = spanSeconds[juce::jlimit (0, 2, proc.historySpan)];
    const int columnMs = dsp::DeEsser::historyColumnMs;
    const int needed = std::min (dsp::LevelHistory::size, span * 1000 / columnMs);
    rawCols.resize ((size_t) needed);
    readPoints (rawCols.data(), needed);

    // group the stored 25 ms columns into ~2 px drawing columns (max of each group)
    const int cols = std::max (1, (getWidth() - 180) / 2);
    points.assign ((size_t) cols, { -100.0f, 0.0f });
    for (int i = 0; i < needed; ++i)
    {
        const int c = std::min (cols - 1, i * cols / needed);
        auto& d = points[(size_t) c];
        d.levelDb = std::max (d.levelDb, rawCols[(size_t) i].levelDb);
        d.grDb = std::max (d.grDb, rawCols[(size_t) i].grDb);
    }
    repaint();
}

void HistoryStrip::mouseUp (const juce::MouseEvent& e)
{
    if (spanArea.contains (e.position))
    {
        proc.historySpan = (proc.historySpan + 1) % 3;
        if (onSpanChanged) onSpanChanged();
        update();
    }
}

void HistoryStrip::paint (juce::Graphics& g)
{
    const auto cfg = getConfig();
    // a disabled module is drawn in grey, with an OFF tag over the graph
    const auto accent = cfg.on ? *cfg.accent : theme::textFaint;

    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    auto labelArea = r.removeFromLeft (118.0f);
    auto leftScale = r.removeFromLeft (28.0f);
    auto rightScale = r.removeFromRight (cfg.stacked ? 6.0f : 30.0f);
    auto area = r.reduced (2.0f, 8.0f);

    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (cfg.lo, cfg.hi, db), cfg.hi, cfg.lo, area.getY(), area.getBottom()); };
    const float grH = area.getHeight() * 0.5f;
    auto yForGr = [&] (float gr) { return area.getY() + grH * juce::jlimit (0.0f, 1.0f, gr / cfg.grMax); };

    g.setFont (theme::font (9.5f));
    const float step = (cfg.hi - cfg.lo) > 50.0f ? 12.0f : 8.0f;
    for (float db = cfg.hi; db >= cfg.lo - 0.01f; db -= step)
    {
        const float y = yFor (db);
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (theme::textFaint);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (leftScale.getX(), y - 6.0f, leftScale.getWidth() - 4.0f, 12.0f),
                    juce::Justification::centredRight);
    }

    const int n = (int) points.size();
    const float colW = area.getWidth() / (float) std::max (1, n);
    const float w = std::max (1.0f, colW - 0.5f);
    for (int i = 0; i < n; ++i)
    {
        const auto& p = points[(size_t) i];
        if (p.levelDb <= cfg.lo) continue;
        const float x = area.getX() + (float) i * colW;

        if (cfg.stacked)
        {
            // input (grey), compressed (amber), makeup stacked on top (teal)
            const float yIn = yFor (p.levelDb);
            const float comp = p.levelDb - (cfg.on ? p.grDb : 0.0f);
            const float yComp = yFor (comp);
            g.setColour (theme::input.withAlpha (cfg.on ? 0.55f : 0.25f));
            g.fillRect (x, yIn, w, area.getBottom() - yIn);
            g.setColour (accent.withAlpha (cfg.on ? 0.9f : 0.35f));
            g.fillRect (x, yComp, w, area.getBottom() - yComp);
            if (cfg.on && cfg.makeup > 0.05f)
            {
                const float yMk = yFor (comp + cfg.makeup);
                g.setColour (theme::eq.withAlpha (0.9f));
                g.fillRect (x, yMk, w, yComp - yMk);
            }
        }
        else
        {
            const bool active = cfg.on && p.grDb > 0.2f;
            const float amount = juce::jlimit (0.0f, 1.0f, p.grDb / cfg.grMax);
            const float y = yFor (p.levelDb);
            g.setColour (active ? accent.withAlpha (0.45f + 0.5f * amount) : accent.withAlpha (0.2f));
            g.fillRect (x, y, w, area.getBottom() - y);
            if (active)
            {
                g.setColour (reductionColour.withAlpha (0.75f));
                g.fillRect (x, area.getY(), w, yForGr (p.grDb) - area.getY());
            }
        }
    }

    if (! cfg.stacked)
    {
        g.setFont (theme::font (9.5f));
        const float grStep = cfg.grMax > 12.0f ? 6.0f : 4.0f;
        for (float gr = 0.0f; gr <= cfg.grMax + 0.01f; gr += grStep)
        {
            g.setColour (reductionColour.withAlpha (0.85f));
            g.drawText (gr < 0.01f ? "0" : "-" + juce::String ((int) gr),
                        juce::Rectangle<float> (rightScale.getX() + 4.0f, yForGr (gr) - 6.0f, 26.0f, 12.0f), juce::Justification::centredLeft);
        }
    }

    const float ty = yFor (cfg.threshold);
    g.setColour (cfg.on ? theme::text.withAlpha (0.75f) : theme::textFaint.withAlpha (0.5f));
    const float dash[] { 5.0f, 3.0f };
    g.drawDashedLine ({ area.getX(), ty, area.getRight(), ty }, dash, 2, 1.0f);

    if (! cfg.on)
    {
        g.setColour (theme::panel.withAlpha (0.55f));
        g.fillRect (area);
        auto tag = area.withSizeKeepingCentre (std::min (area.getWidth(), 150.0f), 24.0f);
        g.setColour (theme::panelHi);
        g.fillRoundedRectangle (tag, 5.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (tag, 5.0f, 1.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f, true));
        g.drawText (cfg.title + " OFF", tag, juce::Justification::centred);
    }

    // label column: title, threshold, (legend for the stacked view), span button
    g.setColour (accent);
    g.fillRoundedRectangle (labelArea.getX() + 10.0f, labelArea.getY() + 11.0f, 3.0f, 12.0f, 1.5f);
    auto txt = labelArea.withTrimmedLeft (20.0f);
    g.setColour (cfg.on ? theme::text : theme::textFaint);
    g.setFont (theme::font (12.5f, true));
    g.drawText (cfg.title, txt.removeFromTop (34.0f), juce::Justification::centredLeft);
    g.setFont (theme::font (10.5f));
    g.setColour (cfg.on ? theme::textDim : theme::textFaint);
    g.drawText (cfg.on ? cfg.thresholdText : "off", txt.removeFromTop (15.0f), juce::Justification::centredLeft);
    if (cfg.stacked)
    {
        for (auto [name, col] : { std::pair { "input", theme::input }, std::pair { "compressed", accent }, std::pair { "makeup", theme::eq } })
        {
            auto row = txt.removeFromTop (13.0f);
            g.setColour (col);
            g.fillRect (row.getX(), row.getCentreY() - 3.0f, 8.0f, 6.0f);
            g.setColour (theme::textDim);
            g.drawText (name, row.withTrimmedLeft (12.0f), juce::Justification::centredLeft);
        }
    }
    spanArea = juce::Rectangle<float> (labelArea.getX() + 18.0f, labelArea.getBottom() - 22.0f, 46.0f, 17.0f);
    g.setColour (theme::panelHi);
    g.fillRoundedRectangle (spanArea, 4.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (spanArea, 4.0f, 1.0f);
    g.setColour (theme::textDim);
    g.setFont (theme::font (10.5f, true));
    g.drawText (juce::String (spanSeconds[juce::jlimit (0, 2, proc.historySpan)]) + " s", spanArea, juce::Justification::centred);

    g.setColour (theme::border);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);
}

HistoryStrip::Config DeEssHistory::getConfig()
{
    Config c;
    const bool relative = raw (proc, dsRelative) > 0.5f;
    c.title = "DE-ESS";
    c.accent = &theme::deess;
    c.on = raw (proc, dsOn) > 0.5f;
    c.lo = relative ? -48.0f : -60.0f;
    c.hi = 0.0f;
    c.threshold = relative ? raw (proc, dsRelThr) : raw (proc, dsThr);
    c.thresholdText = (relative ? "rel thr " : "thr ") + juce::String (c.threshold, 0) + " dB";
    c.grMax = std::max (6.0f, raw (proc, dsRange));
    return c;
}

HistoryStrip::Config CompHistory::getConfig()
{
    Config c;
    c.title = "COMPRESSOR";
    c.accent = &theme::comp;
    c.on = raw (proc, compOn) > 0.5f;
    c.stacked = true;
    c.lo = -60.0f; c.hi = 0.0f;
    c.threshold = raw (proc, compThr);
    c.thresholdText = "thr " + juce::String (c.threshold, 0) + " dB";
    c.makeup = proc.comp.getMakeupDb();   // includes auto gain
    return c;
}

// ================================================================ compressor level meter
void CompLevelMeter::update()
{
    const float v = proc.comp.getInputLevelDb();
    const float target = v > in ? v : std::max (v, in - 1.5f);
    const float g = raw (proc, compOn) > 0.5f ? proc.comp.getGainReductionDb() : 0.0f;
    const float mk = raw (proc, compOn) > 0.5f ? proc.comp.getMakeupDb() : 0.0f;
    if (std::abs (target - in) > 0.1f || std::abs (g - gr) > 0.05f || std::abs (mk - makeup) > 0.05f)
    {
        in = target; gr = g; makeup = mk;
        repaint();
    }
}

void CompLevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto head = r.removeFromTop (14.0f);
    auto foot = r.removeFromBottom (28.0f);
    r.reduce (0.0f, 6.0f);
    auto scale = r.removeFromRight (22.0f);
    const float bw = (r.getWidth() - 4.0f) / 2.0f;
    auto inBar = r.removeFromLeft (bw);
    r.removeFromLeft (4.0f);
    auto outBar = r.removeFromLeft (bw);
    constexpr float lo = -60.0f;
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (lo, 0.0f, db), 0.0f, lo, inBar.getY(), inBar.getBottom()); };

    g.setFont (theme::font (9.5f, true));
    g.setColour (theme::textDim);
    g.drawText ("IN", head.withX (inBar.getX() - 4.0f).withWidth (bw + 8.0f), juce::Justification::centred);
    g.drawText ("OUT", head.withX (outBar.getX() - 6.0f).withWidth (bw + 12.0f), juce::Justification::centred);

    for (auto bar : { inBar, outBar }) { g.setColour (theme::bg); g.fillRoundedRectangle (bar, 2.0f); }

    g.setColour (theme::input);
    g.fillRect (inBar.withTop (yFor (in)));

    const float comp = in - gr;
    g.setColour (theme::comp);
    g.fillRect (outBar.withTop (yFor (comp)));
    if (makeup > 0.05f)
    {
        g.setColour (theme::eq);
        const float yTop = yFor (comp + makeup), yBot = yFor (comp);
        g.fillRect (juce::Rectangle<float> (outBar.getX(), yTop, outBar.getWidth(), yBot - yTop));
    }
    // threshold tick across both bars
    g.setColour (theme::text.withAlpha (0.7f));
    g.fillRect (inBar.getX(), yFor (raw (proc, compThr)) - 0.5f, outBar.getRight() - inBar.getX(), 1.0f);

    g.setFont (theme::font (9.0f));
    g.setColour (theme::textFaint);
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (scale.getX() + 2.0f, yFor (db) - 6.0f, 20.0f, 12.0f), juce::Justification::centredLeft);

    g.setFont (theme::font (10.0f, true));
    g.setColour (gr > 0.2f ? reductionColour : theme::textDim);
    g.drawText ("GR " + juce::String (-gr, 1), foot.removeFromTop (14.0f), juce::Justification::centred);
    g.setColour (makeup > 0.05f ? theme::eq : theme::textFaint);
    g.drawText ((raw (proc, compAutoGain) > 0.5f ? "AUTO +" : "MU +") + juce::String (makeup, 1), foot, juce::Justification::centred);
}

// ================================================================ compressor graph
void CompGraph::update()
{
    const float v = proc.comp.getInputLevelDb();
    inDb = v > inDb ? v : std::max (v, inDb - 1.2f);
    grDb = proc.comp.getGainReductionDb();
    repaint();
}

void CompGraph::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().toFloat();
    const bool on = raw (proc, compOn) > 0.5f;
    const float thr = raw (proc, compThr), ratio = raw (proc, compRatio), knee = raw (proc, compKnee);

    auto plot = r.reduced (8.0f).withTrimmedLeft (22.0f).withTrimmedBottom (12.0f);
    plot = plot.withWidth (std::min (plot.getWidth() - 84.0f, plot.getHeight()));
    auto info = juce::Rectangle<float> (plot.getRight() + 10.0f, r.getY() + 6.0f, r.getRight() - plot.getRight() - 14.0f, r.getHeight() - 12.0f);
    constexpr float lo = -60.0f;
    auto xFor = [&] (float db) { return juce::jmap (juce::jlimit (lo, 0.0f, db), lo, 0.0f, plot.getX(), plot.getRight()); };
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (lo, 0.0f, db), 0.0f, lo, plot.getY(), plot.getBottom()); };

    g.setColour (theme::bg);
    g.fillRect (plot);
    g.setFont (theme::font (9.0f));
    for (float db = 0.0f; db >= lo; db -= 20.0f)
    {
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());
        g.drawVerticalLine ((int) xFor (db), plot.getY(), plot.getBottom());
        g.setColour (theme::textFaint);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 24.0f, yFor (db) - 6.0f, 20.0f, 12.0f), juce::Justification::centredRight);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (xFor (db) - 12.0f, plot.getBottom() + 1.0f, 24.0f, 11.0f), juce::Justification::centred);
    }

    // unity line and transfer curve
    g.setColour (theme::textFaint.withAlpha (0.6f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);
    juce::Path curve;
    for (int i = 0; i <= 60; ++i)
    {
        const float x = lo + (float) i;
        const float y = x - dsp::VocalCompressor::computeGainReduction (x, thr, ratio, knee);
        if (i == 0) curve.startNewSubPath (xFor (x), yFor (y)); else curve.lineTo (xFor (x), yFor (y));
    }
    g.setColour (on ? theme::comp : theme::textDim);
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    // threshold marker
    g.setColour (theme::comp.withAlpha (0.35f));
    g.drawVerticalLine ((int) xFor (thr), plot.getY(), plot.getBottom());

    // live input dot (riding the curve, using the real gain reduction)
    if (inDb > lo)
    {
        const float outDb = inDb - (on ? grDb : 0.0f);
        const float dx = xFor (inDb), dy = yFor (outDb);
        g.setColour (theme::comp.withAlpha (0.25f));
        g.fillRect (plot.getX(), dy - 0.5f, dx - plot.getX(), 1.0f);
        g.setColour (on ? theme::comp : theme::textDim);
        g.fillEllipse (dx - 4.5f, dy - 4.5f, 9.0f, 9.0f);
        g.setColour (theme::panel);
        g.drawEllipse (dx - 4.5f, dy - 4.5f, 9.0f, 9.0f, 1.5f);
    }

    // readouts
    g.setColour (theme::text);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("COMP CURVE", info.removeFromTop (16.0f), juce::Justification::centredLeft);
    auto line = [&] (const juce::String& label, const juce::String& value, juce::Colour c)
    {
        auto row = info.removeFromTop (30.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f));
        g.drawText (label, row.removeFromTop (12.0f), juce::Justification::centredLeft);
        g.setColour (c);
        g.setFont (theme::font (14.0f, true));
        g.drawText (value, row, juce::Justification::centredLeft);
    };
    line ("input", inDb > -99.0f ? juce::String (inDb, 1) + " dB" : "-inf", theme::text);
    line ("gain reduction", on ? juce::String (-grDb, 1) + " dB" : "off", on && grDb > 0.2f ? reductionColour : theme::textDim);
    line ("ratio", juce::String (ratio, 1) + ":1", theme::textDim);

    g.setColour (theme::border);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);
}

// ================================================================ PSE key meter
void PseKeyMeter::update()
{
    const float k = proc.pse.getKeyLevelDb();
    key = k > key ? k : std::max (k, key - 1.5f);
    const bool on = raw (proc, pseOn) > 0.5f;
    const int st = ! on ? 0 : (proc.pse.getIsOpen() ? 1 : 2);
    const float gr = proc.pse.getGainReductionDb();
    if (std::abs (key - paintedKey) > 0.25f || st != paintedState || std::abs (gr - paintedGr) > 0.1f)
    {
        paintedKey = key; paintedState = st; paintedGr = gr;
        repaint();
    }
}

void PseKeyMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool on = raw (proc, pseOn) > 0.5f;
    const bool open = ! on || proc.pse.getIsOpen();
    const float thr = raw (proc, pseThr);

    auto top = r.removeFromTop (16.0f);
    const auto lampCol = ! on ? theme::textFaint : (open ? theme::pse : theme::warn);
    g.setColour (lampCol);
    g.fillEllipse (top.getX() + 1.0f, top.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setFont (theme::font (11.0f, true));
    g.drawText (! on ? "OFF" : (open ? "OPEN" : "CLOSED  -" + juce::String (proc.pse.getGainReductionDb(), 1) + " dB"),
                top.withTrimmedLeft (14.0f), juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.0f));
    g.drawText ("key level", top, juce::Justification::centredRight);

    auto bar = r.withTrimmedTop (3.0f).withHeight (8.0f);
    auto toX = [&] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };
    g.setColour (theme::bg);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour ((key >= thr ? theme::pse : theme::textDim).withAlpha (0.85f));
    g.fillRoundedRectangle (bar.withRight (toX (key)), 2.0f);
    g.setColour (theme::text);
    g.fillRect (juce::Rectangle<float> (toX (thr) - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f));
    g.setColour (theme::textFaint);
    g.fillRect (juce::Rectangle<float> (toX (thr - dsp::PrimarySourceExpander::hysteresisDb) - 0.5f, bar.getY(), 1.0f, bar.getHeight()));

    // scale under the bar
    g.setFont (theme::font (9.0f));
    for (float db : { -80.0f, -60.0f, -40.0f, -20.0f, 0.0f })
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (toX (db) - 14.0f, bar.getBottom() + 1.0f, 28.0f, 10.0f),
                    db <= -80.0f ? juce::Justification::centredLeft : (db >= 0.0f ? juce::Justification::centredRight : juce::Justification::centred));
}

// ================================================================ dynamic EQ activity
void DynActivity::update()
{
    const float l = proc.dynEq.getDetectorLevelDb (band);
    level = l > level ? l : std::max (l, level - 1.5f);
    gain = proc.dynEq.getDynamicGainDb (band);
    if (std::abs (level - paintedLevel) > 0.25f || std::abs (gain - paintedGain) > 0.05f)
    {
        paintedLevel = level; paintedGain = gain;
        repaint();
    }
}

void DynActivity::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f, 2.0f);
    const bool dyn = raw (proc, bDyn (band)) > 0.5f && raw (proc, bOn (band)) > 0.5f
                     && ! isPassType (bandTypeFromChoice (band, (band == 0 || band == numEqBands - 1) ? (int) raw (proc, bType (band)) : 0));
    const auto col = theme::bandColours[band];
    const float thr = raw (proc, bThr (band)), range = raw (proc, bRange (band));

    g.setFont (theme::font (10.5f, true));
    g.setColour (theme::textDim);
    g.drawText ("BAND LEVEL vs THRESHOLD", r.removeFromTop (13.0f), juce::Justification::centredLeft);

    auto bar1 = r.removeFromTop (8.0f);
    auto toX = [&] (float db) { return bar1.getX() + bar1.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    g.setColour (theme::bg);
    g.fillRoundedRectangle (bar1, 2.0f);
    if (dyn)
    {
        g.setColour (col.withAlpha (level > thr ? 0.95f : 0.5f));
        g.fillRoundedRectangle (bar1.withRight (toX (level)), 2.0f);
    }
    g.setColour (theme::text);
    g.fillRect (toX (thr) - 1.0f, bar1.getY() - 3.0f, 2.0f, bar1.getHeight() + 6.0f);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (9.0f));
    auto scaleRow = r.removeFromTop (11.0f);
    for (float db : { -60.0f, -40.0f, -20.0f, 0.0f })
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (toX (db) - 14.0f, scaleRow.getY(), 28.0f, 10.0f),
                    db <= -60.0f ? juce::Justification::centredLeft : (db >= 0.0f ? juce::Justification::centredRight : juce::Justification::centred));

    r.removeFromTop (4.0f);
    g.setFont (theme::font (10.5f, true));
    g.setColour (theme::textDim);
    auto head = r.removeFromTop (13.0f);
    g.drawText ("DYNAMIC GAIN", head, juce::Justification::centredLeft);
    g.setColour (dyn ? col : theme::textFaint);
    g.drawText (dyn ? dbText (gain) + " dB" : (raw (proc, bDyn (band)) > 0.5f ? "-" : "static"), head, juce::Justification::centredRight);

    // bipolar bar: centre = 0 dB, ends = +/-|range|
    auto bar2 = r.removeFromTop (8.0f);
    const float span = std::max (1.0f, std::abs (range));
    const float cx = bar2.getCentreX();
    g.setColour (theme::bg);
    g.fillRoundedRectangle (bar2, 2.0f);
    if (dyn)
    {
        const float px = cx + bar2.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, gain / span);
        g.setColour (col);
        g.fillRect (juce::Rectangle<float> (std::min (cx, px), bar2.getY(), std::abs (px - cx), bar2.getHeight()));
    }
    g.setColour (theme::textFaint);
    g.fillRect (cx - 0.5f, bar2.getY() - 2.0f, 1.0f, bar2.getHeight() + 4.0f);
    g.setFont (theme::font (9.0f));
    auto s2 = r.removeFromTop (11.0f);
    g.drawText ("-" + juce::String (span, 0), s2, juce::Justification::centredLeft);
    g.drawText ("0", s2, juce::Justification::centred);
    g.drawText ("+" + juce::String (span, 0), s2, juce::Justification::centredRight);
}

// ================================================================ suggestion bar
SuggestionBar::SuggestionBar (LiveVoiceProcessor& p, const juce::Colour& a) : proc (p), accent (a)
{
    keep.setTooltip ("Keep the suggested settings");
    revert.setTooltip ("Put back exactly the settings you had before Suggest");
    keep.onClick = [this] { clear(); };
    revert.onClick = [this]
    {
        proc.undoManager.beginNewTransaction ("Revert suggestion");
        for (auto& [id, v] : previous) setReal (proc, id, v);
        clear();
    };
    addChildComponent (keep);
    addChildComponent (revert);
}

void SuggestionBar::show (const juce::String& text, std::vector<std::pair<juce::String, float>> before)
{
    message = text;
    previous = std::move (before);
    keep.setVisible (true);
    revert.setVisible (true);
    resized();
    repaint();
}

void SuggestionBar::showMessage (const juce::String& text)
{
    message = text;
    previous.clear();
    keep.setVisible (false);
    revert.setVisible (false);
    repaint();
}

void SuggestionBar::clear()
{
    message.clear();
    previous.clear();
    keep.setVisible (false);
    revert.setVisible (false);
    repaint();
}

void SuggestionBar::resized()
{
    auto r = getLocalBounds();
    if (r.getHeight() >= 60)   // stacked: text above, buttons below
    {
        auto row = r.removeFromBottom (22);
        keep.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (2));
        revert.setBounds (row.withTrimmedLeft (2));
    }
    else                       // one line: text, then buttons on the right
    {
        revert.setBounds (r.removeFromRight (64).reduced (0, 1));
        r.removeFromRight (4);
        keep.setBounds (r.removeFromRight (52).reduced (0, 1));
    }
}

void SuggestionBar::paint (juce::Graphics& g)
{
    if (message.isEmpty()) return;
    auto r = getLocalBounds().toFloat();
    const bool stacked = r.getHeight() >= 60.0f;
    auto text = stacked ? r.withTrimmedBottom (26.0f) : r.withTrimmedRight (keep.isVisible() ? 124.0f : 0.0f);
    g.setColour (accent);
    g.setFont (theme::font (10.5f, true));
    g.drawFittedText (message, text.toNearestInt(), stacked ? juce::Justification::topLeft : juce::Justification::centredRight, stacked ? 5 : 1, 0.9f);
}

// ================================================================ feedback panel
FeedbackPanel::FeedbackPanel (LiveVoiceProcessor& p) : ModulePanel ("FEEDBACK SUPPRESSION", theme::feedback), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, fbOn);

    mode.attach (s, fbMode, { "Speech", "Singing", "Multipurpose" });
    addAndMakeVisible (mode);
    modeLabel.setText ("Mode", juce::dontSendNotification);
    modeLabel.setFont (theme::font (11.5f));
    modeLabel.setColour (juce::Label::textColourId, theme::textDim);
    addAndMakeVisible (modeLabel);

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
    for (auto* k : { &sens, &depth, &hold }) addAndMakeVisible (k);

    status.setFont (theme::font (12.0f));
    status.setColour (juce::Label::textColourId, theme::textDim);
    status.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (status);

    logBtn.setTooltip ("Feedback log: every frequency that rang this session, with times. Useful afterwards for monitor placement and room EQ.");
    logBtn.onClick = [this] { showLog(); };
    addAndMakeVisible (logBtn);
}

namespace
{
    juce::String freqText (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz"; }

    class LogView : public juce::Component
    {
    public:
        explicit LogView (LiveVoiceProcessor& p) : proc (p)
        {
            text.setMultiLine (true);
            text.setReadOnly (true);
            text.setScrollbarsShown (true);
            text.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain)));
            addAndMakeVisible (text);
            copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (text.getText()); };
            clear.onClick = [this] { proc.clearFeedbackLog(); refresh(); };
            addAndMakeVisible (copy);
            addAndMakeVisible (clear);
            setSize (420, 360);
            refresh();
        }
        void refresh()
        {
            const auto& log = proc.getFeedbackLog();
            juce::String s;
            if (log.empty()) s = "No feedback detected yet.";
            else
            {
                // most frequent rings (grouped within 1/12 octave)
                std::vector<std::pair<float, int>> groups;
                for (auto& e : log)
                {
                    bool found = false;
                    for (auto& gr : groups)
                        if (std::abs (std::log2 (e.freq / gr.first)) < 1.0f / 12.0f) { ++gr.second; found = true; break; }
                    if (! found) groups.push_back ({ e.freq, 1 });
                }
                std::sort (groups.begin(), groups.end(), [] (auto& a, auto& b) { return a.second > b.second; });
                s << "MOST FREQUENT\n";
                for (size_t i = 0; i < std::min<size_t> (5, groups.size()); ++i)
                    s << "  " << freqText (groups[i].first).paddedRight (' ', 10) << groups[i].second << "x\n";
                s << "\nALL EVENTS (" << (int) log.size() << ")\n";
                for (auto it = log.rbegin(); it != log.rend(); ++it)
                    s << "  " << it->time.formatted ("%H:%M:%S") << "   " << freqText (it->freq).paddedRight (' ', 10)
                      << (it->fixed ? "ring-out (fixed)" : "floating") << "\n";
            }
            text.setText (s, false);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (8);
            auto row = r.removeFromBottom (26);
            clear.setBounds (row.removeFromRight (70));
            row.removeFromRight (6);
            copy.setBounds (row.removeFromRight (70));
            r.removeFromBottom (6);
            text.setBounds (r);
        }
    private:
        LiveVoiceProcessor& proc;
        juce::TextEditor text;
        juce::TextButton copy { "COPY" }, clear { "CLEAR" };
    };
}

void FeedbackPanel::showLog()
{
    proc.pollFeedbackLog();
    juce::CallOutBox::launchAsynchronously (std::make_unique<LogView> (proc), logBtn.getScreenBounds(), nullptr);
}

void FeedbackPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();

    auto row = r.removeFromTop (26);
    modeLabel.setBounds (row.removeFromLeft (44));
    mode.setBounds (row);

    r.removeFromTop (8);
    row = r.removeFromTop (28);
    learn.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (3));
    freeze.setBounds (row.withTrimmedLeft (3));
    r.removeFromTop (6);
    row = r.removeFromTop (28);
    clearFloat.setBounds (row.removeFromLeft (row.getWidth() / 2).withTrimmedRight (3));
    clearAll.setBounds (row.withTrimmedLeft (3));

    auto statusRow = r.removeFromBottom (22);
    logBtn.setBounds (statusRow.removeFromRight (56));
    status.setBounds (statusRow);
    r.removeFromBottom (6);
    r.removeFromTop (12);

    const int knobH = std::min (104, r.getHeight());
    row = r.removeFromTop (knobH);
    const int kw = row.getWidth() / 3;
    for (auto* k : { &sens, &depth, &hold }) k->setBounds (row.removeFromLeft (kw));
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
    juce::String txt = "Fixed " + juce::String (fixed) + "/" + juce::String ((int) raw (proc, fbMaxFixed))
                     + "   Floating " + juce::String (floating) + "/" + juce::String ((int) raw (proc, fbMaxFloat));
    if (raw (proc, fbLearn) > 0.5f) txt << "   LEARNING";
    else if (raw (proc, fbFreeze) > 0.5f) txt << "   FROZEN";
    status.setText (txt, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, raw (proc, fbLearn) > 0.5f ? theme::feedback : theme::textDim);
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
        btn.setColour (juce::TextButton::textColourOnId, theme::onAccent);
        btn.setTooltip ("Band " + juce::String (b + 1) + " - typical role: " + bandRoleNames[b]);
        btn.onClick = [this, b] { selectBand (b); if (onBandSelected) onBandSelected (b); };
        addAndMakeVisible (btn);
    }

    solo.setTooltip ("Solo: hear only this band's frequency region on output 2 (Monitor). Output 1 (FOH) is not affected. "
                     "Not saved with the session.");
    solo.onClick = [this]
    {
        proc.soloTarget = solo.getToggleState() ? band : LiveVoiceProcessor::soloNone;
    };

    suggest.setColour (juce::TextButton::buttonOnColourId, theme::eq);
    suggest.setColour (juce::TextButton::textColourOnId, theme::onAccent);
    suggest.setTooltip ("Suggest: press while the pastor talks normally. After 20 s LiveVoice suggests a high-pass and gentle cuts "
                        "where this voice builds up (boom, box, nasal, harsh). Static gains only. KEEP or REVERT appears next to the result.");
    suggest.onClick = [this] { clickSuggest (proc, LiveVoiceProcessor::CaptureTarget::eq); suggestion.clear(); };

    for (auto* c : { (juce::Component*) &bandOn, (juce::Component*) &dynamic, (juce::Component*) &type,
                     (juce::Component*) &solo, (juce::Component*) &activity, (juce::Component*) &suggest, (juce::Component*) &suggestion })
        addAndMakeVisible (c);
    for (auto* k : { &freq, &gain, &q, &thr, &range, &ratio, &attack, &release }) addAndMakeVisible (k);

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
    if (band == 0)                    type.attach (s, bType (band), band1Choices);
    else if (band == numEqBands - 1)  type.attach (s, bType (band), band6Choices);
    else { type.attachment.reset(); type.clear (juce::dontSendNotification); type.addItem ("Bell", 1); type.setSelectedId (1, juce::dontSendNotification); }
    type.setEnabled (band == 0 || band == numEqBands - 1);

    freq.attach (s, bFreq (band)); gain.attach (s, bGain (band)); q.attach (s, bQ (band));
    thr.attach (s, bThr (band)); range.attach (s, bRange (band)); ratio.attach (s, bRatio (band));
    attack.attach (s, bAtt (band)); release.attach (s, bRel (band));

    for (auto* k : { &freq, &gain, &q, &thr, &range, &ratio, &attack, &release })
        k->slider.setColour (juce::Slider::rotarySliderFillColourId, theme::bandColours[band]);

    // an active EQ solo follows the selected band
    const int st = proc.soloTarget.load();
    if (juce::isPositiveAndBelow (st, numEqBands) && st != band)
        proc.soloTarget = band;

    activity.setBand (band);
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
    bandOn.setBounds (row.removeFromLeft (76).withTrimmedRight (5));
    type.setBounds (row.removeFromLeft (134).withTrimmedRight (5));
    dynamic.setBounds (row.removeFromLeft (78).withTrimmedRight (5));
    solo.setBounds (row.removeFromLeft (56).withTrimmedRight (5));
    suggest.setBounds (row);
    suggestion.setBounds (110, 6, getWidth() - 172, 22);

    r.removeFromTop (8);
    const int knobH = std::min (96, (r.getHeight() - 12) / 2);
    row = r.removeFromTop (knobH);
    const int kw = row.getWidth() / 5;
    for (auto* k : { &freq, &gain, &q }) k->setBounds (row.removeFromLeft (kw));
    activity.setBounds (row.reduced (4, 2));
    r.removeFromTop (12);
    row = r.removeFromTop (knobH);
    for (auto* k : { &thr, &range, &ratio, &attack, &release }) k->setBounds (row.removeFromLeft (kw));
}

void EqPanel::update()
{
    for (int b = 0; b < numEqBands; ++b)
    {
        const auto shape = bandShapeFromChoice (b, (b == 0 || b == numEqBands - 1) ? (int) raw (proc, bType (b)) : 0);
        juce::String txt (b + 1);
        switch (shape.type)
        {
            case BandType::lowShelf:  txt << " LS"; break;
            case BandType::highShelf: txt << " HS"; break;
            case BandType::highPass:  txt << " HP" << shape.slope; break;
            case BandType::lowPass:   txt << " LP" << shape.slope; break;
            case BandType::bell: default: break;
        }
        if (raw (proc, bDyn (b)) > 0.5f && ! isPassType (shape.type)) txt << " *";
        auto& btn = bandButtons[(size_t) b];
        if (btn.getButtonText() != txt) btn.setButtonText (txt);
        btn.setAlpha (raw (proc, bOn (b)) > 0.5f ? 1.0f : 0.45f);
    }

    const auto shape = bandShapeFromChoice (band, (band == 0 || band == numEqBands - 1) ? (int) raw (proc, bType (band)) : 0);
    const bool pass = isPassType (shape.type);
    const bool dyn = raw (proc, bDyn (band)) > 0.5f && ! pass;
    gain.setEnabled (! pass);
    q.setEnabled (! pass || shape.slope == 12);
    dynamic.setEnabled (! pass);
    solo.setEnabled (! pass);
    for (auto* k : { &thr, &range, &ratio, &attack, &release }) k->setEnabled (dyn);

    const bool soloed = proc.soloTarget.load() == band && ! pass;
    if (solo.getToggleState() != soloed) solo.setToggleState (soloed, juce::dontSendNotification);

    activity.update();

    if (driveSuggestButton (proc, suggest, LiveVoiceProcessor::CaptureTarget::eq))
        runSuggestion();
}

void EqPanel::runSuggestion()
{
    const auto audio = proc.takeCapture();
    const auto s = dsp::analysis::suggestEq (audio.data(), (int) audio.size(), proc.currentSampleRate.load());
    if (! s.ok)
    {
        suggestion.showMessage ("Not enough voice heard - try again while someone talks");
        return;
    }

    // remember exactly what was there, so REVERT can put it back
    std::vector<std::pair<juce::String, float>> before;
    auto remember = [&] (const juce::String& id) { before.emplace_back (id, raw (proc, id)); };
    for (int b = 0; b < numEqBands; ++b) { remember (bOn (b)); remember (bFreq (b)); remember (bGain (b)); remember (bQ (b)); }
    remember (bType (numEqBands - 1)); remember (params::hpfOn); remember (params::hpfFreq);

    proc.undoManager.beginNewTransaction ("EQ suggest");
    setReal (proc, params::hpfOn, 1.0f); setReal (proc, params::hpfFreq, s.hpFreq);
    juce::String txt = "HPF " + juce::String (juce::roundToInt (s.hpFreq));
    for (int i = 0; i < 4; ++i)
    {
        const int b = i + 1;
        setReal (proc, bOn (b), 1.0f);
        setReal (proc, bFreq (b), s.bands[i].freq);
        setReal (proc, bGain (b), s.bands[i].gainDb);
        setReal (proc, bQ (b), s.bands[i].q);
        if (s.bands[i].gainDb < -0.05f)
            txt << ",  " << (s.bands[i].freq >= 1000.0f ? juce::String (s.bands[i].freq / 1000.0f, 1) + "k" : juce::String (juce::roundToInt (s.bands[i].freq)))
                << " " << juce::String (s.bands[i].gainDb, 1);
    }
    setReal (proc, bOn (5), 1.0f); setReal (proc, bType (5), 1.0f); setReal (proc, bGain (5), s.airGainDb);
    if (s.airGainDb > 0.05f) txt << ",  air +" << juce::String (s.airGainDb, 1);
    suggestion.show ("Suggested: " + txt, std::move (before));
}

// ================================================================ de-esser panel
DeEssPanel::DeEssPanel (LiveVoiceProcessor& p) : ModulePanel ("DE-ESSER", theme::deess), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, dsOn);
    mode.attach (s, dsMode, { "Band (narrow)", "Shelf (all highs)", "Wideband" });
    mode.setTooltip ("Band: cuts only the sibilance band (set by Freq and Width). Shelf: turns down all the highs. Wideband: ducks the whole voice.");
    addAndMakeVisible (mode);
    relative.attach (s, dsRelative);
    relative.setTooltip ("Relative detection: compares the sibilance with the whole voice instead of a fixed level, "
                         "so a soft talker and a loud talker are de-essed the same. Off = classic absolute threshold.");
    addAndMakeVisible (relative);
    solo.setTooltip ("Solo: hear exactly what the de-esser listens to on output 2 (Monitor), to tune Freq and Width onto the 's'. "
                     "Output 1 (FOH) is not affected. Not saved with the session.");
    solo.onClick = [this] { proc.soloTarget = solo.getToggleState() ? LiveVoiceProcessor::soloDeEss : LiveVoiceProcessor::soloNone; };
    addAndMakeVisible (solo);

    suggest.setColour (juce::TextButton::buttonOnColourId, theme::deess);
    suggest.setColour (juce::TextButton::textColourOnId, theme::onAccent);
    suggest.setTooltip ("Suggest: press while the pastor talks normally. After 20 s LiveVoice sets Freq, Width and Threshold "
                        "for this voice. Press again to cancel. KEEP or REVERT appears next to the result.");
    suggest.onClick = [this] { clickSuggest (proc, LiveVoiceProcessor::CaptureTarget::deEss); suggestion.clear(); };
    addAndMakeVisible (suggest);
    addAndMakeVisible (suggestion);

    freq.attach (s, dsFreq); width.attach (s, dsQ); range.attach (s, dsRange); rel.attach (s, dsRel);
    bindThreshold();
    for (auto* k : { &freq, &width, &thr, &range, &rel }) addAndMakeVisible (k);
    addAndMakeVisible (grMeter);
    for (auto* l : { &grLabel, &grValue })
    {
        l->setFont (theme::font (11.0f));
        l->setColour (juce::Label::textColourId, theme::textDim);
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }
    grLabel.setText ("GR dB", juce::dontSendNotification);
}

void DeEssPanel::bindThreshold()
{
    const int isRel = raw (proc, dsRelative) > 0.5f ? 1 : 0;
    if (isRel == boundRelative) return;
    boundRelative = isRel;
    thr.attach (proc.apvts, isRel ? dsRelThr : dsThr);
    thr.label.setText (isRel ? "Rel. Threshold" : "Threshold", juce::dontSendNotification);
}

void DeEssPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();
    auto meterCol = r.removeFromRight (34);
    grLabel.setBounds (meterCol.removeFromTop (16));
    grValue.setBounds (meterCol.removeFromBottom (16));
    grMeter.setBounds (meterCol.reduced (2, 4));
    r.removeFromRight (4);

    auto row = r.removeFromTop (26);
    mode.setBounds (row.removeFromLeft (row.getWidth() * 3 / 5).withTrimmedRight (4));
    suggest.setBounds (row);
    r.removeFromTop (6);
    row = r.removeFromTop (26);
    relative.setBounds (row.removeFromLeft (row.getWidth() * 3 / 5).withTrimmedRight (4));
    solo.setBounds (row);
    r.removeFromTop (8);

    const int knobH = std::min (92, (r.getHeight() - 16) / 3);
    const int kw = r.getWidth() / 2;
    row = r.removeFromTop (knobH);
    freq.setBounds (row.removeFromLeft (kw)); width.setBounds (row);
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    thr.setBounds (row.removeFromLeft (kw)); range.setBounds (row);
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    rel.setBounds (row.removeFromLeft (kw));
    suggestion.setBounds (row.reduced (2, 2));
}

void DeEssPanel::update()
{
    bindThreshold();
    const float gr = proc.deEsser.getCurrentGainReductionDb();
    grMeter.setValue (gr);
    grValue.setText (juce::String (-gr, 1), juce::dontSendNotification);
    const bool soloed = proc.soloTarget.load() == LiveVoiceProcessor::soloDeEss;
    if (solo.getToggleState() != soloed) solo.setToggleState (soloed, juce::dontSendNotification);
    width.setEnabled ((int) raw (proc, dsMode) == (int) DsMode::band);

    if (driveSuggestButton (proc, suggest, LiveVoiceProcessor::CaptureTarget::deEss))
        runSuggestion();
}

void DeEssPanel::runSuggestion()
{
    const auto audio = proc.takeCapture();
    const auto s = dsp::analysis::suggestDeEss (audio.data(), (int) audio.size(), proc.currentSampleRate.load());
    if (! s.ok)
    {
        suggestion.showMessage ("Not enough \"s\" sounds heard. Try again while someone talks.");
        return;
    }
    std::vector<std::pair<juce::String, float>> before;
    for (auto* id : { &dsMode, &dsFreq, &dsQ, &dsThr, &dsRelThr })
        before.emplace_back (*id, raw (proc, *id));

    proc.undoManager.beginNewTransaction ("De-esser suggest");
    setReal (proc, dsMode, (float) DsMode::band);
    setReal (proc, dsFreq, s.freq);
    setReal (proc, dsQ, s.q);
    setReal (proc, dsThr, s.thresholdDb);
    setReal (proc, dsRelThr, s.relThresholdDb);
    const bool relMode = raw (proc, dsRelative) > 0.5f;
    const int thrShown = juce::roundToInt (relMode ? s.relThresholdDb : s.thresholdDb);
    suggestion.show ("Suggested: " + juce::String (s.freq / 1000.0f, 1) + " kHz, Q " + juce::String (s.q, 1)
                     + ", " + (relMode ? "rel thr " : "thr ") + juce::String (thrShown) + " dB", std::move (before));
}

// ================================================================ high-pass + PSE panel
PsePanel::PsePanel (LiveVoiceProcessor& p) : ModulePanel ("HIGH-PASS", theme::hpf), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, params::hpfOn);
    power.setTooltip ("High-pass filter, first in the chain: removes rumble, stage thump and handling noise before the PSE, "
                      "the feedback detector and everything else hear it. Also on the RTA as the HP handle.");
    hpfFreq.attach (s, params::hpfFreq);
    hpfSlope.attach (s, params::hpfSlope, params::hpfSlopeChoices);
    hpfSlope.setTooltip ("Steepness. 12-18 dB/oct sounds most natural; 24 dB/oct is a good live default for face and lapel mics.");
    slopeLabel.setText ("Slope", juce::dontSendNotification);
    slopeLabel.setFont (theme::font (11.5f));
    slopeLabel.setColour (juce::Label::textColourId, theme::textDim);
    slopeLabel.setJustificationType (juce::Justification::centred);
    for (auto* c : { (juce::Component*) &hpfFreq, (juce::Component*) &hpfSlope, (juce::Component*) &slopeLabel }) addAndMakeVisible (c);

    pseOn.attach (s, params::pseOn);
    pseOn.setTooltip ("Primary Source Expander on/off.");
    addAndMakeVisible (pseOn);
    keyMeter.setTooltip ("Primary Source Expander: smoothly turns the mic down between phrases, cutting stage bleed and room pickup "
                         "and lowering the chance of feedback while the mic is idle. Set the threshold just below the softest speech.");
    addAndMakeVisible (keyMeter);
    thr.attach (s, pseThr); range.attach (s, pseRange); attack.attach (s, pseAttack);
    hold.attach (s, pseHold); release.attach (s, pseRelease);
    for (auto* k : { &thr, &range, &attack, &hold, &release }) addAndMakeVisible (k);
}

void PsePanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();
    const int knobH = std::min (92, (r.getHeight() - 104) / 3);
    auto row = r.removeFromTop (knobH);
    const int half = row.getWidth() / 2;
    hpfFreq.setBounds (row.removeFromLeft (half));
    slopeLabel.setBounds (row.removeFromTop (16).withTrimmedTop (2));
    hpfSlope.setBounds (row.removeFromTop (26).reduced (4, 0));

    r.removeFromTop (10);
    pseY = r.getY();
    r.removeFromTop (8);
    row = r.removeFromTop (26);
    pseOn.setBounds (row.removeFromRight (42).withSizeKeepingCentre (42, 20).translated (-2, 0));
    r.removeFromTop (6);
    keyMeter.setBounds (r.removeFromTop (38));
    r.removeFromTop (6);
    row = r.removeFromTop (knobH);
    const int kw = row.getWidth() / 3;
    for (auto* k : { &thr, &range, &attack }) k->setBounds (row.removeFromLeft (kw));
    r.removeFromTop (6);
    row = r.removeFromTop (knobH).withTrimmedLeft (kw / 2);
    for (auto* k : { &hold, &release }) k->setBounds (row.removeFromLeft (kw));
}

void PsePanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);
    auto b = getContentBounds();
    g.setColour (theme::border);
    g.drawHorizontalLine (pseY, (float) b.getX(), (float) b.getRight());
    g.setColour (theme::pse);   // the PSE section keeps its own green; the panel accent is the high-pass colour
    g.fillRoundedRectangle ((float) b.getX(), (float) pseY + 15.0f, 3.0f, 12.0f, 1.5f);
    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText ("PSE", b.getX() + 10, pseY + 8, 120, 26, juce::Justification::centredLeft);
}

void PsePanel::update()
{
    keyMeter.update();
}

// ================================================================ leveler + tone
void LevelerMeter::update()
{
    const bool o = raw (proc, lvlOn) > 0.5f;
    const float gdb = proc.leveler.getGainDb();
    const bool h = proc.leveler.isHoldingBoost();
    if (o != on || h != holding || std::abs (gdb - painted) > 0.05f)
    {
        on = o; gain = gdb; painted = gdb; holding = h;
        repaint();
    }
}

void LevelerMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto top = r.removeFromTop (14.0f);
    g.setFont (theme::font (9.5f, true));
    g.setColour (theme::textDim);
    g.drawText ("RIDING", top, juce::Justification::centredLeft);
    g.setFont (theme::font (11.5f, true));
    g.setColour (on ? theme::output : theme::textFaint);
    g.drawText (on ? (gain >= 0.05f ? "+" : "") + juce::String (gain, 1) + " dB" : "off", top, juce::Justification::centredRight);
    if (holding)
    {
        g.setColour (theme::feedback);
        g.setFont (theme::font (9.5f, true));
        g.drawText ("HOLD (FEEDBACK)", top.withTrimmedLeft (44.0f), juce::Justification::centredLeft);
    }

    auto bar = r.removeFromTop (10.0f).reduced (0.0f, 1.0f);
    constexpr float span = 12.0f;
    g.setColour (theme::bg);
    g.fillRoundedRectangle (bar, 3.0f);
    const float cx = bar.getCentreX();
    if (on)
    {
        const float x = juce::jmap (juce::jlimit (-span, span, gain), -span, span, bar.getX(), bar.getRight());
        g.setColour (theme::output);
        g.fillRect (juce::Rectangle<float> (std::min (cx, x), bar.getY(), std::abs (x - cx), bar.getHeight()));
    }
    g.setColour (theme::text.withAlpha (0.6f));
    g.fillRect (cx - 0.5f, bar.getY() - 1.0f, 1.0f, bar.getHeight() + 2.0f);
    g.setFont (theme::font (9.0f));
    g.setColour (theme::textFaint);
    g.drawText ("-12", r.withWidth (30.0f), juce::Justification::centredLeft);
    g.drawText ("0", r.withSizeKeepingCentre (20.0f, r.getHeight()), juce::Justification::centred);
    g.drawText ("+12", r.withTrimmedLeft (r.getWidth() - 30.0f), juce::Justification::centredRight);
}

LevelTonePanel::LevelTonePanel (LiveVoiceProcessor& p) : ModulePanel ("LEVELER", theme::output), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, lvlOn);
    power.setTooltip ("Leveler: slowly rides the voice toward Target before the compressor, so someone who turns away "
                      "or leans in stays at a steady level. It only moves while they talk and holds between phrases. "
                      "When feedback is detected it stops turning up for 4 s (HOLD), so it can't feed a ring.");
    addAndMakeVisible (meter);
    target.attach (s, lvlTarget); range.attach (s, lvlRange);
    target.slider.setTooltip ("Level the voice is ridden toward (average loudness, dBFS RMS). Measured over ~0.3 s including the small gaps between words, so it reads well below the peaks: about -28 suits healthy -18 to -6 dB peaks. Set it where the voice sits when it sounds right.");
    range.slider.setTooltip ("Most it will turn up or down. Every dB of boost also raises the feedback risk, so keep it modest.");
    speed.attach (s, lvlSpeed, { "Slow", "Medium", "Fast" });
    speed.setTooltip ("How quickly it follows: Slow ~4 s, Medium ~2 s, Fast ~1 s. Turning down is twice as quick as turning up.");
    speedLabel.setText ("Speed", juce::dontSendNotification);
    proximity.attach (s, proxOn);
    proximity.setTooltip ("Proximity compensation: learns the voice's normal low-end balance while they talk, and turns the "
                          "low end (250 Hz shelf) down when they get close to the mic and it booms. Max Cut limits it.");
    proxAmount.attach (s, params::proxAmount);
    warmth.attach (s, params::warmth);
    warmth.slider.setTooltip ("Warmth: adds a little 2nd and 3rd harmonic to the low-mids only (80 Hz - 1.5 kHz) for thin voices. "
                       "0 % = completely bypassed. Subtle by design.");
    for (auto* l : { &speedLabel, &proxValue })
    {
        l->setFont (theme::font (11.0f));
        l->setColour (juce::Label::textColourId, theme::textDim);
        addAndMakeVisible (l);
    }
    proxValue.setJustificationType (juce::Justification::centredRight);
    for (auto* c : { (juce::Component*) &target, (juce::Component*) &range, (juce::Component*) &speed,
                     (juce::Component*) &proximity, (juce::Component*) &proxAmount, (juce::Component*) &warmth })
        addAndMakeVisible (c);
}

void LevelTonePanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();
    meter.setBounds (r.removeFromTop (34));
    r.removeFromTop (8);
    const int knobH = std::min (88, (r.getHeight() - 110) / 2);
    const int kw = r.getWidth() / 2;
    auto row = r.removeFromTop (knobH);
    target.setBounds (row.removeFromLeft (kw)); range.setBounds (row);
    r.removeFromTop (6);
    row = r.removeFromTop (26);
    speedLabel.setBounds (row.removeFromLeft (48));
    speed.setBounds (row);
    r.removeFromTop (12);
    toneY = r.getY();
    r.removeFromTop (22);
    row = r.removeFromTop (26);
    proximity.setBounds (row.removeFromLeft (row.getWidth() * 3 / 5).withTrimmedRight (4));
    proxValue.setBounds (row);
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    proxAmount.setBounds (row.removeFromLeft (kw)); warmth.setBounds (row);
}

void LevelTonePanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);
    auto b = getContentBounds();
    g.setColour (theme::border);
    g.drawHorizontalLine (toneY, (float) b.getX(), (float) b.getRight());
    g.setColour (theme::textFaint);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("TONE", b.getX(), toneY + 4, b.getWidth(), 16, juce::Justification::centredLeft);
}

void LevelTonePanel::update()
{
    meter.update();
    const bool on = raw (proc, proxOn) > 0.5f;
    const float cut = proc.proximity.getCutDb();
    proxValue.setText (on ? (cut > 0.05f ? "-" + juce::String (cut, 1) + " dB" : "0.0 dB") : "", juce::dontSendNotification);
    proxValue.setColour (juce::Label::textColourId, on && cut > 0.5f ? theme::output : theme::textDim);
    proxAmount.setEnabled (on);
    const bool lv = raw (proc, lvlOn) > 0.5f;
    target.setEnabled (lv); range.setEnabled (lv); speed.setEnabled (lv);
}

// ================================================================ compressor panel
CompPanel::CompPanel (LiveVoiceProcessor& p) : ModulePanel ("COMPRESSOR", theme::comp), proc (p)
{
    auto& s = proc.apvts;
    power.attach (s, compOn);
    autoRel.attach (s, compAuto);
    autoRel.setTooltip ("Program-dependent release: recovers quickly after short peaks and more gently after sustained "
                        "compression, like an optical compressor. The Release knob sets the overall speed.");
    autoGain.attach (s, compAutoGain);
    autoGain.setTooltip ("Auto gain: adds back the average level the compressor takes away (measured while someone talks), "
                         "so turning the compressor on or changing Threshold/Ratio doesn't change the overall loudness. "
                         "Makeup still adds on top.");
    addAndMakeVisible (autoRel);
    addAndMakeVisible (autoGain);
    thr.attach (s, compThr); ratio.attach (s, compRatio); knee.attach (s, compKnee);
    attack.attach (s, compAttack); release.attach (s, compRelease); makeup.attach (s, compMakeup);
    mix.attach (s, compMix);
    for (auto* k : { &thr, &ratio, &knee, &attack, &release, &makeup, &mix }) addAndMakeVisible (k);
    addAndMakeVisible (levelMeter);
}

void CompPanel::resized()
{
    layoutHeader();
    auto r = getContentBounds();
    levelMeter.setBounds (r.removeFromRight (64));
    r.removeFromRight (4);

    auto top = r.removeFromTop (26);
    autoRel.setBounds (top.removeFromLeft (top.getWidth() / 2).withTrimmedRight (3));
    autoGain.setBounds (top.withTrimmedLeft (3));
    r.removeFromTop (8);
    const int knobH = std::min (92, (r.getHeight() - 16) / 3);
    const int kw = r.getWidth() / 3;
    auto row = r.removeFromTop (knobH);
    for (auto* k : { &thr, &ratio, &knee }) k->setBounds (row.removeFromLeft (kw));
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    for (auto* k : { &attack, &release, &makeup }) k->setBounds (row.removeFromLeft (kw));
    r.removeFromTop (8);
    row = r.removeFromTop (knobH);
    mix.setBounds (row.removeFromLeft (kw));
}

void CompPanel::update()
{
    levelMeter.update();
}

// ================================================================ level match
LevelMatch::LevelMatch (LiveVoiceProcessor& p) : proc (p)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Output vs input loudness, measured while someone talks (last few seconds). "
                "Green = level: LiveVoice isn't making the voice louder or quieter overall. Click to set OUTPUT so they match.");
}

void LevelMatch::update()
{
    const bool v = proc.levelMatchValid();
    const float d = v ? std::round (proc.getOutputVsInputDb() * 2.0f) / 2.0f : 0.0f;   // 0.5 dB steps, no flicker
    if (v != valid || std::abs (d - shown) > 0.01f)
    {
        valid = v; shown = d;
        repaint();
    }
}

void LevelMatch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    const bool level = valid && std::abs (shown) < 1.0f;
    const auto c = ! valid ? theme::textFaint : (level ? theme::good : (std::abs (shown) < 3.0f ? theme::warn : theme::danger));
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (c.withAlpha (valid ? 0.9f : 0.5f));
    g.drawRoundedRectangle (r, 5.0f, 1.2f);

    auto top = r.removeFromTop (r.getHeight() * 0.45f);
    g.setFont (theme::font (9.0f, true));
    g.setColour (theme::textDim);
    g.drawText ("OUT vs IN", top, juce::Justification::centredBottom);
    g.setFont (theme::font (12.0f, true));
    g.setColour (c);
    juce::String t = "--";
    if (valid) t = level ? "LEVEL" : (shown > 0.0f ? "+" : "") + juce::String (shown, 1) + " dB";
    g.drawText (t, r, juce::Justification::centredTop);
}

void LevelMatch::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && proc.levelMatchValid())
        proc.matchOutputToInput();
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

    undoBtn.onClick = [this] { proc.undoManager.undo(); };
    redoBtn.onClick = [this] { proc.undoManager.redo(); };
    undoBtn.setTooltip ("Undo the last setting change");
    redoBtn.setTooltip ("Redo");

    for (auto* b : { &slotA, &slotB })
    {
        b->setColour (juce::TextButton::buttonOnColourId, theme::output);
        b->setColour (juce::TextButton::textColourOnId, theme::onAccent);
    }
    slotA.onClick = [this] { proc.selectSlot (0); };
    slotB.onClick = [this] { proc.selectSlot (1); };
    copyBtn.onClick = [this] { proc.copyActiveToOther(); };
    slotA.setTooltip ("A/B compare: two complete voice settings. Click B to try changes, A to go back.");
    slotB.setTooltip ("A/B compare: two complete voice settings. Click B to try changes, A to go back.");
    copyBtn.setTooltip ("Copy the current settings into the other slot");

    monitorBtn.setColour (juce::TextButton::buttonOnColourId, theme::warn);
    monitorBtn.setColour (juce::TextButton::textColourOnId, theme::onAccent);
    monitorBtn.setTooltip ("Monitor A/B: switches output 2 (Monitor) between the processed sound and the dry input, "
                           "so you can hear what LiveVoice is doing. Output 1 (FOH) is never affected.");
    monitorBtn.onClick = [this] { proc.monitorDry = ! proc.monitorDry.load(); };

    settingsBtn.setTooltip ("Display, outputs, feedback limits and detector filters");
    settingsBtn.onClick = [this] { if (onOpenSettings) onOpenSettings(); };

    inGain.attach (s, params::inGain);
    outGain.attach (s, params::outGain);
    bypass.attach (s, params::bypass);
    for (auto* k : { &inGain, &outGain })
    {
        k->slider.setSliderStyle (juce::Slider::LinearHorizontal);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
        k->label.setJustificationType (juce::Justification::centredLeft);
        k->label.setFont (theme::font (11.5f, true));
        addAndMakeVisible (k);
    }

    for (auto* c : { (juce::Component*) &saveBtn, (juce::Component*) &deleteBtn, (juce::Component*) &undoBtn, (juce::Component*) &redoBtn,
                     (juce::Component*) &slotA, (juce::Component*) &slotB, (juce::Component*) &copyBtn, (juce::Component*) &monitorBtn,
                     (juce::Component*) &settingsBtn, (juce::Component*) &levelMatch, (juce::Component*) &bypass })
        addAndMakeVisible (c);

    refreshPresetList();
}

void HeaderBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ("LIVEVOICE", juce::Rectangle<int> (14, 6, 110, 24), juce::Justification::centredLeft);
    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f));
    g.drawText ("v" LIVEVOICE_VERSION "  -  zero latency", juce::Rectangle<int> (15, 28, 150, 14), juce::Justification::centredLeft);
    g.setColour (theme::border);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void HeaderBar::resized()
{
    auto r = getLocalBounds().reduced (8, 0).withTrimmedLeft (150);
    const int h = 26, cy = getHeight() / 2;
    auto centreRow = [&] (juce::Rectangle<int> b) { return b.withSizeKeepingCentre (b.getWidth(), h).withY (cy - h / 2); };
    auto left = [&] (juce::Component& c, int w, int gap) { c.setBounds (centreRow (r.removeFromLeft (w))); r.removeFromLeft (gap); };
    auto right = [&] (juce::Component& c, int w, int gap) { c.setBounds (centreRow (r.removeFromRight (w))); r.removeFromRight (gap); };

    left (presetBox, 210, 5); left (saveBtn, 52, 3); left (deleteBtn, 58, 14);
    left (undoBtn, 52, 3); left (redoBtn, 52, 14);
    left (slotA, 28, 2); left (slotB, 28, 3); left (copyBtn, 42, 14);

    right (bypass, 74, 8); right (settingsBtn, 84, 12); right (monitorBtn, 78, 16);
    levelMatch.setBounds (r.removeFromRight (78).withSizeKeepingCentre (78, 34).withY (cy - 17));
    r.removeFromRight (8);

    // [label][slider........][value]
    auto gainBlock = [&] (Knob& k, int width)
    {
        auto b = r.removeFromRight (width);
        k.setBounds (b.withHeight (28).withY (cy - 14));
        auto kb = k.getLocalBounds();
        k.label.setBounds (kb.removeFromLeft (52));
        k.slider.setBounds (kb);
        r.removeFromRight (12);
    };
    gainBlock (outGain, 190);
    gainBlock (inGain, 190);
}

void HeaderBar::update()
{
    undoBtn.setEnabled (proc.undoManager.canUndo());
    redoBtn.setEnabled (proc.undoManager.canRedo());
    slotA.setToggleState (proc.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.getActiveSlot() == 1, juce::dontSendNotification);
    copyBtn.setButtonText (proc.getActiveSlot() == 0 ? "A>B" : "B>A");
    const bool dry = proc.monitorDry.load();
    monitorBtn.setToggleState (dry, juce::dontSendNotification);
    monitorBtn.setButtonText (dry ? "MON: DRY" : "MON: FX");
    monitorBtn.setEnabled (proc.isMonitorBusActive());
    levelMatch.update();
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
    proc.undoManager.beginNewTransaction ("Load preset");
    if (id >= 1000) proc.presets.loadUser (userNames[id - 1000]);
    else if (id >= 1) proc.presets.loadFactory (id - 1);
    deleteBtn.setEnabled (id >= 1000);
}

void HeaderBar::savePreset()
{
    dialog = std::make_unique<juce::AlertWindow> ("Save preset",
        "Saves the PSE, feedback, dynamic EQ, de-esser and compressor settings.\nGain and learned notches are not part of presets.",
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
