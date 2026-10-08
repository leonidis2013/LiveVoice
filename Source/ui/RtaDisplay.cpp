#include "RtaDisplay.h"
#include "Theme.h"

using namespace params;

namespace
{
    constexpr float smoothingFractions[] { 3.0f, 6.0f, 12.0f };
    const char* const smoothingNames[] { "1/3 oct", "1/6 oct", "1/12 oct" };
}

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
    binMag.resize (fftSize / 2 + 1);
    setOpaque (true);

    // view: input / output / both
    for (auto* b : { &viewIn, &viewOut, &viewBoth })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (101);
        b->setColour (juce::TextButton::buttonOnColourId, theme::output);
        b->setColour (juce::TextButton::textColourOnId, theme::onAccent);
        addAndMakeVisible (b);
    }
    viewIn.onClick   = [this] { proc.rtaView = 0; repaint(); };
    viewOut.onClick  = [this] { proc.rtaView = 1; repaint(); };
    viewBoth.onClick = [this] { proc.rtaView = 2; repaint(); };
    viewIn.setTooltip ("Show only the input (before LiveVoice)");
    viewOut.setTooltip ("Show only the output (what goes to FOH)");
    viewBoth.setTooltip ("Input as a grey area behind the output");

    smoothBtn.setTooltip ("RTA smoothing: 1/3, 1/6 or 1/12 octave");
    smoothBtn.onClick = [this] { proc.rtaSmoothing = (proc.rtaSmoothing + 1) % 3; updateButtons(); };
    tiltBtn.setClickingTogglesState (true);
    tiltBtn.setColour (juce::TextButton::buttonOnColourId, theme::output);
    tiltBtn.setColour (juce::TextButton::textColourOnId, theme::onAccent);
    tiltBtn.setTooltip ("+3 dB/octave tilt (pivot 1 kHz): pink noise reads flat, like most live RTAs");
    tiltBtn.onClick = [this] { proc.rtaTilt = tiltBtn.getToggleState(); };
    addAndMakeVisible (smoothBtn);
    addAndMakeVisible (tiltBtn);

    soloBanner.setColour (juce::TextButton::buttonColourId, theme::warn);
    soloBanner.setColour (juce::TextButton::textColourOffId, theme::onAccent);
    soloBanner.onClick = [this] { proc.soloTarget = LiveVoiceProcessor::soloNone; };
    addChildComponent (soloBanner);

    updateButtons();
}

