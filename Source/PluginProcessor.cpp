// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#include "PluginProcessor.h"
#include "PluginEditor.h"

LiveVoiceProcessor::LiveVoiceProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                        .withOutput ("1 FOH + 2 Monitor", juce::AudioChannelSet::stereo(), true)),   // ch 1 = FOH, ch 2 = Monitor
      apvts (*this, &undoManager, "LiveVoiceState", params::createLayout()),
      presets (apvts)
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };
    using namespace params;
    p.inGain = raw (inGain); p.outGain = raw (outGain); p.bypass = raw (bypass);
    p.fbOn = raw (fbOn); p.fbMode = raw (fbMode); p.fbSens = raw (fbSens); p.fbLearn = raw (fbLearn); p.fbFreeze = raw (fbFreeze);
    p.fbMaxFixed = raw (fbMaxFixed); p.fbMaxFloat = raw (fbMaxFloat); p.fbMaxDepth = raw (fbMaxDepth); p.fbWidth = raw (fbWidth);
    p.fbHold = raw (fbHold); p.fbLo = raw (fbLoFreq); p.fbHi = raw (fbHiFreq);
    p.eqOn = raw (eqOn);
    for (int b = 0; b < numEqBands; ++b)
    {
        auto& d = p.bands[(size_t) b];
        d.on = raw (bOn (b)); d.type = (b == 0 || b == numEqBands - 1) ? raw (bType (b)) : nullptr;
        d.freq = raw (bFreq (b)); d.gain = raw (bGain (b)); d.q = raw (bQ (b)); d.dyn = raw (bDyn (b));
        d.thr = raw (bThr (b)); d.range = raw (bRange (b)); d.ratio = raw (bRatio (b)); d.att = raw (bAtt (b)); d.rel = raw (bRel (b));
    }
    p.dsOn = raw (dsOn); p.dsFreq = raw (dsFreq); p.dsThr = raw (dsThr); p.dsRange = raw (dsRange); p.dsMode = raw (dsMode); p.dsRel = raw (dsRel);
    p.dsRelative = raw (dsRelative); p.dsRelThr = raw (dsRelThr); p.dsQ = raw (dsQ);
    p.pseOn = raw (pseOn); p.pseThr = raw (pseThr); p.pseRange = raw (pseRange); p.pseAttack = raw (pseAttack);
    p.pseHold = raw (pseHold); p.pseRelease = raw (pseRelease); p.pseScHp = raw (pseScHp); p.pseScLp = raw (pseScLp);
    p.compOn = raw (compOn); p.compThr = raw (compThr); p.compRatio = raw (compRatio); p.compKnee = raw (compKnee);
    p.compAttack = raw (compAttack); p.compRelease = raw (compRelease); p.compAuto = raw (compAuto); p.compMakeup = raw (compMakeup);
    p.compMix = raw (compMix); p.compScHp = raw (compScHp); p.compAutoGain = raw (compAutoGain);
    p.hpfOn = raw (hpfOn); p.hpfFreq = raw (hpfFreq); p.hpfSlope = raw (hpfSlope);
    p.lvlOn = raw (lvlOn); p.lvlTarget = raw (lvlTarget); p.lvlRange = raw (lvlRange); p.lvlSpeed = raw (lvlSpeed);
    p.proxOn = raw (proxOn); p.proxAmount = raw (proxAmount); p.warmth = raw (warmth);
}

LiveVoiceProcessor::~LiveVoiceProcessor()
{
    detector.release();
}

// Supported layouts:
//   mono in  -> 2 ch out : channel 1 = FOH (processed), channel 2 = Monitor (solo / A/B)   <- default
//   mono in  -> mono out : FOH only
//   stereo in -> stereo out : processed stereo, no monitor channel
bool LiveVoiceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in == juce::AudioChannelSet::mono())
        return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return in == juce::AudioChannelSet::stereo() && out == juce::AudioChannelSet::stereo();
}

void LiveVoiceProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int nCh = std::max (1, getTotalNumOutputChannels());

    setLatencySamples (0);

    feedback.prepare (sampleRate, nCh);
    dynEq.prepare (sampleRate, nCh);
    deEsser.prepare (sampleRate, nCh);
    pse.prepare (sampleRate, nCh);
    comp.prepare (sampleRate, nCh);
    proximity.prepare (sampleRate, nCh);
    highPass.prepare (sampleRate, nCh);
    leveler.prepare (sampleRate, nCh);
    warmthFx.prepare (sampleRate, nCh);
    detector.prepare (sampleRate);

    monoScratch.assign ((size_t) std::max (512, samplesPerBlock), 0.0f);
    dryBuf.setSize (2, std::max (512, samplesPerBlock));
    soloBuf.setSize (2, std::max (512, samplesPerBlock));

    inGainSmooth.reset (sampleRate, 0.02);
    outGainSmooth.reset (sampleRate, 0.02);
    inGainSmooth.setCurrentAndTargetValue (dsp::dbToGain (p.inGain->load()));
    outGainSmooth.setCurrentAndTargetValue (dsp::dbToGain (p.outGain->load()));

    updateSettings();
}

void LiveVoiceProcessor::releaseResources()
{
    detector.release();
}

float LiveVoiceProcessor::getNotchQ() const
{
    // Q for a bandwidth of N octaves: sqrt(2^N) / (2^N - 1)
    static constexpr float qs[] { 7.2f, 14.4f, 28.9f, 57.7f };   // 1/5, 1/10, 1/20, 1/40 octave
    return qs[juce::jlimit (0, 3, (int) p.fbWidth->load())];
}

void LiveVoiceProcessor::updateSettings()
{
    dsp::FeedbackSuppressor::Settings fs;
    fs.on = p.fbOn->load() > 0.5f;
    fs.freeze = p.fbFreeze->load() > 0.5f;
    fs.maxFixed = (int) p.fbMaxFixed->load();
    fs.maxFloat = (int) p.fbMaxFloat->load();
    fs.maxDepthDb = p.fbMaxDepth->load();
    fs.q = getNotchQ();
    fs.holdSeconds = p.fbHold->load();
    feedback.setSettings (fs);

    dsp::FeedbackDetector::Settings ds;
    ds.enabled = fs.on && p.bypass->load() < 0.5f;
    ds.learn = p.fbLearn->load() > 0.5f;
    ds.freeze = fs.freeze;
    ds.mode = (int) p.fbMode->load();
    ds.sensitivity = p.fbSens->load();
    ds.loFreq = p.fbLo->load();
    ds.hiFreq = p.fbHi->load();
    detector.setSettings (ds);

    for (int b = 0; b < params::numEqBands; ++b)
    {
        const auto& d = p.bands[(size_t) b];
        dsp::DynamicEq::BandSettings s;
        s.on = d.on->load() > 0.5f;
        const auto shape = params::bandShapeFromChoice (b, d.type != nullptr ? (int) d.type->load() : 0);
        s.type = shape.type; s.slope = shape.slope;
        s.freq = d.freq->load(); s.gainDb = d.gain->load(); s.q = d.q->load();
        s.dynamic = d.dyn->load() > 0.5f;
        s.thresholdDb = d.thr->load(); s.rangeDb = d.range->load(); s.ratio = d.ratio->load();
        s.attackMs = d.att->load(); s.releaseMs = d.rel->load();
        dynEq.setBand (b, s);
    }

    dsp::DeEsser::Settings de;
    de.on = p.dsOn->load() > 0.5f;
    de.mode = juce::jlimit (0, 2, (int) p.dsMode->load());
    de.freq = p.dsFreq->load(); de.q = p.dsQ->load(); de.thresholdDb = p.dsThr->load();
    de.rangeDb = p.dsRange->load(); de.releaseMs = p.dsRel->load();
    de.relative = p.dsRelative->load() > 0.5f; de.relThresholdDb = p.dsRelThr->load();
    de.listen = deEssListen.load();
    deEsser.setSettings (de);

    dsp::PrimarySourceExpander::Settings ps;
    ps.on = p.pseOn->load() > 0.5f;
    ps.thresholdDb = p.pseThr->load(); ps.rangeDb = p.pseRange->load(); ps.attackMs = p.pseAttack->load();
    ps.holdMs = p.pseHold->load(); ps.releaseMs = p.pseRelease->load();
    ps.scHpHz = p.pseScHp->load(); ps.scLpHz = p.pseScLp->load();
    pse.setSettings (ps);

    dsp::VocalCompressor::Settings cs;
    cs.on = p.compOn->load() > 0.5f;
    cs.thresholdDb = p.compThr->load(); cs.ratio = p.compRatio->load(); cs.kneeDb = p.compKnee->load();
    cs.attackMs = p.compAttack->load(); cs.releaseMs = p.compRelease->load(); cs.autoRelease = p.compAuto->load() > 0.5f;
    cs.makeupDb = p.compMakeup->load(); cs.mix = p.compMix->load() * 0.01f; cs.scHpHz = p.compScHp->load();
    cs.autoGain = p.compAutoGain->load() > 0.5f;
    comp.setSettings (cs);

    highPass.setSettings ({ p.hpfOn->load() > 0.5f, p.hpfFreq->load(), juce::jlimit (0, 5, (int) p.hpfSlope->load()) });
    proximity.setSettings ({ p.proxOn->load() > 0.5f, p.proxAmount->load() });
    dsp::AutoLeveler::Settings ls;
    ls.on = p.lvlOn->load() > 0.5f; ls.targetDb = p.lvlTarget->load(); ls.rangeDb = p.lvlRange->load();
    ls.speed = juce::jlimit (0, 2, (int) p.lvlSpeed->load());
    leveler.setSettings (ls);
    warmthFx.setAmount (p.warmth->load());
}

