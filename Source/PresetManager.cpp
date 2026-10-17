// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki

#include "PresetManager.h"
#include "Params.h"

namespace
{
    const juce::String presetExtension { ".lvpreset" };
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state)
    : apvts (state), factory (buildFactoryPresets())
{
    getUserPresetDirectory().createDirectory();
}

juce::File PresetManager::getUserPresetDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("LiveVoice").getChildFile ("Presets");
}

bool PresetManager::isPresetParameter (const juce::String& id)
{
    static const juce::StringArray excluded { params::inGain, params::outGain,
                                              params::bypass, params::fbLearn, params::fbFreeze };
    return ! excluded.contains (id);
}

juce::StringArray PresetManager::getFactoryNames() const
{
    juce::StringArray names;
    for (auto& f : factory) names.add (f.name);
    return names;
}

juce::StringArray PresetManager::getUserNames() const
{
    juce::StringArray names;
    for (auto& f : getUserPresetDirectory().findChildFiles (juce::File::findFiles, false, "*" + presetExtension))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    return names;
}

PresetManager::Values PresetManager::captureValues() const
{
    Values v;
    for (auto* p : apvts.processor.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (isPresetParameter (rp->getParameterID()))
                v.emplace_back (rp->getParameterID(), rp->convertFrom0to1 (rp->getValue()));
    return v;
}

void PresetManager::applyValues (const Values& original)
{
    Values values;
    for (auto& v : original)
        if (std::isfinite (v.second)) values.push_back (v);   // a corrupt NaN / inf value falls back to the default
    params::migrateLegacyHighPass (values);   // presets / A-B slots saved before 0.7.0
    for (auto* p : apvts.processor.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr || ! isPresetParameter (rp->getParameterID()))
            continue;

        float norm = rp->getDefaultValue();
        for (auto& [id, v] : values)
            if (id == rp->getParameterID()) { norm = rp->convertTo0to1 (v); break; }

        rp->beginChangeGesture();
        rp->setValueNotifyingHost (norm);
        rp->endChangeGesture();
    }
}

bool PresetManager::loadFactory (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) factory.size()))
        return false;
    applyValues (factory[(size_t) index].values);
    currentName = factory[(size_t) index].name;
    return true;
}

bool PresetManager::loadUser (const juce::String& name)
{
    auto file = getUserPresetDirectory().getChildFile (juce::File::createLegalFileName (name) + presetExtension);
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("LiveVoicePreset"))
        return false;

    std::vector<std::pair<juce::String, float>> values;
    for (auto* e : xml->getChildWithTagNameIterator ("Param"))
        values.emplace_back (e->getStringAttribute ("id"), (float) e->getDoubleAttribute ("value"));

    applyValues (values);
    currentName = name;
    return true;
}

