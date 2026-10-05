#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// Full-width spectrum and band panel. The ALL column sits at the far left (General mode,
// the average delay of the active bands), followed by equal-width columns for the active bands;
// a column's height is the band's delay value (two thin side-by-side bars if L and R differ).
// Behind them is the live input spectrum on a 10-octave log-frequency axis, and on top the
// region removed by low/high cut is shaded. The S buttons next to the labels toggle solo.
//
// A click selects the band (or ALL) (onBandSelected). Vertical dragging changes the delay of
// the grabbed bar: with Link on, L and R move together to the same value; with Link off, the
// left half of the column is L and the right half is R. On a channel with sync on the bar shows
// the division's ms value and dragging picks the nearest note division. Double-clicking a column
// resets the band's delay, division and feedback.
class BandSpectrumComponent final : public juce::Component,
                                    private juce::Timer
{
public:
    static constexpr int allBands = -1;   // instead of a selected band: General mode (ALL column)

    explicit BandSpectrumComponent (MultiwayAudioProcessor&);
    ~BandSpectrumComponent() override;

    void setSelectedBand (int band);
    void setLinked (bool shouldBeLinked) noexcept   { linked = shouldBeLinked; }

    std::function<void (int)> onBandSelected;

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    using Processor = MultiwayAudioProcessor;

    static constexpr int numBands  = Processor::numBands;
    static constexpr int fftOrder  = 12;
    static constexpr int fftSize   = 1 << fftOrder;     // 4096, display only
    static constexpr int numBins   = fftSize / 2 + 1;
    static constexpr float minDb   = -84.0f;
    static constexpr float maxDb   = 0.0f;

    void timerCallback() override;
    void computeSpectrum();

    juce::Rectangle<float> getPlotArea() const;
    juce::Rectangle<float> getAllColumnArea() const;
    float frequencyToX (float hz, juce::Rectangle<float> plot) const;
    float xToFrequency (float x, juce::Rectangle<float> plot) const;
    juce::Rectangle<float> getColumnBounds (int band, juce::Rectangle<float> plot) const;
    juce::Rectangle<float> getSoloButtonBounds (int band, juce::Rectangle<float> plot) const;
    int getBandAt (float x, juce::Rectangle<float> plot) const;
    int getSoloButtonAt (juce::Point<float>, juce::Rectangle<float> plot) const;
    float getDelayMs (int band, Processor::BandParam side) const;
    juce::RangedAudioParameter* getDelayTarget (int band, Processor::BandParam side) const;
    float msToNormalised (const juce::RangedAudioParameter&, float ms) const;
    juce::String delayText (int band, Processor::BandParam side) const;
    bool isSoloed (int band) const;

    void drawSpectrum (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawColumn (juce::Graphics&, juce::Rectangle<float> column,
                     float delayL, float delayR, bool selected, bool highlighted) const;
    void drawBands (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawAllColumn (juce::Graphics&) const;
    void drawCutShading (juce::Graphics&, juce::Rectangle<float> plot) const;
    void drawBandLabels (juce::Graphics&, juce::Rectangle<float> plot) const;
    void resetBand (int band);

    Processor& processor;

    std::array<juce::RangedAudioParameter*, numBands> delayLParams {}, delayRParams {}, feedbackParams {};
    std::array<juce::RangedAudioParameter*, numBands> divisionLParams {}, divisionRParams {};
    std::atomic<float>* lowCutParam  = nullptr;
    std::atomic<float>* highCutParam = nullptr;

    // Spectrum: samples pulled from SpectrumTap are written to a circular buffer,
    // and the last fftSize samples are analysed on every timer tick.
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> ring, fftData, displayDb;
    int ringPosition = 0;

    int selectedBand = 0;
    bool linked = false;

    // Dragging: the grabbed bar's starting value (ms) and the parameters to change (two with Link);
    // on a channel with sync on the parameter is the note division.
    struct Drag
    {
        std::array<juce::RangedAudioParameter*, 2> params {};
        int numParams = 0;
        float startMs = 0.0f;
    };
    Drag drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandSpectrumComponent)
};