bool LiveVoiceProcessor::isMonitorBusActive() const
{
    return getTotalNumInputChannels() == 1 && getTotalNumOutputChannels() >= 2;
}

void LiveVoiceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    // processing runs on the input channels (1 = mono voice, 2 = stereo); with a mono input and
    // 2 output channels, output channel 2 is the Monitor feed
    const int nIn = juce::jlimit (1, 2, std::min (getTotalNumInputChannels(), buffer.getNumChannels()));
    const bool monitorOn = isMonitorBusActive() && buffer.getNumChannels() >= 2;
    juce::AudioBuffer<float> main (buffer.getArrayOfWritePointers(), nIn, numSamples);
    juce::AudioBuffer<float> mon;
    if (monitorOn) mon = juce::AudioBuffer<float> (buffer.getArrayOfWritePointers() + 1, 1, numSamples);

    const int nCh = nIn;
    if (numSamples == 0 || nCh == 0)
        return;

    // growing blocks (some hosts exceed the prepared size): resize outside the hot path rarely
    if (numSamples > dryBuf.getNumSamples())
    {
        dryBuf.setSize (2, numSamples, false, false, true);
        soloBuf.setSize (2, numSamples, false, false, true);
    }
    juce::AudioBuffer<float> dry (dryBuf.getArrayOfWritePointers(), nCh, numSamples);
    juce::AudioBuffer<float> soloSig (soloBuf.getArrayOfWritePointers(), nCh, numSamples);
    for (int ch = 0; ch < nCh; ++ch) dry.copyFrom (ch, 0, main, ch, 0, numSamples);   // untouched input for A/B

    updateSettings();

    auto tapMono = [&] (const juce::AudioBuffer<float>& src, auto&& sink)
    {
        const int chunk = (int) monoScratch.size();
        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = std::min (chunk, numSamples - start);
            auto* m = monoScratch.data();
            const float* l = src.getReadPointer (0, start);
            if (nCh > 1)
            {
                const float* r = src.getReadPointer (1, start);
                for (int i = 0; i < n; ++i) m[i] = 0.5f * (l[i] + r[i]);
            }
            else
                std::copy (l, l + n, m);
            sink (m, n);
        }
    };

    auto storePeak = [] (std::atomic<float>& meter, float peak)
    {
        const float db = dsp::gainToDb (peak);
        if (db > meter.load (std::memory_order_relaxed)) meter.store (db, std::memory_order_relaxed);
    };

    auto blockPeak = [&] (const juce::AudioBuffer<float>& b)
    {
        float pk = 0.0f;
        for (int ch = 0; ch < nCh; ++ch) pk = std::max (pk, b.getMagnitude (ch, 0, numSamples));
        return pk;
    };

    auto writeMonitor = [&] (const juce::AudioBuffer<float>& src)
    {
        if (! monitorOn) return;
        for (int ch = 0; ch < mon.getNumChannels(); ++ch)
        {
            mon.copyFrom (ch, 0, src, std::min (ch, nCh - 1), 0, numSamples);
            juce::FloatVectorOperations::clip (mon.getWritePointer (ch), mon.getReadPointer (ch), -1.0f, 1.0f, numSamples);
        }
    };

    if (p.bypass->load() > 0.5f)
    {
        tapMono (main, [this] (const float* m, int n) { inputTap.push (m, n); outputTap.push (m, n); });
        const float pk = blockPeak (main);
        storePeak (inputPeakDb, pk);
        storePeak (outputPeakDb, pk);
        writeMonitor (main);
        return;
    }

    // ---- input trim ----
    inGainSmooth.setTargetValue (dsp::dbToGain (p.inGain->load()));
    if (inGainSmooth.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = inGainSmooth.getNextValue();
            for (int ch = 0; ch < nCh; ++ch) main.getWritePointer (ch)[i] *= g;
        }
    }
    else
        main.applyGain (0, numSamples, inGainSmooth.getTargetValue());

    storePeak (inputPeakDb, blockPeak (main));
    tapMono (main, [this] (const float* m, int n) { inputTap.push (m, n); });

    // ---- processing chain (all zero latency) ----
    feedCapture (main, numSamples, CaptureTarget::eq);   // before the HPF, so EQ Suggest can judge where it belongs
    highPass.process (main, numSamples);
    pse.process (main, numSamples);
    feedback.process (main, numSamples);

    const bool eqOnNow = p.eqOn->load() > 0.5f;
    if (eqOnNow && ! eqWasOn) dynEq.reset();
    eqWasOn = eqOnNow;
    if (eqOnNow) dynEq.process (main, numSamples);

    // ---- solo taps: the main path is never altered by soloing ----
    const int solo = soloTarget.load();
    bool soloActive = applyEqSolo (main, soloSig, numSamples, solo);

    feedCapture (main, numSamples, CaptureTarget::deEss);
    if (solo == soloDeEss)
    {
        deEsser.process (main, numSamples, &soloSig);
        soloActive = true;
    }
    else
        deEsser.process (main, numSamples);

    proximity.process (main, numSamples);
    if (const int dc = detector.getDetectionCount(); dc != lastDetectCount)
    {
        lastDetectCount = dc;
        leveler.holdBoost (4.0);   // a ring was just found: the leveler must not turn up into it
    }
    leveler.process (main, numSamples);
    warmthFx.process (main, numSamples);
    comp.process (main, numSamples);

    // detector listens to what is actually going to the speakers (pre-solo, pre-output trim)
    tapMono (main, [this] (const float* m, int n) { detector.pushSamples (m, n); });

    // fallback for hosts without monitor routing: solo replaces the main output
    const bool soloOnMain = soloActive && soloToMain.load();
    if (soloOnMain)
        for (int ch = 0; ch < nCh; ++ch) main.copyFrom (ch, 0, soloSig, ch, 0, numSamples);

    // ---- output trim ----
    outGainSmooth.setTargetValue (dsp::dbToGain (p.outGain->load()));
    if (outGainSmooth.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outGainSmooth.getNextValue();
            for (int ch = 0; ch < nCh; ++ch) main.getWritePointer (ch)[i] *= g;
        }
    }
    else
        main.applyGain (0, numSamples, outGainSmooth.getTargetValue());

    // ---- level match: gated ~3 s loudness of the raw input vs the output ----
    {
        double eIn = 0.0, eOut = 0.0;
        for (int ch = 0; ch < nCh; ++ch)
        {
            const float* a = dry.getReadPointer (ch);
            const float* b = main.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i) { eIn += (double) a[i] * a[i]; eOut += (double) b[i] * b[i]; }
        }
        eIn /= (double) (numSamples * nCh);
        eOut /= (double) (numSamples * nCh);
        if (const float adj = levelAdjustDb.exchange (0.0f); std::abs (adj) > 1.0e-4f)
            powOut *= std::pow (10.0, adj / 10.0);
        if (eIn > 1.0e-6)   // only while someone is talking (above about -60 dBFS)
        {
            const double a = std::exp (-(double) numSamples / (3.0 * currentSampleRate.load()));
            powIn = a * powIn + (1.0 - a) * eIn;
            powOut = a * powOut + (1.0 - a) * eOut;
            loudIn.store ((float) (10.0 * std::log10 (powIn + 1.0e-12)), std::memory_order_relaxed);
            loudOut.store ((float) (10.0 * std::log10 (powOut + 1.0e-12)), std::memory_order_relaxed);
        }
    }

    storePeak (outputPeakDb, blockPeak (main));
    tapMono (main, [this] (const float* m, int n) { outputTap.push (m, n); });

    // ---- monitor output: solo, or A/B between processed and dry ----
    if (soloActive && ! soloOnMain) writeMonitor (soloSig);
    else if (monitorDry.load())     writeMonitor (dry);
    else                            writeMonitor (main);
}

