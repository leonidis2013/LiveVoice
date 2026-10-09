#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"

// Top display: input (grey fill) / output (blue fill) RTA, dynamic EQ curve with
// draggable band handles, feedback notch curve with markers, and the de-esser band.
//
// Drawing is cached: the grid/labels live in an image that is rebuilt only on
// resize or theme change, and the EQ / notch curves are recomputed only when a
// setting or a dynamic gain actually changes.
class RtaDisplay : public juce::Component
{
public:
    explicit RtaDisplay (LiveVoiceProcessor& p);

    void update();                      // called by the editor's timer
    void setSelectedBand (int b)        { selectedBand = b; repaint(); }
    void invalidateCaches()             { gridDirty = true; eqDirty = true; notchDirty = true; repaint(); }
    std::function<void (int)> onBandSelected;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    static constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    static constexpr float specTop = 0.0f, specBottom = -96.0f;
    static constexpr float eqRange = 24.0f;

    float xForFreq (float f) const;
    float freqForX (float x) const;
    float yForSpecDb (float db) const;
    float yForEqDb (float db) const;
    float eqDbForY (float y) const;

    void computeSpectrum (ScopeTap& tap, std::vector<float>& columns);
    void bandResponse (int band, bool includeDynamic, std::vector<float>& out) const;
    void refreshCurves();
    juce::Point<float> handlePosition (int band) const;
    int hitBand (juce::Point<float> pos) const;
    juce::Point<float> hpfHandlePosition() const;
    bool hitHpf (juce::Point<float> pos) const;
    int hitNotch (juce::Point<float> pos) const;
    juce::RangedAudioParameter* param (const juce::String& id) const;
    params::BandType bandType (int band) const;
    void setParam (const juce::String& id, float realValue);
    void updateButtons();

    void renderGrid();
    void drawDeEss (juce::Graphics&);
    void drawEqSolo (juce::Graphics&);
    void drawNotchMarkers (juce::Graphics&);
    void drawHandles (juce::Graphics&);

    LiveVoiceProcessor& proc;
    juce::Rectangle<float> plot;

    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftBuffer, binMag;
    std::vector<float> inCols, outCols;       // one per 2 px column

    // cached curves (dB at curveFreqs)
    std::vector<float> curveFreqs, curveX;
    std::array<std::vector<float>, params::numEqBands> bandStatic, bandLive;
    std::array<std::vector<float>, params::numEqBands> bandSig;   // params each band was computed with
    std::array<float, params::numEqBands> bandLiveGain {};
    std::vector<float> eqStatic, eqLive, notchCurve, hpfCurve, hpfSig;
    bool hpfWasOn = false, dragHpf = false, hoverHpf = false;
    std::vector<float> lastNotchSig;
    bool eqDirty = true, notchDirty = true, eqWasOn = false, fbWasOn = false;
    float lastNotchQ = 0.0f;

    juce::Image gridImage;
    bool gridDirty = true;

    std::array<dsp::FeedbackSuppressor::NotchInfo, dsp::FeedbackSuppressor::maxSlots> notches;
    std::array<float, params::numEqBands> dynGain {};

    juce::TextButton viewIn { "IN" }, viewOut { "OUT" }, viewBoth { "BOTH" }, smoothBtn { "1/6 oct" }, tiltBtn { "TILT" };
    juce::TextButton soloBanner;

    int selectedBand = 0, dragBand = -1, hoverBand = -1, hoverNotch = -1;
    int lastDetectCount = 0, soloFlash = 0;
    float detectFlash = 0.0f, detectFreq = 0.0f;
};