bool PresetManager::saveUser (const juce::String& name)
{
    const auto legal = juce::File::createLegalFileName (name.trim());
    if (legal.isEmpty())
        return false;

    juce::XmlElement xml ("LiveVoicePreset");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("name", name.trim());

    for (auto* p : apvts.processor.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            if (isPresetParameter (rp->getParameterID()))
            {
                auto* e = xml.createNewChildElement ("Param");
                e->setAttribute ("id", rp->getParameterID());
                e->setAttribute ("value", rp->convertFrom0to1 (rp->getValue()));
            }

    auto dir = getUserPresetDirectory();
    dir.createDirectory();
    if (! xml.writeTo (dir.getChildFile (legal + presetExtension)))
        return false;

    currentName = name.trim();
    return true;
}

bool PresetManager::deleteUser (const juce::String& name)
{
    auto file = getUserPresetDirectory().getChildFile (juce::File::createLegalFileName (name) + presetExtension);
    return file.existsAsFile() && file.deleteFile();
}

// ---------------------------------------------------------------------------
// Factory presets: sensible starting points, meant to be tweaked at soundcheck.
// Band types: band 1 {0 Bell, 1 Low Shelf, 2 HP12, 3 HP24}; band 6 {0 Bell, 1 High Shelf, 2 LP12, 3 LP24}.
// ---------------------------------------------------------------------------
std::vector<PresetManager::Factory> PresetManager::buildFactoryPresets()
{
    using namespace params;
    using V = std::vector<std::pair<juce::String, float>>;

    auto band = [] (V& v, int b, bool on, float freq, float gain, float q)
    {
        v.push_back ({ bOn (b), on ? 1.0f : 0.0f });
        v.push_back ({ bFreq (b), freq });
        v.push_back ({ bGain (b), gain });
        v.push_back ({ bQ (b), q });
    };
    auto dyn = [] (V& v, int b, float thr, float range, float ratio, float att, float rel)
    {
        v.push_back ({ bDyn (b), 1.0f });
        v.push_back ({ bThr (b), thr });
        v.push_back ({ bRange (b), range });
        v.push_back ({ bRatio (b), ratio });
        v.push_back ({ bAtt (b), att });
        v.push_back ({ bRel (b), rel });
    };
    auto hpf = [] (V& v, float freq, int slopeIndex)
    {
        v.push_back ({ params::hpfOn, 1.0f });
        v.push_back ({ params::hpfFreq, freq });
        v.push_back ({ params::hpfSlope, (float) slopeIndex });
    };
    auto fb = [] (V& v, int mode, float sens, float depth, int width, float hold, float lo, float hi)
    {
        v.push_back ({ fbMode, (float) mode });
        v.push_back ({ fbSens, sens });
        v.push_back ({ fbMaxDepth, depth });
        v.push_back ({ fbWidth, (float) width });
        v.push_back ({ fbHold, hold });
        v.push_back ({ fbLoFreq, lo });
        v.push_back ({ fbHiFreq, hi });
    };
    auto ds = [] (V& v, bool on, float freq, float thr, float range, int mode, float rel)
    {
        v.push_back ({ dsOn, on ? 1.0f : 0.0f });
        v.push_back ({ dsFreq, freq });
        v.push_back ({ dsThr, thr });
        v.push_back ({ dsRange, range });
        v.push_back ({ dsMode, (float) mode });
        v.push_back ({ dsRel, rel });
    };

    auto pse = [] (V& v, bool on, float thr, float range, float hold, float rel, float hp, float lp)
    {
        v.push_back ({ params::pseOn, on ? 1.0f : 0.0f });
        v.push_back ({ params::pseThr, thr });
        v.push_back ({ params::pseRange, range });
        v.push_back ({ params::pseHold, hold });
        v.push_back ({ params::pseRelease, rel });
        v.push_back ({ params::pseScHp, hp });
        v.push_back ({ params::pseScLp, lp });
    };
    auto comp = [] (V& v, float thr, float ratio, float knee, float att, float rel, float makeup, float scHp)
    {
        v.push_back ({ params::compOn, 1.0f });
        v.push_back ({ params::compThr, thr });
        v.push_back ({ params::compRatio, ratio });
        v.push_back ({ params::compKnee, knee });
        v.push_back ({ params::compAttack, att });
        v.push_back ({ params::compRelease, rel });
        v.push_back ({ params::compAuto, 1.0f });
        v.push_back ({ params::compMakeup, makeup });
        v.push_back ({ params::compMix, 100.0f });
        v.push_back ({ params::compScHp, scHp });
    };

    std::vector<Factory> out;

    out.push_back ({ "Default", {} });

    // Every preset uses the same band roles so you always know where to reach:
    // HPF module (rumble) | EQ 1 low shelf | 2 boom/proximity | 3 boxiness | 4 nasal/honk | 5 presence/harshness | 6 air (shelf)
    // hpf slope index {0 6, 1 12, 2 18, 3 24, 4 36, 5 48 dB/oct}; bType band 6 {0 Bell, 1 High Shelf, 2..7 LP 6..48}.

    { // Headworn/face mic on a pastor: aggressive feedback control, tame chest boom
        V v;
        fb (v, 0, 55.0f, -18.0f, 2, 30.0f, 150.0f, 10000.0f);
        hpf (v, 110.0f, 3);
        band (v, 1, true, 220.0f, -1.5f, 1.0f);  dyn (v, 1, -30.0f, -4.0f, 2.0f, 15.0f, 150.0f);
        band (v, 2, true, 450.0f, -1.0f, 1.4f);
        band (v, 3, true, 1000.0f, 0.0f, 1.4f);
        band (v, 4, true, 3000.0f, 1.5f, 1.2f);  dyn (v, 4, -32.0f, -4.0f, 2.0f, 5.0f, 100.0f);
        v.push_back ({ bType (5), 1.0f }); band (v, 5, true, 10000.0f, 1.0f, 0.71f);
        ds (v, true, 6500.0f, -32.0f, 6.0f, 0, 80.0f);
        v.push_back ({ dsRelative, 1.0f });
        pse (v, false, -48.0f, 8.0f, 150.0f, 400.0f, 150.0f, 8000.0f);
        comp (v, -24.0f, 2.5f, 12.0f, 10.0f, 300.0f, 3.0f, 120.0f);
        out.push_back ({ "Pastor - Face Mic (Speech)", v });
    }
    { // Pulpit / lectern gooseneck: more proximity boom to manage
        V v;
        fb (v, 0, 60.0f, -18.0f, 2, 30.0f, 150.0f, 10000.0f);
        hpf (v, 120.0f, 3);
        band (v, 1, true, 200.0f, 0.0f, 1.0f);   dyn (v, 1, -28.0f, -6.0f, 2.5f, 15.0f, 150.0f);
        band (v, 2, true, 450.0f, -1.5f, 1.4f);
        band (v, 3, true, 1000.0f, 0.0f, 1.4f);
        band (v, 4, true, 3000.0f, 2.0f, 1.2f);
        v.push_back ({ bType (5), 1.0f }); band (v, 5, true, 10000.0f, 0.0f, 0.71f);
        ds (v, true, 6000.0f, -30.0f, 6.0f, 0, 80.0f);
        v.push_back ({ dsRelative, 1.0f });
        pse (v, true, -45.0f, 12.0f, 200.0f, 500.0f, 150.0f, 8000.0f);   // pulls the lectern mic down when nobody is at it
        comp (v, -24.0f, 3.0f, 12.0f, 10.0f, 300.0f, 4.0f, 120.0f);
        out.push_back ({ "Spoken Word - Lectern Mic", v });
    }
    { // Handheld male vocal: dynamic boom band tames proximity effect
        V v;
        fb (v, 1, 45.0f, -15.0f, 2, 20.0f, 150.0f, 10000.0f);
        hpf (v, 90.0f, 3);
        band (v, 1, true, 200.0f, 0.0f, 1.0f);   dyn (v, 1, -26.0f, -6.0f, 2.5f, 15.0f, 150.0f);
        band (v, 2, true, 400.0f, -1.5f, 1.4f);
        band (v, 3, true, 1000.0f, -0.5f, 1.4f);
        band (v, 4, true, 3000.0f, 2.0f, 1.2f);  dyn (v, 4, -30.0f, -4.0f, 2.0f, 5.0f, 100.0f);
        v.push_back ({ bType (5), 1.0f }); band (v, 5, true, 12000.0f, 2.0f, 0.71f);
        ds (v, true, 5500.0f, -30.0f, 7.0f, 0, 80.0f);
        pse (v, false, -42.0f, 10.0f, 100.0f, 300.0f, 150.0f, 8000.0f);
        comp (v, -20.0f, 3.0f, 10.0f, 6.0f, 220.0f, 3.0f, 100.0f);
        out.push_back ({ "Male Vocal - Handheld", v });
    }
    { // Handheld female vocal: less boom, watch the 1 kHz honk and 3-4 kHz edge
        V v;
        fb (v, 1, 45.0f, -15.0f, 2, 20.0f, 150.0f, 10000.0f);
        hpf (v, 120.0f, 3);
        band (v, 1, true, 250.0f, 0.0f, 1.0f);   dyn (v, 1, -28.0f, -4.0f, 2.0f, 15.0f, 150.0f);
        band (v, 2, true, 500.0f, -1.0f, 1.4f);
        band (v, 3, true, 1100.0f, -1.0f, 1.4f);
        band (v, 4, true, 3500.0f, 1.5f, 1.2f);  dyn (v, 4, -30.0f, -4.0f, 2.0f, 5.0f, 100.0f);
        v.push_back ({ bType (5), 1.0f }); band (v, 5, true, 12000.0f, 2.0f, 0.71f);
        ds (v, true, 7500.0f, -30.0f, 7.0f, 0, 80.0f);
        pse (v, false, -42.0f, 10.0f, 100.0f, 300.0f, 200.0f, 10000.0f);
        comp (v, -20.0f, 3.0f, 10.0f, 6.0f, 220.0f, 3.0f, 120.0f);
        out.push_back ({ "Female Vocal - Handheld", v });
    }
    { // Worship leader who both speaks and sings
        V v;
        fb (v, 2, 50.0f, -15.0f, 2, 25.0f, 150.0f, 10000.0f);
        hpf (v, 100.0f, 3);
        band (v, 1, true, 220.0f, 0.0f, 1.0f);   dyn (v, 1, -27.0f, -4.0f, 2.0f, 15.0f, 150.0f);
        band (v, 2, true, 450.0f, -1.0f, 1.4f);
        band (v, 3, true, 1000.0f, 0.0f, 1.4f);
        band (v, 4, true, 3000.0f, 1.5f, 1.2f);  dyn (v, 4, -30.0f, -4.0f, 2.0f, 5.0f, 100.0f);
        v.push_back ({ bType (5), 1.0f }); band (v, 5, true, 12000.0f, 1.5f, 0.71f);
        ds (v, true, 6500.0f, -30.0f, 6.0f, 0, 80.0f);
        v.push_back ({ dsRelative, 1.0f });
        pse (v, false, -45.0f, 10.0f, 120.0f, 350.0f, 150.0f, 8000.0f);
        comp (v, -22.0f, 3.0f, 12.0f, 8.0f, 250.0f, 3.0f, 110.0f);
        out.push_back ({ "Worship Leader - Speak & Sing", v });
    }

    return out;
}