bool LiveVoiceProcessor::applyEqSolo (const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst, int numSamples, int solo)
{
    if (! juce::isPositiveAndBelow (solo, params::numEqBands))
    {
        lastSoloBand = -1;
        return false;
    }

    const auto& d = p.bands[(size_t) solo];
    const auto shape = params::bandShapeFromChoice (solo, d.type != nullptr ? (int) d.type->load() : 0);
    if (params::isPassType (shape.type))
        return false;   // nothing meaningful to solo on a cut filter

    const double fs = currentSampleRate.load();
    const float f = d.freq->load(), q = d.q->load();
    if (shape.type == params::BandType::lowShelf)       soloCoeffs.lowPass (f, 0.707, fs);
    else if (shape.type == params::BandType::highShelf) soloCoeffs.highPass (f, 0.707, fs);
    else                                                soloCoeffs.bandPass (f, std::max (0.7f, q), fs);

    if (solo != lastSoloBand) { for (auto& st : soloState) st.reset(); lastSoloBand = solo; }

    const int nCh = std::min (src.getNumChannels(), dst.getNumChannels());
    for (int ch = 0; ch < nCh; ++ch)
    {
        const auto* x = src.getReadPointer (ch);
        auto* y = dst.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i) y[i] = soloState[(size_t) ch].process (x[i], soloCoeffs);
    }
    return true;
}

