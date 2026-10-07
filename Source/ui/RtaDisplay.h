#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"

// Top display: input/output RTA, dynamic EQ curve with draggable band
// handles, and the feedback notch curve with notch markers.
class RtaDisplay : public juce::Component
{
public:
    explicit RtaDisplay (LiveVoiceProcessor& p);

    void update();                      // called by the editor's timer
    void setSelectedBand (int b)        { selectedBand = b; repaint(); }
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
    float bandEqDb (int band, double freq, bool includeDynamic) const;
    juce::Point<float> handlePosition (int band) const;
    int hitBand (juce::Point<float> pos) const;
    int hitNotch (juce::Point<float> pos) const;
    juce::RangedAudioParameter* param (const juce::String& id) const;
    params::BandType bandType (int band) const;
    void setParam (const juce::String& id, float realValue);

    void drawGrid (juce::Graphics&);
    void drawCurves (juce::Graphics&);
    void drawNotchMarkers (juce::Graphics&);
    void drawHandles (juce::Graphics&);

    LiveVoiceProcessor& proc;
    juce::Rectangle<float> plot;

    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftBuffer, binDb;
    std::vector<float> inCols, outCols;

    std::array<dsp::FeedbackSuppressor::NotchInfo, dsp::FeedbackSuppressor::maxSlots> notches;
    std::array<float, params::numEqBands> dynGain {};

    int selectedBand = 0, dragBand = -1, hoverBand = -1, hoverNotch = -1;
    int lastDetectCount = 0;
    float detectFlash = 0.0f, detectFreq = 0.0f;
};
