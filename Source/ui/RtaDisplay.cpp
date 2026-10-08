#include "RtaDisplay.h"
#include "Theme.h"

using namespace params;

RtaDisplay::RtaDisplay (LiveVoiceProcessor& p) : proc (p)
{
    window.resize (fftSize);
    double sum = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        sum += window[(size_t) i];
    }
    for (auto& w : window) w *= (float) (2.0 / sum);   // full-scale sine reads 0 dBFS
    fftBuffer.resize (fftSize * 2);
    binDb.resize (fftSize / 2 + 1);
    setOpaque (true);
}

// ---------------------------------------------------------------- mapping
float RtaDisplay::xForFreq (float f) const
{
    return plot.getX() + plot.getWidth() * std::log (f / minFreq) / std::log (maxFreq / minFreq);
}
float RtaDisplay::freqForX (float x) const
{
    return minFreq * std::pow (maxFreq / minFreq, juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth()));
}
float RtaDisplay::yForSpecDb (float db) const
{
    return juce::jmap (juce::jlimit (specBottom, specTop, db), specTop, specBottom, plot.getY(), plot.getBottom());
}
float RtaDisplay::yForEqDb (float db) const
{
    return juce::jmap (juce::jlimit (-eqRange, eqRange, db), eqRange, -eqRange, plot.getY(), plot.getBottom());
}
float RtaDisplay::eqDbForY (float y) const
{
    return juce::jmap (y, plot.getY(), plot.getBottom(), eqRange, -eqRange);
}

void RtaDisplay::resized()
{
    plot = getLocalBounds().toFloat().withTrimmedLeft (38.0f).withTrimmedRight (36.0f)
                                      .withTrimmedTop (22.0f).withTrimmedBottom (20.0f);
    inCols.assign ((size_t) std::max (1, (int) plot.getWidth()), specBottom);
    outCols.assign (inCols.size(), specBottom);
}

// ---------------------------------------------------------------- params
juce::RangedAudioParameter* RtaDisplay::param (const juce::String& id) const
{
    return proc.apvts.getParameter (id);
}

params::BandType RtaDisplay::bandType (int b) const
{
    if (b == 0 || b == numEqBands - 1)
        return bandTypeFromChoice (b, (int) proc.apvts.getRawParameterValue (bType (b))->load());
    return BandType::bell;
}

void RtaDisplay::setParam (const juce::String& id, float realValue)
{
    if (auto* rp = param (id))
        rp->setValueNotifyingHost (rp->convertTo0to1 (realValue));
}

float RtaDisplay::bandEqDb (int b, double freq, bool includeDynamic) const
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    if (v (bOn (b)) < 0.5f) return 0.0f;

    dsp::DynamicEq::BandSettings s;
    const auto shape = bandShapeFromChoice (b, (b == 0 || b == numEqBands - 1) ? (int) v (bType (b)) : 0);
    s.type = shape.type; s.slope = shape.slope;
    const float gain = v (bGain (b)) + (includeDynamic && v (bDyn (b)) > 0.5f ? dynGain[(size_t) b] : 0.0f);
    dsp::SvfCoeffs c[dsp::DynamicEq::maxStages];
    int stages = 1;
    const double fs = proc.currentSampleRate.load();
    dsp::DynamicEq::designBand (s, v (bFreq (b)), v (bQ (b)), gain, fs, c, stages);
    double db = 0.0;
    for (int i = 0; i < stages; ++i) db += c[i].magnitudeDb (freq, fs);
    return (float) db;
}

juce::Point<float> RtaDisplay::handlePosition (int b) const
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const float x = xForFreq (v (bFreq (b)));
    const float y = isPassType (bandType (b)) ? yForEqDb (0.0f) : yForEqDb (v (bGain (b)));
    return { x, y };
}

