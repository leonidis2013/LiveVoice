#include "PluginProcessor.h"
#include "PluginEditor.h"

LiveVoiceProcessor::LiveVoiceProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                        .withOutput ("Output", juce::AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "LiveVoiceState", params::createLayout()),
      presets (apvts)
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };
    using namespace params;
    p.inGain = raw (inGain); p.outGain = raw (outGain); p.limitOn = raw (limitOn); p.ceiling = raw (ceiling); p.bypass = raw (bypass);
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
}

LiveVoiceProcessor::~LiveVoiceProcessor()
{
    detector.release();
}

bool LiveVoiceProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void LiveVoiceProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    const int nCh = std::max (1, getTotalNumOutputChannels());

    setLatencySamples (0);

    feedback.prepare (sampleRate, nCh);
    dynEq.prepare (sampleRate, nCh);
    deEsser.prepare (sampleRate, nCh);
    limiter.prepare (sampleRate);
    detector.prepare (sampleRate);

    monoScratch.assign ((size_t) std::max (512, samplesPerBlock), 0.0f);

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
        s.type = params::bandTypeFromChoice (b, d.type != nullptr ? (int) d.type->load() : 0);
        s.freq = d.freq->load(); s.gainDb = d.gain->load(); s.q = d.q->load();
        s.dynamic = d.dyn->load() > 0.5f;
        s.thresholdDb = d.thr->load(); s.rangeDb = d.range->load(); s.ratio = d.ratio->load();
        s.attackMs = d.att->load(); s.releaseMs = d.rel->load();
        dynEq.setBand (b, s);
    }

    dsp::DeEsser::Settings de;
    de.on = p.dsOn->load() > 0.5f;
    de.split = (int) p.dsMode->load() == (int) params::DsMode::split;
    de.freq = p.dsFreq->load(); de.thresholdDb = p.dsThr->load();
    de.rangeDb = p.dsRange->load(); de.releaseMs = p.dsRel->load();
    deEsser.setSettings (de);
}

void LiveVoiceProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int nCh = std::min (buffer.getNumChannels(), 2);
    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
    if (numSamples == 0 || nCh == 0)
        return;

    updateSettings();

    auto tapMono = [&] (auto&& sink)
    {
        const int chunk = (int) monoScratch.size();
        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = std::min (chunk, numSamples - start);
            auto* m = monoScratch.data();
            const float* l = buffer.getReadPointer (0, start);
            if (nCh > 1)
            {
                const float* r = buffer.getReadPointer (1, start);
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

    auto blockPeak = [&]
    {
        float pk = 0.0f;
        for (int ch = 0; ch < nCh; ++ch) pk = std::max (pk, buffer.getMagnitude (ch, 0, numSamples));
        return pk;
    };

    if (p.bypass->load() > 0.5f)
    {
        tapMono ([this] (const float* m, int n) { inputTap.push (m, n); outputTap.push (m, n); });
        const float pk = blockPeak();
        storePeak (inputPeakDb, pk);
        storePeak (outputPeakDb, pk);
        return;
    }

    // ---- input trim ----
    inGainSmooth.setTargetValue (dsp::dbToGain (p.inGain->load()));
    if (inGainSmooth.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = inGainSmooth.getNextValue();
            for (int ch = 0; ch < nCh; ++ch) buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
        buffer.applyGain (0, numSamples, inGainSmooth.getTargetValue());

    storePeak (inputPeakDb, blockPeak());
    tapMono ([this] (const float* m, int n) { inputTap.push (m, n); });

    // ---- processing chain (all zero latency) ----
    feedback.process (buffer, numSamples);

    const bool eqOnNow = p.eqOn->load() > 0.5f;
    if (eqOnNow && ! eqWasOn) dynEq.reset();
    eqWasOn = eqOnNow;
    if (eqOnNow) dynEq.process (buffer, numSamples);

    deEsser.process (buffer, numSamples);

    // detector listens to what is actually going to the speakers
    tapMono ([this] (const float* m, int n) { detector.pushSamples (m, n); });

    // ---- output trim + safety limiter ----
    outGainSmooth.setTargetValue (dsp::dbToGain (p.outGain->load()));
    if (outGainSmooth.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outGainSmooth.getNextValue();
            for (int ch = 0; ch < nCh; ++ch) buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
        buffer.applyGain (0, numSamples, outGainSmooth.getTargetValue());

    limiter.process (buffer, numSamples, p.limitOn->load() > 0.5f, p.ceiling->load());

    storePeak (outputPeakDb, blockPeak());
    tapMono ([this] (const float* m, int n) { outputTap.push (m, n); });
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
    }

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