void RtaDisplay::updateButtons()
{
    viewIn.setToggleState (proc.rtaView == 0, juce::dontSendNotification);
    viewOut.setToggleState (proc.rtaView == 1, juce::dontSendNotification);
    viewBoth.setToggleState (proc.rtaView == 2, juce::dontSendNotification);
    smoothBtn.setButtonText (smoothingNames[juce::jlimit (0, 2, proc.rtaSmoothing)]);
    tiltBtn.setToggleState (proc.rtaTilt, juce::dontSendNotification);
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
                                      .withTrimmedTop (24.0f).withTrimmedBottom (20.0f);
    const size_t nCols = (size_t) std::max (1, (int) plot.getWidth() / 2);
    inCols.assign (nCols, specBottom);
    outCols.assign (nCols, specBottom);

    // curve sample points every 3 px
    const int nPts = std::max (2, (int) plot.getWidth() / 3);
    curveFreqs.resize ((size_t) nPts);
    curveX.resize ((size_t) nPts);
    for (int i = 0; i < nPts; ++i)
    {
        curveX[(size_t) i] = plot.getX() + plot.getWidth() * (float) i / (float) (nPts - 1);
        curveFreqs[(size_t) i] = freqForX (curveX[(size_t) i]);
    }
    for (auto& s : bandSig) s.clear();
    eqDirty = notchDirty = gridDirty = true;

    int x = 44;
    auto place = [&] (juce::Button& b, int w) { b.setBounds (x, 3, w, 18); x += w + 3; };
    place (viewIn, 34); place (viewOut, 40); place (viewBoth, 46);
    x += 8;
    place (smoothBtn, 60); place (tiltBtn, 40);
    soloBanner.setBounds (juce::Rectangle<int> (240, 22).withCentre ({ (int) plot.getCentreX(), (int) plot.getY() + 16 }));
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

void RtaDisplay::bandResponse (int b, bool includeDynamic, std::vector<float>& out) const
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    out.assign (curveFreqs.size(), 0.0f);
    if (v (bOn (b)) < 0.5f) return;

    dsp::DynamicEq::BandSettings s;
    const auto shape = bandShapeFromChoice (b, (b == 0 || b == numEqBands - 1) ? (int) v (bType (b)) : 0);
    s.type = shape.type; s.slope = shape.slope;
    const float gain = v (bGain (b)) + (includeDynamic && v (bDyn (b)) > 0.5f ? dynGain[(size_t) b] : 0.0f);
    dsp::SvfCoeffs c[dsp::DynamicEq::maxStages];
    int stages = 1;
    const double fs = proc.currentSampleRate.load();
    dsp::DynamicEq::designBand (s, v (bFreq (b)), v (bQ (b)), gain, fs, c, stages);
    for (size_t i = 0; i < curveFreqs.size(); ++i)
    {
        double db = 0.0;
        for (int st = 0; st < stages; ++st) db += c[st].magnitudeDb (curveFreqs[i], fs);
        out[i] = (float) db;
    }
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

// Recomputes only what changed since the last frame.
void RtaDisplay::refreshCurves()
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const size_t n = curveFreqs.size();

    const bool eqOn = v (params::eqOn) > 0.5f;
    if (eqOn != eqWasOn) { eqDirty = true; eqWasOn = eqOn; }

    bool anyChanged = eqDirty;
    for (int b = 0; b < numEqBands; ++b)
    {
        std::vector<float> sig { v (bOn (b)), (b == 0 || b == numEqBands - 1) ? v (bType (b)) : 0.0f,
                                 v (bFreq (b)), v (bGain (b)), v (bQ (b)), v (bDyn (b)) };
        const bool dyn = sig[5] > 0.5f && sig[0] > 0.5f;
        if (sig != bandSig[(size_t) b] || bandStatic[(size_t) b].size() != n)
        {
            bandSig[(size_t) b] = sig;
            bandResponse (b, false, bandStatic[(size_t) b]);
            bandLiveGain[(size_t) b] = 1.0e9f;   // force live refresh
            anyChanged = true;
        }
        if (! dyn)
        {
            if (bandLive[(size_t) b] != bandStatic[(size_t) b]) { bandLive[(size_t) b] = bandStatic[(size_t) b]; anyChanged = true; }
        }
        else if (std::abs (dynGain[(size_t) b] - bandLiveGain[(size_t) b]) > 0.05f)
        {
            bandResponse (b, true, bandLive[(size_t) b]);
            bandLiveGain[(size_t) b] = dynGain[(size_t) b];
            anyChanged = true;
        }
    }
    if (anyChanged)
    {
        eqStatic.assign (n, 0.0f);
        eqLive.assign (n, 0.0f);
        if (eqOn)
            for (int b = 0; b < numEqBands; ++b)
                for (size_t i = 0; i < n; ++i) { eqStatic[i] += bandStatic[(size_t) b][i]; eqLive[i] += bandLive[(size_t) b][i]; }
        eqDirty = false;
        repaint();
    }

    // notches: signature of every active slot
    const bool fbOn = v (params::fbOn) > 0.5f;
    const float q = proc.getNotchQ();
    std::vector<float> sig;
    if (fbOn)
        for (auto& nt : notches)
            if (nt.kind != dsp::FeedbackSuppressor::kindInactive && nt.depthDb < -0.05f)
            {
                sig.push_back (std::round (nt.freq));
                sig.push_back (std::round (nt.depthDb * 10.0f));   // 0.1 dB steps
            }
    if (notchDirty || sig != lastNotchSig || ! juce::exactlyEqual (q, lastNotchQ) || fbOn != fbWasOn || notchCurve.size() != n)
    {
        lastNotchSig = sig; lastNotchQ = q; fbWasOn = fbOn;
        notchCurve.assign (n, 0.0f);
        if (fbOn)
        {
            const double fs = proc.currentSampleRate.load();
            for (auto& nt : notches)
            {
                if (nt.kind == dsp::FeedbackSuppressor::kindInactive || nt.depthDb > -0.05f) continue;
                dsp::SvfCoeffs c;
                c.bell (nt.freq, q, nt.depthDb, fs);
                for (size_t i = 0; i < n; ++i)
                {
                    // a narrow notch only matters within ~2 octaves; skip the far-away points
                    if (std::abs (std::log2 (curveFreqs[i] / nt.freq)) > 2.0f) continue;
                    notchCurve[i] += (float) c.magnitudeDb (curveFreqs[i], fs);
                }
            }
        }
        notchDirty = false;
        repaint();
    }
}