int RtaDisplay::hitBand (juce::Point<float> pos) const
{
    int best = -1;
    float bestD = 11.0f;
    for (int b = 0; b < numEqBands; ++b)
    {
        const float d = handlePosition (b).getDistanceFrom (pos);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

int RtaDisplay::hitNotch (juce::Point<float> pos) const
{
    if (pos.y > plot.getY() + 4.0f || pos.y < plot.getY() - 20.0f) return -1;
    int best = -1;
    float bestD = 7.0f;
    for (int i = 0; i < (int) notches.size(); ++i)
    {
        if (notches[(size_t) i].kind == dsp::FeedbackSuppressor::kindInactive) continue;
        const float d = std::abs (xForFreq (notches[(size_t) i].freq) - pos.x);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------- analysis
void RtaDisplay::computeSpectrum (ScopeTap& tap, std::vector<float>& cols)
{
    tap.readLatest (fftBuffer.data(), fftSize);
    for (int i = 0; i < fftSize; ++i) fftBuffer[(size_t) i] *= window[(size_t) i];
    std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    const int numBins = fftSize / 2;
    for (int k = 0; k <= numBins; ++k)
        binDb[(size_t) k] = fftBuffer[(size_t) k];   // linear for now

    const double fs = proc.currentSampleRate.load();
    const float binHz = (float) (fs / fftSize);
    const int nCols = (int) cols.size();
    const float smoothing = std::pow (2.0f, 1.0f / 48.0f);   // ~1/24 octave per column window

    for (int x = 0; x < nCols; ++x)
    {
        const float f = freqForX (plot.getX() + (float) x + 0.5f);
        const float lo = f / smoothing / binHz, hi = f * smoothing / binHz;
        float power = 0.0f;
        if (hi - lo < 1.0f)
        {
            const float pos = juce::jlimit (0.0f, (float) numBins - 1.001f, f / binHz);
            const int k = (int) pos;
            const float frac = pos - (float) k;
            const float a = binDb[(size_t) k] * (1.0f - frac) + binDb[(size_t) k + 1] * frac;
            power = a * a;
        }
        else
        {
            const int k0 = juce::jlimit (0, numBins, (int) std::ceil (lo)), k1 = juce::jlimit (0, numBins, (int) std::floor (hi));
            for (int k = k0; k <= k1; ++k) power = std::max (power, binDb[(size_t) k] * binDb[(size_t) k]);
        }
        const float db = 10.0f * std::log10 (power + 1.0e-14f);

        auto& c = cols[(size_t) x];
        c = db > c ? 0.5f * (db + c) : std::max (db, c - 1.2f);   // fast rise, ~36 dB/s fall
    }
}

void RtaDisplay::update()
{
    if (plot.isEmpty()) return;

    computeSpectrum (proc.inputTap, inCols);
    computeSpectrum (proc.outputTap, outCols);

    for (int i = 0; i < (int) notches.size(); ++i)
        notches[(size_t) i] = proc.feedback.getNotch (i);
    for (int b = 0; b < numEqBands; ++b)
        dynGain[(size_t) b] = proc.dynEq.getDynamicGainDb (b);

    const int count = proc.detector.getDetectionCount();
    if (count != lastDetectCount)
    {
        lastDetectCount = count;
        detectFlash = 1.0f;
        detectFreq = proc.detector.getLastDetectionFreq();
    }
    else
        detectFlash = std::max (0.0f, detectFlash - 0.012f);

    repaint();
}

// ---------------------------------------------------------------- painting
void RtaDisplay::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    drawGrid (g);

    // spectra
    const int nCols = (int) outCols.size();
    if (nCols > 1)
    {
        juce::Path outFill, outLine, inLine;
        for (int x = 0; x < nCols; ++x)
        {
            const float px = plot.getX() + (float) x;
            const float yo = yForSpecDb (outCols[(size_t) x]);
            const float yi = yForSpecDb (inCols[(size_t) x]);
            if (x == 0) { outFill.startNewSubPath (px, plot.getBottom()); outLine.startNewSubPath (px, yo); inLine.startNewSubPath (px, yi); }
            outFill.lineTo (px, yo);
            if (x > 0) { outLine.lineTo (px, yo); inLine.lineTo (px, yi); }
        }
        outFill.lineTo (plot.getRight(), plot.getBottom());
        outFill.closeSubPath();

        g.setGradientFill (juce::ColourGradient (theme::output.withAlpha (0.38f), 0.0f, plot.getY(),
                                                 theme::output.withAlpha (0.04f), 0.0f, plot.getBottom(), false));
        g.fillPath (outFill);
        g.setColour (theme::output.withAlpha (0.85f));
        g.strokePath (outLine, juce::PathStrokeType (1.2f));
        g.setColour (theme::input.withAlpha (0.9f));
        g.strokePath (inLine, juce::PathStrokeType (1.0f));
    }

    drawCurves (g);
    drawNotchMarkers (g);
    drawHandles (g);

    // legend
    auto legend = getLocalBounds().toFloat().removeFromTop (20.0f).withTrimmedRight (40.0f);
    g.setFont (theme::font (11.5f));
    const std::tuple<const char*, juce::Colour, float> items[] {
        { "Input", theme::input, 54.0f }, { "Output", theme::output, 62.0f }, { "Dynamic EQ", theme::eq, 86.0f },
        { "Feedback notches", theme::feedback, 118.0f }, { "De-esser", theme::deess, 72.0f } };
    for (int i = 4; i >= 0; --i)
    {
        auto r = legend.removeFromRight (std::get<2> (items[i]));
        g.setColour (std::get<1> (items[i]));
        g.fillRoundedRectangle (r.getX(), r.getCentreY() - 3.0f, 10.0f, 6.0f, 2.0f);
        g.setColour (theme::textDim);
        g.drawText (std::get<0> (items[i]), r.withTrimmedLeft (15.0f), juce::Justification::centredLeft);
    }

    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f, true));
    g.drawText ("RTA", getLocalBounds().toFloat().removeFromTop (20.0f).withTrimmedLeft (10.0f), juce::Justification::centredLeft);

    if (detectFlash > 0.0f && detectFreq > 0.0f)
    {
        const juce::String txt = "FEEDBACK  " + (detectFreq >= 1000.0f ? juce::String (detectFreq / 1000.0f, 2) + " kHz"
                                                                      : juce::String (juce::roundToInt (detectFreq)) + " Hz");
        auto r = juce::Rectangle<float> (plot.getX() + 46.0f, 2.0f, 150.0f, 17.0f);
        g.setColour (theme::feedback.withAlpha (0.22f * detectFlash + 0.1f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::feedback.withAlpha (0.4f + 0.6f * detectFlash));
        g.setFont (theme::font (11.5f, true));
        g.drawText (txt, r, juce::Justification::centred);
    }

    g.setColour (theme::border);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);
}

void RtaDisplay::drawGrid (juce::Graphics& g)
{
    g.setFont (theme::font (10.5f));

    const float freqs[] { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000 };
    for (float f : freqs)
    {
        const float x = xForFreq (f);
        const int fi = juce::roundToInt (f);
        const bool major = fi == 100 || fi == 1000 || fi == 10000;
        g.setColour (major ? theme::grid.overlaidWith (theme::textFaint.withAlpha (0.3f)) : theme::grid);
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
    }
    const std::pair<float, const char*> labels[] { { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1k" },
                                                   { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" } };
    g.setColour (theme::textFaint);
    for (auto& [f, t] : labels)
        g.drawText (t, juce::Rectangle<float> (xForFreq (f) - 20.0f, plot.getBottom() + 2.0f, 40.0f, 16.0f), juce::Justification::centred);

    // spectrum dB on the left
    for (float db = specTop; db >= specBottom; db -= 12.0f)
    {
        const float y = yForSpecDb (db);
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (theme::textFaint);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (2.0f, y - 7.0f, 32.0f, 14.0f), juce::Justification::centredRight);
    }

    // EQ dB on the right
    for (int dbi = -18; dbi <= 18; dbi += 6)
    {
        const float db = (float) dbi;
        const float y = yForEqDb (db);
        if (dbi == 0)
        {
            g.setColour (theme::eq.withAlpha (0.25f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
        g.setColour (theme::eq.withAlpha (0.45f));
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (plot.getRight() + 3.0f, y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredLeft);
    }
}

// De-esser: shaded detection band plus its live reduction curve.
void RtaDisplay::drawDeEss (juce::Graphics& g)
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    if (v (dsOn) < 0.5f && proc.soloTarget.load() != LiveVoiceProcessor::soloDeEss) return;

    dsp::DeEsser::Settings s;
    s.mode = juce::jlimit (0, 2, (int) v (dsMode));
    s.freq = v (dsFreq); s.q = v (dsQ);
    const double fs = proc.currentSampleRate.load();
    const float gr = proc.deEsser.getCurrentGainReductionDb();
    const bool soloed = proc.soloTarget.load() == LiveVoiceProcessor::soloDeEss;

    float lo = s.freq, hi = maxFreq;
    if (s.mode == dsp::DeEsser::band)
    {
        const float bwOct = 2.0f * std::asinh (1.0f / (2.0f * s.q)) / std::log (2.0f);
        lo = s.freq * std::pow (2.0f, -bwOct * 0.5f);
        hi = s.freq * std::pow (2.0f, bwOct * 0.5f);
    }
    const float x0 = xForFreq (juce::jlimit (minFreq, maxFreq, lo)), x1 = xForFreq (juce::jlimit (minFreq, maxFreq, hi));
    const float activity = juce::jlimit (0.0f, 1.0f, gr / std::max (1.0f, v (dsRange)));
    g.setColour (theme::deess.withAlpha ((soloed ? 0.20f : 0.08f) + 0.14f * activity));
    g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, plot.getHeight()));
    g.setColour (theme::deess.withAlpha (0.5f));
    g.drawVerticalLine ((int) xForFreq (s.freq), plot.getY(), plot.getBottom());

    // label at the top of the band
    g.setFont (theme::font (10.5f, true));
    g.setColour (theme::deess);
    const juce::String label = juce::String (soloed ? "SOLO  " : "") + "DE-ESS " + (gr > 0.2f ? juce::String (-gr, 1) + " dB" : "");
    g.drawText (label, juce::Rectangle<float> (xForFreq (s.freq) - 70.0f, plot.getY() + 22.0f, 140.0f, 14.0f), juce::Justification::centred);

    // live reduction curve
    if (gr > 0.05f)
    {
        juce::Path curve;
        const int n = std::max (2, (int) plot.getWidth() / 2);
        for (int i = 0; i < n; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
            const float y = yForEqDb (dsp::DeEsser::reductionResponseDb (s, gr, freqForX (x), fs));
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        g.setColour (theme::deess);
        g.strokePath (curve, juce::PathStrokeType (1.8f));
    }
}

void RtaDisplay::drawCurves (juce::Graphics& g)
{
    const double fs = proc.currentSampleRate.load();
    const int nCols = std::max (2, (int) plot.getWidth() / 2);
    const bool eqOnNow = proc.apvts.getRawParameterValue (params::eqOn)->load() > 0.5f;
    const bool fbOnNow = proc.apvts.getRawParameterValue (params::fbOn)->load() > 0.5f;
    const float q = proc.getNotchQ();

    juce::Path eqStatic, eqLive, eqFill, fbCurve;
    for (int i = 0; i < nCols; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (nCols - 1);
        const double f = freqForX (x);
        float sDb = 0.0f, lDb = 0.0f;
        if (eqOnNow)
            for (int b = 0; b < numEqBands; ++b) { sDb += bandEqDb (b, f, false); lDb += bandEqDb (b, f, true); }
        const float nDb = fbOnNow ? dsp::FeedbackSuppressor::responseDb (notches.data(), (int) notches.size(), q, f, fs) : 0.0f;

        const float ys = yForEqDb (sDb), yl = yForEqDb (lDb), yn = yForEqDb (nDb);
        if (i == 0)
        {
            eqStatic.startNewSubPath (x, ys); eqLive.startNewSubPath (x, yl); fbCurve.startNewSubPath (x, yn);
            eqFill.startNewSubPath (x, yForEqDb (0.0f)); eqFill.lineTo (x, yl);
        }
        else
        {
            eqStatic.lineTo (x, ys); eqLive.lineTo (x, yl); fbCurve.lineTo (x, yn); eqFill.lineTo (x, yl);
        }
    }
    eqFill.lineTo (plot.getRight(), yForEqDb (0.0f));
    eqFill.closeSubPath();

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());

    drawDeEss (g);

    if (fbOnNow)
    {
        g.setColour (theme::feedback.withAlpha (0.95f));
        g.strokePath (fbCurve, juce::PathStrokeType (1.6f));
    }

    if (eqOnNow)
    {
        g.setColour (theme::eq.withAlpha (0.10f));
        g.fillPath (eqFill);
        g.setColour (theme::eq.withAlpha (0.4f));
        const float dashes[] { 4.0f, 3.0f };
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, eqStatic, dashes, 2);
        g.fillPath (dashed);
        g.setColour (theme::eq);
        g.strokePath (eqLive, juce::PathStrokeType (2.0f));
    }

    g.restoreState();
}

void RtaDisplay::drawNotchMarkers (juce::Graphics& g)
{
    for (int i = 0; i < (int) notches.size(); ++i)
    {
        const auto& n = notches[(size_t) i];
        if (n.kind == dsp::FeedbackSuppressor::kindInactive) continue;
        const float x = xForFreq (n.freq);
        const bool fixed = n.kind == dsp::FeedbackSuppressor::kindFixed;
        const float y = plot.getY() - 1.0f;

        juce::Path tri;
        tri.addTriangle (x - 5.0f, y - 8.0f, x + 5.0f, y - 8.0f, x, y);
        g.setColour (theme::feedback.withAlpha (i == hoverNotch ? 1.0f : 0.9f));
        if (fixed) g.fillPath (tri);
        else       g.strokePath (tri, juce::PathStrokeType (1.3f));

        if (i == hoverNotch)
        {
            const juce::String label = (n.freq >= 1000.0f ? juce::String (n.freq / 1000.0f, 2) + "k" : juce::String (juce::roundToInt (n.freq)))
                                       + "  " + juce::String (n.depthDb, 1) + " dB" + (fixed ? "  fixed" : "  floating");
            auto r = juce::Rectangle<float> (x - 70.0f, plot.getY() + 4.0f, 140.0f, 16.0f);
            g.setColour (theme::bg.withAlpha (0.85f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (theme::feedback);
            g.setFont (theme::font (11.0f));
            g.drawText (label, r, juce::Justification::centred);
        }
    }
}

void RtaDisplay::drawHandles (juce::Graphics& g)
{
    if (proc.apvts.getRawParameterValue (params::eqOn)->load() < 0.5f) return;

    for (int b = 0; b < numEqBands; ++b)
    {
        const bool on = proc.apvts.getRawParameterValue (bOn (b))->load() > 0.5f;
        const bool dyn = proc.apvts.getRawParameterValue (bDyn (b))->load() > 0.5f && ! isPassType (bandType (b));
        const auto pos = handlePosition (b);
        const auto col = theme::bandColours[b];
        const float r = (b == selectedBand ? 9.0f : 7.5f) + (b == hoverBand ? 1.0f : 0.0f);

        if (dyn && on && std::abs (dynGain[(size_t) b]) > 0.05f)
        {
            const float y2 = yForEqDb (proc.apvts.getRawParameterValue (bGain (b))->load() + dynGain[(size_t) b]);
            g.setColour (col.withAlpha (0.7f));
            g.drawLine (pos.x, pos.y, pos.x, y2, 3.0f);
        }

        g.setColour (on ? col.withAlpha (0.25f) : theme::panel);
        g.fillEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f);
        g.setColour (on ? col : theme::textFaint);
        g.drawEllipse (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f, b == selectedBand ? 2.0f : 1.3f);
        g.setFont (theme::font (10.5f, true));
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (pos.x - r, pos.y - r, r * 2.0f, r * 2.0f), juce::Justification::centred);
        if (dyn)
        {
            g.setColour (col);
            g.fillEllipse (pos.x + r * 0.6f, pos.y - r * 1.05f, 5.0f, 5.0f);  // "dynamic" dot
        }
    }
}

// ---------------------------------------------------------------- mouse
void RtaDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.position;

    if (const int n = hitNotch (pos); n >= 0)
    {
        {
            const auto info = notches[(size_t) n];
            juce::PopupMenu m;
            m.addSectionHeader ((info.freq >= 1000.0f ? juce::String (info.freq / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (info.freq)) + " Hz")
                                + "  (" + juce::String (info.depthDb, 1) + " dB)");
            m.addItem (1, "Remove this notch");
            m.addItem (2, "Make fixed", info.kind == dsp::FeedbackSuppressor::kindFloating);
            juce::Component::SafePointer<RtaDisplay> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                             [safe, n] (int result)
                             {
                                 if (safe == nullptr || result == 0) return;
                                 dsp::FeedbackSuppressor::Command c;
                                 c.type = result == 1 ? dsp::FeedbackSuppressor::Command::Type::removeSlot
                                                      : dsp::FeedbackSuppressor::Command::Type::makeFixed;
                                 c.slot = n;
                                 safe->proc.sendNotchCommand (c);
                             });
        }
        return;
    }

    dragBand = hitBand (pos);
    if (dragBand >= 0)
    {
        selectedBand = dragBand;
        if (onBandSelected) onBandSelected (dragBand);
        if (auto* f = param (bFreq (dragBand))) f->beginChangeGesture();
        if (auto* gp = param (bGain (dragBand))) gp->beginChangeGesture();
        repaint();
    }
}

void RtaDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    setParam (bFreq (dragBand), juce::jlimit (minFreq, maxFreq, freqForX (e.position.x)));
    if (! isPassType (bandType (dragBand)))
        setParam (bGain (dragBand), juce::jlimit (-18.0f, 18.0f, eqDbForY (e.position.y)));
}

void RtaDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragBand >= 0)
    {
        if (auto* f = param (bFreq (dragBand))) f->endChangeGesture();
        if (auto* gp = param (bGain (dragBand))) gp->endChangeGesture();
    }
    dragBand = -1;
}

void RtaDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int hb = hitBand (e.position), hn = hitNotch (e.position);
    if (hb != hoverBand || hn != hoverNotch)
    {
        hoverBand = hb; hoverNotch = hn;
        setMouseCursor (hb >= 0 ? juce::MouseCursor::DraggingHandCursor
                                : hn >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void RtaDisplay::mouseExit (const juce::MouseEvent&)
{
    hoverBand = hoverNotch = -1;
    repaint();
}

void RtaDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = hitBand (e.position);
    if (b >= 0 && ! isPassType (bandType (b)))
    {
        auto* gp = param (bGain (b));
        gp->beginChangeGesture();
        setParam (bGain (b), 0.0f);
        gp->endChangeGesture();
    }
}

void RtaDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    int b = hitBand (e.position);
    if (b < 0) b = dragBand;
    if (b < 0) return;
    auto* qp = param (bQ (b));
    const float q = proc.apvts.getRawParameterValue (bQ (b))->load();
    qp->beginChangeGesture();
    setParam (bQ (b), juce::jlimit (0.3f, 10.0f, q * std::pow (1.15f, w.deltaY * 6.0f)));
    qp->endChangeGesture();
}