// ---------------------------------------------------------------- capture (Suggest)
void LiveVoiceProcessor::feedCapture (const juce::AudioBuffer<float>& b, int numSamples, CaptureTarget at) noexcept
{
    if (captureState.load (std::memory_order_acquire) != 1 || captureTarget.load (std::memory_order_relaxed) != (int) at)
        return;
    int pos = capturePos.load (std::memory_order_relaxed);
    const int cap = (int) captureData.size();
    const int n = std::min (numSamples, cap - pos);
    const float* l = b.getReadPointer (0);
    const float* r = b.getNumChannels() > 1 ? b.getReadPointer (1) : nullptr;
    for (int i = 0; i < n; ++i) captureData[(size_t) (pos + i)] = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
    pos += n;
    capturePos.store (pos, std::memory_order_release);
    if (pos >= cap) captureState.store (2, std::memory_order_release);
}

void LiveVoiceProcessor::startCapture (CaptureTarget t, double seconds)
{
    captureState.store (0);
    captureData.assign ((size_t) (seconds * currentSampleRate.load()), 0.0f);   // allocate on the message thread
    capturePos.store (0);
    captureTarget.store ((int) t);
    captureState.store (1, std::memory_order_release);
}

void LiveVoiceProcessor::cancelCapture()
{
    captureState.store (0);
    captureTarget.store (0);
}