// ---------------------------------------------------------------- analysis
void RtaDisplay::computeSpectrum (ScopeTap& tap, std::vector<float>& cols)
{
    tap.readLatest (fftBuffer.data(), fftSize);
    for (int i = 0; i < fftSize; ++i) fftBuffer[(size_t) i] *= window[(size_t) i];
    std::fill (fftBuffer.begin() + fftSize, fftBuffer.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    const int numBins = fftSize / 2;
    for (int k = 0; k <= numBins; ++k) binMag[(size_t) k] = fftBuffer[(size_t) k];

    const double fs = proc.currentSampleRate.load();
    const float binHz = (float) (fs / fftSize);
    const int nCols = (int) cols.size();
    const float half = std::pow (2.0f, 0.5f / smoothingFractions[juce::jlimit (0, 2, proc.rtaSmoothing)]);
    const bool tilt = proc.rtaTilt;

    for (int x = 0; x < nCols; ++x)
    {
        const float f = freqForX (plot.getX() + (float) (2 * x) + 1.0f);
        const float lo = f / half / binHz, hi = f * half / binHz;
        float power = 0.0f;
        if (hi - lo < 1.0f)
        {
            const float pos = juce::jlimit (0.0f, (float) numBins - 1.001f, f / binHz);
            const int k = (int) pos;
            const float frac = pos - (float) k;
            const float a = binMag[(size_t) k] * (1.0f - frac) + binMag[(size_t) k + 1] * frac;
            power = a * a;
        }
        else
        {
            // average power across the smoothing band (true fractional-octave smoothing)
            const int k0 = juce::jlimit (0, numBins, (int) std::ceil (lo)), k1 = juce::jlimit (0, numBins, (int) std::floor (hi));
            for (int k = k0; k <= k1; ++k) power += binMag[(size_t) k] * binMag[(size_t) k];
            power /= (float) std::max (1, k1 - k0 + 1);
        }
        float db = 10.0f * std::log10 (power + 1.0e-14f);
        if (tilt) db += 3.0f * std::log2 (f / 1000.0f);

        auto& c = cols[(size_t) x];
        c = db > c ? 0.5f * (db + c) : std::max (db, c - 1.2f);   // fast rise, ~36 dB/s fall
    }
}

void RtaDisplay::update()
{
    if (plot.isEmpty()) return;

    if (proc.rtaView != 1) computeSpectrum (proc.inputTap, inCols);
    if (proc.rtaView != 0) computeSpectrum (proc.outputTap, outCols);

    for (int i = 0; i < (int) notches.size(); ++i)
        notches[(size_t) i] = proc.feedback.getNotch (i);
    for (int b = 0; b < numEqBands; ++b)
        dynGain[(size_t) b] = proc.dynEq.getDynamicGainDb (b);
    refreshCurves();

    const int count = proc.detector.getDetectionCount();
    if (count != lastDetectCount)
    {
        lastDetectCount = count;
        detectFlash = 1.0f;
        detectFreq = proc.detector.getLastDetectionFreq();
    }
    else
        detectFlash = std::max (0.0f, detectFlash - 0.012f);

    // solo banner (moved here from the header): obvious, and one click clears it
    const int st = proc.soloTarget.load();
    const bool soloOn = st != LiveVoiceProcessor::soloNone;
    soloBanner.setVisible (soloOn);
    if (soloOn)
    {
        soloFlash = (soloFlash + 1) % 30;
        soloBanner.setAlpha (soloFlash < 18 ? 1.0f : 0.6f);
        const juce::String what = st == LiveVoiceProcessor::soloDeEss ? "DE-ESSER" : "EQ BAND " + juce::String (st + 1);
        const bool toMain = proc.soloToMain.load();
        const juce::String txt = "SOLO " + what + (toMain ? " ON MAIN OUT" : " ON MONITOR") + "  -  CLEAR";
        if (soloBanner.getButtonText() != txt) soloBanner.setButtonText (txt);
    }
    updateButtons();
    repaint();
}

// ---------------------------------------------------------------- painting
void RtaDisplay::renderGrid()
{
    const float scale = juce::Component::getApproximateScaleFactorForComponent (this);
    const float s = std::max (1.0f, scale);
    gridImage = juce::Image (juce::Image::RGB, std::max (1, juce::roundToInt (getWidth() * s)), std::max (1, juce::roundToInt (getHeight() * s)), false);
    juce::Graphics g (gridImage);
    g.addTransform (juce::AffineTransform::scale (s));

    g.fillAll (theme::panel);
    g.setFont (theme::font (10.5f));

    const float freqs[] { 20, 30, 40, 50, 60, 80, 100, 200, 300, 400, 500, 600, 800, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 20000 };
    for (float f : freqs)
    {
        const int fi = juce::roundToInt (f);
        const bool major = fi == 100 || fi == 1000 || fi == 10000;
        g.setColour (major ? theme::grid.overlaidWith (theme::textFaint.withAlpha (0.3f)) : theme::grid);
        g.drawVerticalLine ((int) xForFreq (f), plot.getY(), plot.getBottom());
    }
    const std::pair<float, const char*> labels[] { { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" }, { 1000, "1k" },
                                                   { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" } };
    g.setColour (theme::textFaint);
    for (auto& [f, t] : labels)
        g.drawText (t, juce::Rectangle<float> (xForFreq (f) - 20.0f, plot.getBottom() + 2.0f, 40.0f, 16.0f), juce::Justification::centred);

    for (float db = specTop; db >= specBottom; db -= 12.0f)
    {
        const float y = yForSpecDb (db);
        g.setColour (theme::grid);
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (theme::textFaint);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (2.0f, y - 7.0f, 32.0f, 14.0f), juce::Justification::centredRight);
    }
    for (int dbi = -18; dbi <= 18; dbi += 6)
    {
        const float y = yForEqDb ((float) dbi);
        if (dbi == 0)
        {
            g.setColour (theme::eq.withAlpha (0.25f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        }
        g.setColour (theme::eq.withAlpha (0.45f));
        g.drawText ((dbi > 0 ? "+" : "") + juce::String (dbi), juce::Rectangle<float> (plot.getRight() + 3.0f, y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredLeft);
    }

    // legend
    auto legend = getLocalBounds().toFloat().removeFromTop (22.0f).withTrimmedRight (40.0f);
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
    g.drawText ("RTA", juce::Rectangle<float> (10.0f, 0.0f, 32.0f, 24.0f), juce::Justification::centredLeft);

    g.setColour (theme::border);
    g.drawRect (getLocalBounds().toFloat(), 1.0f);
    gridDirty = false;
}

void RtaDisplay::paint (juce::Graphics& g)
{
    if (gridDirty || gridImage.isNull()) renderGrid();
    g.drawImage (gridImage, getLocalBounds().toFloat());

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.toNearestInt());

        drawDeEss (g);

        // spectra: input as a solid grey area, output as a blue area with a line
        const int nCols = (int) outCols.size();
        auto areaPath = [&] (const std::vector<float>& cols, juce::Path& fill, juce::Path* line)
        {
            fill.startNewSubPath (plot.getX(), plot.getBottom());
            for (int x = 0; x < nCols; ++x)
            {
                const float px = plot.getX() + (float) (2 * x);
                const float y = yForSpecDb (cols[(size_t) x]);
                fill.lineTo (px, y);
                if (line != nullptr) { if (x == 0) line->startNewSubPath (px, y); else line->lineTo (px, y); }
            }
            fill.lineTo (plot.getRight(), plot.getBottom());
            fill.closeSubPath();
        };
        if (proc.rtaView != 1)
        {
            juce::Path inFill;
            areaPath (inCols, inFill, nullptr);
            g.setColour (theme::input.withAlpha (theme::lightMode ? 0.35f : 0.45f));
            g.fillPath (inFill);
        }
        if (proc.rtaView != 0)
        {
            juce::Path outFill, outLine;
            areaPath (outCols, outFill, &outLine);
            g.setColour (theme::output.withAlpha (proc.rtaView == 2 ? 0.28f : 0.35f));
            g.fillPath (outFill);
            g.setColour (theme::output.withAlpha (0.9f));
            g.strokePath (outLine, juce::PathStrokeType (1.2f));
        }

        // cached curves
        const size_t n = curveFreqs.size();
        if (n > 1 && eqLive.size() == n && notchCurve.size() == n)
        {
            if (fbWasOn)
            {
                juce::Path fb;
                for (size_t i = 0; i < n; ++i)
                    if (i == 0) fb.startNewSubPath (curveX[i], yForEqDb (notchCurve[i])); else fb.lineTo (curveX[i], yForEqDb (notchCurve[i]));
                g.setColour (theme::feedback.withAlpha (0.95f));
                g.strokePath (fb, juce::PathStrokeType (1.6f));
            }
            if (eqWasOn)
            {
                juce::Path live, stat, fill;
                const float y0 = yForEqDb (0.0f);
                fill.startNewSubPath (curveX[0], y0);
                for (size_t i = 0; i < n; ++i)
                {
                    const float yl = yForEqDb (eqLive[i]), ys = yForEqDb (eqStatic[i]);
                    if (i == 0) { live.startNewSubPath (curveX[i], yl); stat.startNewSubPath (curveX[i], ys); }
                    else        { live.lineTo (curveX[i], yl); stat.lineTo (curveX[i], ys); }
                    fill.lineTo (curveX[i], yl);
                }
                fill.lineTo (curveX[n - 1], y0);
                fill.closeSubPath();
                g.setColour (theme::eq.withAlpha (0.10f));
                g.fillPath (fill);
                if (eqLive != eqStatic)   // only draw the static (dashed) curve when the dynamic one differs
                {
                    g.setColour (theme::eq.withAlpha (0.45f));
                    const float dashes[] { 4.0f, 3.0f };
                    juce::Path dashed;
                    juce::PathStrokeType (1.0f).createDashedStroke (dashed, stat, dashes, 2);
                    g.fillPath (dashed);
                }
                g.setColour (theme::eq);
                g.strokePath (live, juce::PathStrokeType (2.0f));
            }
        }
    }

    drawNotchMarkers (g);
    drawHandles (g);

    if (detectFlash > 0.0f && detectFreq > 0.0f)
    {
        const juce::String txt = "FEEDBACK  " + (detectFreq >= 1000.0f ? juce::String (detectFreq / 1000.0f, 2) + " kHz"
                                                                      : juce::String (juce::roundToInt (detectFreq)) + " Hz");
        auto r = juce::Rectangle<float> (300.0f, 3.0f, 150.0f, 18.0f);
        g.setColour (theme::feedback.withAlpha (0.22f * detectFlash + 0.1f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::feedback.withAlpha (0.4f + 0.6f * detectFlash));
        g.setFont (theme::font (11.5f, true));
        g.drawText (txt, r, juce::Justification::centred);
    }
}

// De-esser: a quiet shaded band with a label. No live numbers.
void RtaDisplay::drawDeEss (juce::Graphics& g)
{
    auto v = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const bool soloed = proc.soloTarget.load() == LiveVoiceProcessor::soloDeEss;
    if (v (dsOn) < 0.5f && ! soloed) return;

    const int mode = juce::jlimit (0, 2, (int) v (dsMode));
    const float freq = v (dsFreq), q = v (dsQ);
    float lo = freq, hi = maxFreq;
    if (mode == dsp::DeEsser::band)
    {
        const float bwOct = 2.0f * std::asinh (1.0f / (2.0f * q)) / std::log (2.0f);
        lo = freq * std::pow (2.0f, -bwOct * 0.5f);
        hi = freq * std::pow (2.0f, bwOct * 0.5f);
    }
    const float x0 = xForFreq (juce::jlimit (minFreq, maxFreq, lo)), x1 = xForFreq (juce::jlimit (minFreq, maxFreq, hi));
    g.setColour (theme::deess.withAlpha (soloed ? 0.18f : 0.09f));
    g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, plot.getHeight()));
    g.setFont (theme::font (10.5f, true));
    g.setColour (theme::deess.withAlpha (0.85f));
    g.drawText ("De-esser", juce::Rectangle<float> (0.5f * (x0 + x1) - 40.0f, plot.getBottom() - 18.0f, 80.0f, 14.0f), juce::Justification::centred);
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
