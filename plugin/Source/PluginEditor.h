/*
  ==============================================================================

    Multiway editor: Time panel (delay/feedback of the selected band or, in General
    mode, of all active bands, an ms | RATE switch for L / R and the global
    cut/mix knobs), Shaper panel (the LFO of the clicked Time knob: shape,
    pencil drawing and LFO row), Settings row (Bands, Resolution, Random,
    Reset, undo) and the full-width spectrum / band panel.

    The look comes from the Clipotype family: ClipotypeLookAndFeel, PillToggleButton and the
    top-left BopsAudio badge (BrandBadge) are shared from the Clipotype source; colours live in MultiwayPalette.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ClipotypeLookAndFeel.h"
#include "PillToggleButton.h"
#include "BrandBadge.h"
#include "BandSpectrumComponent.h"

//==============================================================================
class MultiwayAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit MultiwayAudioProcessorEditor (MultiwayAudioProcessor&);
    ~MultiwayAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using Processor = MultiwayAudioProcessor;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    // isUserEditing() returns true while the user holds the knob with the mouse or turns it with
    // the wheel. Link writes to the opposite channel only in that case; rebinding on band change
    // or host automation doesn't change the other channel. onPress: the knob was pressed
    // (on modulatable knobs it switches the Shaper to that knob's LFO).
    class KnobSlider final : public juce::Slider
    {
    public:
        bool isUserEditing() const noexcept   { return userEditing; }

        std::function<void()> onPress;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (onPress != nullptr)
                onPress();

            userEditing = true;
            Slider::mouseDown (e);
        }

        void mouseUp (const juce::MouseEvent& e) override     { Slider::mouseUp (e); userEditing = false; }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            userEditing = true;
            Slider::mouseWheelMove (e, wheel);
            userEditing = false;
        }

        // A double-click (return to default) arrives after mouseUp; it is a user edit too.
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            userEditing = true;
            Slider::mouseDoubleClick (e);
            userEditing = false;
        }

    private:
        bool userEditing = false;
    };

    // Small two-option switch ("ms | RATE"): the selected side is a half-pill filled with the accent colour.
    // The clicked side gets selected; onChange is only called when the user changes it.
    class SegmentSwitch final : public juce::Component
    {
    public:
        SegmentSwitch (juce::String leftText, juce::String rightText);

        bool isRightSelected() const noexcept   { return rightSelected; }
        void setRightSelected (bool shouldSelectRight);

        std::function<void (bool)> onChange;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::String leftText, rightText;
        bool rightSelected = false;
    };

    // Shaper drawing area: the selected LFO's shape (sine or drawn) and its current phase. With the pencil
    // on, freehand drawing is sampled into the 128-point table; it goes to the audio thread while drawing
    // and is written to state when the mouse is released.
    class ShapeDisplay final : public juce::Component
    {
    public:
        explicit ShapeDisplay (MultiwayAudioProcessor& p) : processor (p) {}

        void setLfo (int newLfo);
        void refresh();     // from the timer: phase marker, externally changed shape or pencil

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> getPlotArea() const;
        void drawTo (juce::Point<float> position);

        MultiwayAudioProcessor& processor;
        int lfo = 0;
        MultiwayLfo::Table drawn {};       // drawn shape (copy of the processor's table)
        bool usesDrawn = false;
        int lastIndex = -1;                // previous point while dragging; points in between are filled linearly
        float lastValue = 0.0f;
        double phase = 0.0;
        float modulation = 0.0f;
        juce::uint32 shapeVersion = 0;
    };

    // Knob with its name and value (with unit) beneath it.
    struct Knob
    {
        KnobSlider slider;
        juce::Label nameLabel, valueLabel;
        std::function<juce::String (double)> format;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void configureKnob (Knob&, const juce::String& name, std::function<juce::String (double)> format);
    void updateValueLabel (Knob&);
    void layoutKnob (Knob&, juce::Rectangle<int> cell);
    void layoutSmallKnob (Knob&, juce::Rectangle<int> cell);

    bool isGeneralMode() const noexcept   { return selectedBand == BandSpectrumComponent::allBands; }

    void selectBand (int band);
    void bindBandKnobs();
    void refreshGeneralKnobs();
    void setLinked (bool shouldBeLinked);
    void bandCountChanged (int choiceIndex);
    void fftSizeChanged (int choiceIndex);

    // Shaper / LFO: the clicked knob's LFO is selected; the LFO knobs and Hz | RATE are rebound
    // to that LFO's parameters.
    void selectLfo (int lfo);
    void bindLfoKnobs();
    void setLfoPen (bool shouldUseDrawn);
    float getModulatedProportion (int lfo) const;
    juce::Rectangle<int> getRingArea (int lfo) const;
    void drawModulationRing (juce::Graphics&, int lfo) const;

    // Tempo sync: on a channel with sync on the L / R knob is bound to the band's note division, otherwise to ms.
    bool isSynced (Processor::BandParam side) const noexcept   { return syncStates[side == Processor::delayRParam ? 1 : 0]; }
    juce::String delayParamID (int band, Processor::BandParam side) const;
    void setSync (Processor::BandParam side, bool shouldSync);
    void syncChanged();
    void updateClampWarnings();

    // Parameters written outside the attachment when the user turns a band knob:
    // all active bands in General mode, plus the opposite channel if Link is on.
    juce::Array<juce::RangedAudioParameter*> getKnobTargets (Processor::BandParam side) const;
    void bandKnobChanged (Knob&, Processor::BandParam side);
    void beginKnobGesture (Processor::BandParam side);
    void endKnobGesture();
    void writeParameter (juce::RangedAudioParameter&, float normalisedValue);

    void randomise();
    void resetAll();
    void undoLast();
    void storeUndoSnapshot();
    void setUndoAvailable (bool);

    void timerCallback() override;
    void generateNoiseTexture();

    Processor& processorRef;
    ClipotypeLookAndFeel clipotypeLookAndFeel;
    juce::Typeface::Ptr titleTypeface;
    juce::Image noiseTexture;

    juce::Label titleLabel;
    BrandBadge brandBadge;   // top-left BopsAudio header (logo + "BOPSAUDIO" / version), opens bopsaudio.com on click

    Knob delayLKnob, delayRKnob, feedbackKnob;   // selected band
    Knob lowCutKnob, highCutKnob, mixKnob;       // global

    PillToggleButton linkButton;
    SegmentSwitch syncLSwitch { "ms", "RATE" }, syncRSwitch { "ms", "RATE" };
    std::array<std::unique_ptr<juce::ParameterAttachment>, 2> syncAttachments;   // syncL, syncR
    std::array<bool, 2> syncStates {};      // from the attachment; the processor's atomic may still be stale depending on listener order
    std::array<bool, 2> clampWarnings {};   // division exceeds maxDelayMs (L, R): warning next to the value
    std::vector<juce::RangedAudioParameter*> gestureParams;   // parameters whose gesture stays open while a knob is dragged

    // Settings row
    juce::Label bandsLabel, resolutionLabel;
    std::array<juce::TextButton, Processor::bandCountChoices.size()> bandCountButtons;
    std::array<juce::TextButton, Processor::fftSizeChoices.size()> resolutionButtons;
    juce::TextButton randomButton, resetButton;
    juce::ShapeButton undoButton { "undo", {}, {}, {} };
    std::unique_ptr<juce::ParameterAttachment> bandsAttachment, fftSizeAttachment;

    // State before the last Random / Reset (single-step undo); empty means there is nothing to undo.
    std::vector<std::pair<juce::RangedAudioParameter*, float>> undoSnapshot;
    juce::Random random;

    // Shaper: drawing area + pencil button and the selected LFO's row (Rate, Offset, Jitter, Smooth, Amount).
    ShapeDisplay shapeDisplay;
    juce::ShapeButton pencilButton { "pencil", {}, {}, {} };
    Knob lfoRateKnob, lfoOffsetKnob, lfoJitterKnob, lfoSmoothKnob, lfoAmountKnob;
    SegmentSwitch lfoRateSwitch { "Hz", "RATE" };
    std::unique_ptr<juce::ParameterAttachment> lfoSyncAttachment;
    bool lfoSynced = false;                 // selected LFO is in Rate mode (from the attachment)
    int selectedLfo = Processor::lfoDelayL;
    std::array<Knob*, Processor::numLfos> modulatedKnobs {};   // in LfoTarget order
    std::array<bool, Processor::numLfos> ringVisible {};       // the ring was visible in the last paint

    BandSpectrumComponent bandSpectrum;

    int selectedBand = 0;              // or BandSpectrumComponent::allBands (General mode)
    int activeBandCount = Processor::numBands;
    bool knobsBound = false;
    bool linked = false;

    // Panel rectangles in reference (900 x 720) coordinates; the scale is applied in resized().
    juce::Rectangle<int> timePanel, shaperPanel, settingsPanel, spectrumPanel;
    juce::Rectangle<int> shaperDrawArea, lfoTitleArea;
    std::array<int, 2> settingsDividers {};   // x positions of the group dividers in the Settings row
    std::array<juce::Rectangle<int>, 3> bandKnobFrames;   // highlight around L, R, Feedback
    float scale = 1.0f;

    juce::ComponentBoundsConstrainer editorConstrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultiwayAudioProcessorEditor)
};