float LiveVoiceProcessor::getCaptureProgress() const noexcept
{
    if (captureData.empty()) return 0.0f;
    return (float) capturePos.load() / (float) captureData.size();
}

std::vector<float> LiveVoiceProcessor::takeCapture()
{
    std::vector<float> out;
    if (captureState.load() == 2) out.swap (captureData);
    captureState.store (0);
    captureTarget.store (0);
    return out;
}

// ---------------------------------------------------------------- feedback log
void LiveVoiceProcessor::pollFeedbackLog()
{
    dsp::FeedbackDetector::LogEvent e;
    while (detector.popLogEvent (e))
    {
        feedbackLog.push_back ({ juce::Time::getCurrentTime(), e.freq, e.fixed });
        if (feedbackLog.size() > 500) feedbackLog.erase (feedbackLog.begin());
    }
}

// ---------------------------------------------------------------- A/B settings
void LiveVoiceProcessor::selectSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot) return;
    undoManager.beginNewTransaction (slot == 0 ? "Switch to A" : "Switch to B");
    slots[(size_t) activeSlot] = presets.captureValues();
    if (slots[(size_t) slot].empty()) slots[(size_t) slot] = slots[(size_t) activeSlot];   // B starts as a copy of A
    presets.applyValues (slots[(size_t) slot]);
    activeSlot = slot;
}

void LiveVoiceProcessor::copyActiveToOther()
{
    slots[(size_t) (1 - activeSlot)] = presets.captureValues();
}

void LiveVoiceProcessor::matchOutputToInput()
{
    if (! levelMatchValid()) return;
    auto* rp = apvts.getParameter (params::outGain);
    const float current = rp->convertFrom0to1 (rp->getValue());
    const float target = juce::jlimit (-24.0f, 24.0f, current - getOutputVsInputDb());
    undoManager.beginNewTransaction ("Match output level");
    rp->beginChangeGesture();
    rp->setValueNotifyingHost (rp->convertTo0to1 (target));
    rp->endChangeGesture();
    // the measured output moves over ~3 s; tell the audio thread to jump its estimate now
    levelAdjustDb.store (levelAdjustDb.load() + (target - current));
}

void LiveVoiceProcessor::sendNotchCommand (const dsp::FeedbackSuppressor::Command& c)
{
    const juce::SpinLock::ScopedLockType lock (uiCommandLock);
    feedback.pushUiCommand (c);
}

juce::AudioProcessorParameter* LiveVoiceProcessor::getBypassParameter() const
{
    return apvts.getParameter (params::bypass);
}

// ---- programs = factory presets, so hosts can list them too ----
int LiveVoiceProcessor::getNumPrograms()
{
    return presets.getFactoryNames().size();
}

void LiveVoiceProcessor::setCurrentProgram (int index)
{
    if (presets.loadFactory (index))
        currentProgram = index;
}

const juce::String LiveVoiceProcessor::getProgramName (int index)
{
    return presets.getFactoryNames()[index];
}

// ---- state: parameters + learned (fixed) notches + preset name ----
void LiveVoiceProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    auto* notches = xml->createNewChildElement ("FixedNotches");
    for (int i = 0; i < dsp::FeedbackSuppressor::maxSlots; ++i)
    {
        const auto n = feedback.getNotch (i);
        if (n.kind == dsp::FeedbackSuppressor::kindFixed && n.targetDb < -0.01f)
        {
            auto* e = notches->createNewChildElement ("Notch");
            e->setAttribute ("freq", n.freq);
            e->setAttribute ("depth", n.targetDb);
        }
    }

    auto* ui = xml->createNewChildElement ("UiState");
    ui->setAttribute ("preset", presets.getCurrentName());
    ui->setAttribute ("band", selectedBand.load());
    ui->setAttribute ("scale", (double) uiScale.load());
    ui->setAttribute ("soloToMain", soloToMain.load());
    ui->setAttribute ("rtaView", rtaView);
    ui->setAttribute ("rtaSmoothing", rtaSmoothing);
    ui->setAttribute ("rtaTilt", rtaTilt);
    ui->setAttribute ("historySpan", historySpan);
    ui->setAttribute ("abSlot", activeSlot);
    for (int s = 0; s < 2; ++s)
        if (! slots[(size_t) s].empty() && s != activeSlot)   // the active slot is the live parameters
        {
            auto* e = ui->createNewChildElement ("Slot");
            e->setAttribute ("index", s);
            for (auto& [id, v] : slots[(size_t) s]) e->setAttribute (id, (double) v);
        }

    copyXmlToBinary (*xml, destData);
}

void LiveVoiceProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    if (auto* ui = xml->getChildByName ("UiState"))
    {
        presets.setCurrentName (ui->getStringAttribute ("preset", "Default"));
        selectedBand = juce::jlimit (0, params::numEqBands - 1, ui->getIntAttribute ("band", 0));
        if (ui->hasAttribute ("scale"))
        {
            uiScale = (float) juce::jlimit (0.5, 1.5, ui->getDoubleAttribute ("scale", 0.85));
            uiScaleRestored = true;
        }
        soloToMain = ui->getBoolAttribute ("soloToMain", false);
        rtaView = juce::jlimit (0, 2, ui->getIntAttribute ("rtaView", 2));
        rtaSmoothing = juce::jlimit (0, 2, ui->getIntAttribute ("rtaSmoothing", 1));
        rtaTilt = ui->getBoolAttribute ("rtaTilt", false);
        historySpan = juce::jlimit (0, 2, ui->getIntAttribute ("historySpan", 1));
        activeSlot = juce::jlimit (0, 1, ui->getIntAttribute ("abSlot", 0));
        slots = {};
        for (auto* e : ui->getChildWithTagNameIterator ("Slot"))
        {
            const int s = juce::jlimit (0, 1, e->getIntAttribute ("index"));
            for (int i = 0; i < e->getNumAttributes(); ++i)
                if (e->getAttributeName (i) != "index")
                    slots[(size_t) s].emplace_back (e->getAttributeName (i), (float) e->getAttributeValue (i).getDoubleValue());
        }
    }
    soloTarget = soloNone;
    monitorDry = false;

    dsp::FeedbackSuppressor::Command clear;
    clear.type = dsp::FeedbackSuppressor::Command::Type::clearAll;
    sendNotchCommand (clear);

    if (auto* notches = xml->getChildByName ("FixedNotches"))
        for (auto* e : notches->getChildIterator())
        {
            dsp::FeedbackSuppressor::Command c;
            c.type = dsp::FeedbackSuppressor::Command::Type::setFixed;
            c.freq = (float) e->getDoubleAttribute ("freq", 1000.0);
            c.depthDb = (float) e->getDoubleAttribute ("depth", -6.0);
            sendNotchCommand (c);
        }

    xml->deleteAllChildElementsWithTagName ("FixedNotches");
    xml->deleteAllChildElementsWithTagName ("UiState");

    // sessions from before 0.7.0: the high-pass lived on EQ band 1
    {
        std::vector<std::pair<juce::String, float>> values;
        for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
            values.emplace_back (e->getStringAttribute ("id"), (float) e->getDoubleAttribute ("value"));
        const auto before = values.size();
        params::migrateLegacyHighPass (values);
        if (values.size() != before)
        {
            for (auto& [id, v] : values)
            {
                juce::XmlElement* found = nullptr;
                for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
                    if (e->getStringAttribute ("id") == id) { found = e; break; }
                if (found == nullptr) { found = xml->createNewChildElement ("PARAM"); found->setAttribute ("id", id); }
                found->setAttribute ("value", (double) v);
            }
        }
    }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* LiveVoiceProcessor::createEditor()
{
    return new LiveVoiceEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LiveVoiceProcessor();
}
