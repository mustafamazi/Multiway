/*
  ==============================================================================

    Multiway editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MultiwayPalette.h"
#include <BinaryData.h>

//==============================================================================
namespace
{
    // The whole layout is done at a 900 x 720 reference size; the window scales in this ratio.
    constexpr int referenceWidth  = 900;
    constexpr int referenceHeight = 720;

    constexpr int knobDiameter    = 70;   // ~62 px heptagon with the LookAndFeel's 4 px inner padding
    constexpr int panelHeaderHeight = 30;
    constexpr int knobRowHeight   = 106;  // knob + name + value
    constexpr int knobRowGap      = 6;
    constexpr int bandFrameWidth  = 150;  // highlight frame of the band knobs
    constexpr int syncRowHeight   = 20;   // ms | RATE switch under the L / R knobs
    constexpr int smallKnobDiameter = 48; // LFO knobs in the Shaper
    constexpr int minWidth = 720, maxWidth = 1500;

    // Random limits; low/high cut and mix are left untouched.
    constexpr float randomMaxDelayMs  = 800.0f;
    constexpr float randomMaxFeedback = 60.0f;

    // APVTS state properties (not parameters, invisible to host automation).
    const juce::Identifier selectedBandProperty { "selectedBand" };
    const juce::Identifier editorWidthProperty  { "editorWidth" };
    const juce::Identifier selectedLfoProperty  { "selectedLfo" };

    juce::String formatMs (double value)      { return juce::String (juce::roundToInt (value)) + " ms"; }
    juce::String formatPercent (double value) { return juce::String (juce::roundToInt (value)) + " %"; }
    juce::String formatSignedPercent (double value)
    {
        const auto rounded = juce::roundToInt (value);
        return (rounded > 0 ? "+" : "") + juce::String (rounded) + " %";
    }
    juce::String formatLfoHz (double hz) { return juce::String (hz, hz < 10.0 ? 2 : 1) + " Hz"; }

    // The axis of a delay knob with sync on is the division index; this maps an ms value to a position on it:
    // linear between the first pair of neighbouring divisions that contains the ms value (divisions are nearly sorted by ms).
    float divisionPosition (float ms, double bpm)
    {
        using P = MultiwayAudioProcessor;
        const auto msAt = [bpm] (int i) { return juce::jmin (P::maxDelayMs, P::divisionToMs (i, bpm)); };

        if (ms <= msAt (0))
            return 0.0f;

        for (int i = 0; i < P::numDivisions - 1; ++i)
        {
            const auto a = msAt (i), b = msAt (i + 1);
            if (ms >= juce::jmin (a, b) && ms <= juce::jmax (a, b))
                return (float) i + (b != a ? (ms - a) / (b - a) : 0.0f);
        }

        return (float) (P::numDivisions - 1);
    }

    juce::String formatHz (double value)
    {
        if (value < 1000.0)
            return juce::String (juce::roundToInt (value)) + " Hz";

        return juce::String (value / 1000.0, value < 10000.0 ? 2 : 1) + " kHz";
    }

    juce::String bandCentreName (int band, int activeBands)
    {
        const auto hz = MultiwayAudioProcessor::bandCentreHz (band, activeBands);
        return hz < 1000.0f ? juce::String ((int) std::floor (hz + 0.5f)) + " Hz"
                            : juce::String ((int) std::floor (hz / 1000.0f + 0.5f)) + " kHz";
    }

    juce::Font knobNameFont()   { return juce::Font (juce::FontOptions (15.0f, juce::Font::bold)).withExtraKerningFactor (0.08f); }
    juce::Font knobValueFont()  { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font panelTitleFont() { return juce::Font (juce::FontOptions (13.0f, juce::Font::bold)).withExtraKerningFactor (0.12f); }
    juce::Font smallKnobNameFont()  { return juce::Font (juce::FontOptions (11.0f, juce::Font::bold)).withExtraKerningFactor (0.06f); }
    juce::Font smallKnobValueFont() { return juce::Font (juce::FontOptions (10.5f)); }

    // Pencil: tip at the bottom left, body and ferrule line extending to the top right (20 x 20).
    juce::Path createPencilShape()
    {
        juce::Path outline;
        outline.startNewSubPath (3.0f, 17.0f);
        outline.lineTo (4.2f, 12.6f);
        outline.lineTo (13.0f, 3.8f);
        outline.lineTo (16.2f, 7.0f);
        outline.lineTo (7.4f, 15.8f);
        outline.closeSubPath();
        outline.startNewSubPath (11.0f, 5.8f);
        outline.lineTo (14.2f, 9.0f);
        outline.startNewSubPath (4.2f, 12.6f);
        outline.lineTo (7.4f, 15.8f);

        juce::Path shape;
        juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (shape, outline);
        return shape;
    }

    // Division warning: an exclamation mark inside a small triangle.
    void drawWarningIcon (juce::Graphics& g, juce::Rectangle<float> area)
    {
        juce::Path triangle;
        triangle.addTriangle (area.getCentreX(), area.getY(), area.getRight(), area.getBottom(), area.getX(), area.getBottom());

        g.setColour (MultiwayPalette::accent);
        g.fillPath (triangle);

        g.setColour (juce::Colours::white);
        const auto x = area.getCentreX();
        g.drawLine (x, area.getY() + area.getHeight() * 0.35f, x, area.getY() + area.getHeight() * 0.68f, 1.3f);
        g.fillEllipse (x - 0.75f, area.getY() + area.getHeight() * 0.76f, 1.5f, 1.5f);
    }

    // Undo arrow: a left-pointing arrowhead on the left, a body on the right that curls down and back.
    juce::Path createUndoShape()
    {
        juce::Path stem;
        stem.startNewSubPath (6.0f, 5.0f);
        stem.lineTo (12.0f, 5.0f);
        stem.cubicTo (18.5f, 5.0f, 18.5f, 15.0f, 12.0f, 15.0f);
        stem.lineTo (5.0f, 15.0f);

        juce::Path shape;
        juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (shape, stem);
        shape.addTriangle (1.0f, 5.0f, 6.5f, 1.0f, 6.5f, 9.0f);
        return shape;
    }
}

//==============================================================================
MultiwayAudioProcessorEditor::SegmentSwitch::SegmentSwitch (juce::String left, juce::String right)
    : leftText (std::move (left)), rightText (std::move (right))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MultiwayAudioProcessorEditor::SegmentSwitch::setRightSelected (bool shouldSelectRight)
{
    if (rightSelected == shouldSelectRight)
        return;

    rightSelected = shouldSelectRight;
    repaint();
}

void MultiwayAudioProcessorEditor::SegmentSwitch::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = bounds.getHeight() * 0.5f;

    auto selected = bounds;
    selected = rightSelected ? selected.removeFromRight (bounds.getWidth() * 0.5f)
                             : selected.removeFromLeft (bounds.getWidth() * 0.5f);

    g.setColour (MultiwayPalette::accent);
    g.fillRoundedRectangle (selected, radius);

    g.setColour (MultiwayPalette::ink.withAlpha (0.25f));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    g.setFont (juce::Font (juce::FontOptions (bounds.getHeight() * 0.62f, juce::Font::bold)).withExtraKerningFactor (0.04f));

    auto half = bounds;
    const auto left = half.removeFromLeft (bounds.getWidth() * 0.5f);

    g.setColour (rightSelected ? MultiwayPalette::ink.withAlpha (0.55f) : juce::Colours::white);
    g.drawText (leftText, left, juce::Justification::centred);
    g.setColour (rightSelected ? juce::Colours::white : MultiwayPalette::ink.withAlpha (0.55f));
    g.drawText (rightText, half, juce::Justification::centred);
}

void MultiwayAudioProcessorEditor::SegmentSwitch::mouseUp (const juce::MouseEvent& e)
{
    if (! isEnabled() || ! getLocalBounds().contains (e.getPosition()))
        return;

    const auto clickedRight = e.position.x >= (float) getWidth() * 0.5f;
    if (clickedRight == rightSelected)
        return;

    setRightSelected (clickedRight);

    if (onChange != nullptr)
        onChange (rightSelected);
}

//==============================================================================
void MultiwayAudioProcessorEditor::ShapeDisplay::setLfo (int newLfo)
{
    lfo = newLfo;
    drawn = processor.getLfoDrawnShape (lfo);
    usesDrawn = processor.usesDrawnShape (lfo);
    shapeVersion = processor.getLfoShapeVersion();
    lastIndex = -1;
    setMouseCursor (usesDrawn ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void MultiwayAudioProcessorEditor::ShapeDisplay::refresh()
{
    // State was loaded (project, preset) or the pencil changed: fetch the shape from the processor again.
    if (processor.getLfoShapeVersion() != shapeVersion || processor.usesDrawnShape (lfo) != usesDrawn)
        setLfo (lfo);

    phase = processor.getLfoPhase (lfo);
    modulation = processor.getLfoModulation (lfo);
    repaint();
}

// Area where the curve is drawn: a margin of one line thickness at the top and bottom.
juce::Rectangle<float> MultiwayAudioProcessorEditor::ShapeDisplay::getPlotArea() const
{
    return getLocalBounds().toFloat().reduced (8.0f, 10.0f);
}

void MultiwayAudioProcessorEditor::ShapeDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Spectrum panel background, thin lines at 25 % intervals.
    g.setColour (MultiwayPalette::plotFill);
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (MultiwayPalette::ink.withAlpha (0.06f));
    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine (juce::roundToInt (bounds.getHeight() * (float) i / 4.0f), 0.0f, bounds.getRight());

    // Shape: the drawn table if the pencil is on, otherwise a sine (the same one the processor uses).
    const auto plot = getPlotArea();
    auto yFor = [&plot] (float value) { return plot.getCentreY() - value * plot.getHeight() * 0.5f; };
    auto shapeAt = [this] (double p)
    {
        return usesDrawn ? MultiwayLfo::readTable ([this] (int i) { return drawn[(size_t) i]; }, p)
                         : MultiwayLfo::sine (p);
    };

    juce::Path curve;
    const auto steps = juce::jmax (2, juce::roundToInt (plot.getWidth()));
    for (int i = 0; i <= steps; ++i)
    {
        const auto p = (double) i / steps;
        const auto point = juce::Point<float> (plot.getX() + (float) p * plot.getWidth(), yFor (shapeAt (juce::jmin (p, 0.99999))));
        i == 0 ? curve.startNewSubPath (point) : curve.lineTo (point);
    }

    const auto active = modulation != 0.0f || processor.getLfoAmount (lfo) != 0.0f;
    g.setColour (active ? MultiwayPalette::accent : MultiwayPalette::ink.withAlpha (0.45f));
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Current phase: a thin vertical line and the shape's point there.
    const auto x = plot.getX() + (float) phase * plot.getWidth();
    g.setColour (MultiwayPalette::ink.withAlpha (active ? 0.25f : 0.12f));
    g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());

    if (active)
    {
        g.setColour (MultiwayPalette::accent);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, yFor (shapeAt (phase)) }));
    }
}

void MultiwayAudioProcessorEditor::ShapeDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (! usesDrawn)
        return;

    lastIndex = -1;
    drawTo (e.position);
}

void MultiwayAudioProcessorEditor::ShapeDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (usesDrawn)
        drawTo (e.position);
}

void MultiwayAudioProcessorEditor::ShapeDisplay::mouseUp (const juce::MouseEvent&)
{
    if (! usesDrawn)
        return;

    lastIndex = -1;
    processor.setLfoDrawnShape (lfo, drawn, true);   // drawing finished: write to state
}

// Converts the mouse position to a table point; the gap to the previous point is filled linearly (so fast
// drags leave no holes). The table is handed to the audio thread immediately.
void MultiwayAudioProcessorEditor::ShapeDisplay::drawTo (juce::Point<float> position)
{
    const auto plot = getPlotArea();
    const auto index = juce::jlimit (0, MultiwayLfo::shapeSize - 1,
                                     (int) ((position.x - plot.getX()) / plot.getWidth() * MultiwayLfo::shapeSize));
    const auto value = juce::jlimit (-1.0f, 1.0f, (plot.getCentreY() - position.y) / (plot.getHeight() * 0.5f));

    if (lastIndex < 0)
    {
        drawn[(size_t) index] = value;
    }
    else
    {
        const auto step = index >= lastIndex ? 1 : -1;
        for (int i = lastIndex; i != index + step; i += step)
        {
            const auto t = index == lastIndex ? 1.0f : (float) (i - lastIndex) / (float) (index - lastIndex);
            drawn[(size_t) i] = lastValue + (value - lastValue) * t;
        }
    }

    lastIndex = index;
    lastValue = value;
    processor.setLfoDrawnShape (lfo, drawn, false);
    repaint();
}

//==============================================================================
MultiwayAudioProcessorEditor::MultiwayAudioProcessorEditor (MultiwayAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      brandBadge (MultiwayPalette::ink),
      linkButton ("LINK", MultiwayPalette::ink, MultiwayPalette::accent, juce::Colours::white, false),
      shapeDisplay (p),
      bandSpectrum (p)
{
    // As in Clipotype: getTypefaceForFont() only takes effect when this instance is also the default
    // LookAndFeel (the JUCE font cache resolves through the default).
    setLookAndFeel (&clipotypeLookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&clipotypeLookAndFeel);

    titleTypeface = juce::Typeface::createSystemTypefaceFor (BinaryData::SpaceMonoBold_ttf,
                                                              (size_t) BinaryData::SpaceMonoBold_ttfSize);

    titleLabel.setText ("MULTIWAY", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, MultiwayPalette::ink);
    titleLabel.setFont (juce::Font (juce::FontOptions (titleTypeface).withHeight (40.0f)));
    addAndMakeVisible (titleLabel);
    brandBadge.setWordmarkTypeface (titleTypeface);
    addAndMakeVisible (brandBadge);   // after the title: so it stays above the full-width title label

    generateNoiseTexture();

    // Time: selected band. On a channel with sync on the value is the division name ("1/8D").
    auto formatDelay = [this] (Processor::BandParam side)
    {
        return [this, side] (double value)
        {
            return isSynced (side) ? juce::String (Processor::divisionNames[(size_t) juce::jlimit (0, Processor::numDivisions - 1, juce::roundToInt (value))])
                                                : formatMs (value);
        };
    };

    configureKnob (delayLKnob,   "L DELAY",  formatDelay (Processor::delayLParam));
    configureKnob (delayRKnob,   "R DELAY",  formatDelay (Processor::delayRParam));
    configureKnob (feedbackKnob, "FEEDBACK", formatPercent);

    // Names of the per-band knobs, in the colour of the "BAND ..." label in the panel header.
    for (auto* knob : { &delayLKnob, &delayRKnob, &feedbackKnob })
        knob->nameLabel.setColour (juce::Label::textColourId, MultiwayPalette::accent);

    // A band knob turned by the user also writes, outside the attachment, to all active bands in General mode
    // and to the opposite channel if Link is on; the gesture on these parameters stays open for the whole drag.
    // Slider calls onDragStart / onDragEnd for dragging, the mouse wheel and double-clicks.
    gestureParams.reserve ((size_t) (2 * Processor::numBands));

    for (auto [knob, side] : { std::pair { &delayLKnob, Processor::delayLParam },
                               std::pair { &delayRKnob, Processor::delayRParam },
                               std::pair { &feedbackKnob, Processor::feedbackParam } })
    {
        knob->slider.onValueChange = [this, knob = knob, side = side] { bandKnobChanged (*knob, side); };
        knob->slider.onDragStart   = [this, side = side] { beginKnobGesture (side); };
        knob->slider.onDragEnd     = [this] { endKnobGesture(); };
    }

    // Time: global
    configureKnob (lowCutKnob, "LOW CUT", [] (double hz)
    {
        return hz <= (double) Processor::minLowCutHz + 0.01 ? juce::String ("OFF") : formatHz (hz);
    });
    configureKnob (highCutKnob, "HIGH CUT", [] (double hz)
    {
        return hz >= (double) Processor::maxHighCutHz - 0.5 ? juce::String ("OFF") : formatHz (hz);
    });
    configureKnob (mixKnob, "MIX", formatPercent);

    // SliderAttachment sets the double-click value (setDoubleClickReturnValue) to the parameter's default.
    lowCutKnob.attachment  = std::make_unique<SliderAttachment> (processorRef.apvts, "lowCut",  lowCutKnob.slider);
    highCutKnob.attachment = std::make_unique<SliderAttachment> (processorRef.apvts, "highCut", highCutKnob.slider);
    mixKnob.attachment     = std::make_unique<SliderAttachment> (processorRef.apvts, "mix",     mixKnob.slider);

    for (auto* knob : { &lowCutKnob, &highCutKnob, &mixKnob })
        updateValueLabel (*knob);

    linkButton.onToggle = [this] (bool isOn) { setLinked (isOn); };
    addAndMakeVisible (linkButton);

    // ms | RATE: through the parameter (single-step gesture); display and knob rebinding come from
    // the attachment, so host automation and Reset / undo are followed too.
    for (auto [sw, side] : { std::pair { &syncLSwitch, Processor::delayLParam },
                             std::pair { &syncRSwitch, Processor::delayRParam } })
    {
        sw->onChange = [this, side = side] (bool rate) { setSync (side, rate); };
        addAndMakeVisible (*sw);

        syncAttachments[side == Processor::delayRParam ? 1 : 0] = std::make_unique<juce::ParameterAttachment> (
            *processorRef.apvts.getParameter (Processor::syncParamID (side)),
            [this, sw = sw, side = side] (float value)
            {
                // The order of parameter listeners is undefined; the state comes from here, not the processor's atomic.
                syncStates[side == Processor::delayRParam ? 1 : 0] = value >= 0.5f;
                sw->setRightSelected (value >= 0.5f);
                syncChanged();
            });
    }

    // Shaper: pressing a modulatable Time knob selects that knob's LFO.
    modulatedKnobs = { &delayLKnob, &delayRKnob, &feedbackKnob, &mixKnob, &lowCutKnob, &highCutKnob };
    for (int lfo = 0; lfo < Processor::numLfos; ++lfo)
        modulatedKnobs[(size_t) lfo]->slider.onPress = [this, lfo] { selectLfo (lfo); };

    addAndMakeVisible (shapeDisplay);

    // Pencil: when on, the drawn shape is used and the drawing area can be drawn on; when off, a sine.
    pencilButton.setShape (createPencilShape(), false, true, false);
    pencilButton.setColours (MultiwayPalette::ink.withAlpha (0.6f), MultiwayPalette::ink, MultiwayPalette::accent);
    pencilButton.setOnColours (MultiwayPalette::accent, MultiwayPalette::accent.brighter (0.2f), MultiwayPalette::accent.darker (0.2f));
    pencilButton.shouldUseOnColours (true);
    pencilButton.setClickingTogglesState (true);
    pencilButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    pencilButton.onClick = [this] { setLfoPen (pencilButton.getToggleState()); };
    addAndMakeVisible (pencilButton);

    // LFO row: the selected LFO's parameters (bindLfoKnobs). Rate is in Hz in Hz mode, a note division in Rate mode.
    configureKnob (lfoRateKnob, "RATE", [this] (double value)
    {
        return lfoSynced ? juce::String (Processor::lfoDivisionNames[(size_t) juce::jlimit (0, Processor::numLfoDivisions - 1, juce::roundToInt (value))])
                         : formatLfoHz (value);
    });
    configureKnob (lfoOffsetKnob, "OFFSET", formatPercent);
    configureKnob (lfoJitterKnob, "JITTER", formatPercent);
    configureKnob (lfoSmoothKnob, "SMOOTH", formatPercent);
    configureKnob (lfoAmountKnob, "AMOUNT", formatSignedPercent);

    for (auto* knob : { &lfoRateKnob, &lfoOffsetKnob, &lfoJitterKnob, &lfoSmoothKnob, &lfoAmountKnob })
    {
        knob->nameLabel.setFont (smallKnobNameFont());
        knob->valueLabel.setFont (smallKnobValueFont());
    }

    lfoRateSwitch.onChange = [this] (bool rate)
    {
        if (lfoSyncAttachment != nullptr)
            lfoSyncAttachment->setValueAsCompleteGesture (rate ? 1.0f : 0.0f);
    };
    addAndMakeVisible (lfoRateSwitch);

    // Settings row: Bands | Resolution | Random, Reset, undo.
    for (auto [label, text] : { std::pair { &bandsLabel, "BANDS" }, std::pair { &resolutionLabel, "RESOLUTION" } })
    {
        label->setText (text, juce::dontSendNotification);
        label->setFont (panelTitleFont());
        label->setColour (juce::Label::textColourId, MultiwayPalette::ink.withAlpha (0.55f));
        addAndMakeVisible (*label);
    }

    auto styleTextButton = [this] (juce::TextButton& button, juce::Colour offColour)
    {
        button.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        button.setColour (juce::TextButton::textColourOffId,  offColour);
        button.setColour (juce::TextButton::textColourOnId,   MultiwayPalette::ink);
        button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        addAndMakeVisible (button);
    };

    // Bands: selection through the parameter (single-step gesture), display from bandsAttachment.
    for (size_t i = 0; i < bandCountButtons.size(); ++i)
    {
        auto& button = bandCountButtons[i];
        button.setButtonText (juce::String (Processor::bandCountChoices[i]));
        styleTextButton (button, MultiwayPalette::ink.withAlpha (0.4f));
        button.onClick = [this, i] { bandsAttachment->setValueAsCompleteGesture ((float) i); };
    }

    // Resolution (FFT size): same as Bands; selection through the parameter (single-step gesture),
    // display from fftSizeAttachment.
    for (size_t i = 0; i < resolutionButtons.size(); ++i)
    {
        auto& button = resolutionButtons[i];
        button.setButtonText (juce::String (Processor::fftSizeChoices[i]));
        styleTextButton (button, MultiwayPalette::ink.withAlpha (0.4f));
        button.onClick = [this, i] { fftSizeAttachment->setValueAsCompleteGesture ((float) i); };
    }

    randomButton.setButtonText ("RANDOM");
    resetButton.setButtonText ("RESET");
    styleTextButton (randomButton, MultiwayPalette::ink.withAlpha (0.8f));
    styleTextButton (resetButton,  MultiwayPalette::ink.withAlpha (0.8f));
    randomButton.onClick = [this] { randomise(); };
    resetButton.onClick  = [this] { resetAll(); };

    undoButton.setShape (createUndoShape(), false, true, false);
    undoButton.setColours (MultiwayPalette::ink.withAlpha (0.6f), MultiwayPalette::ink, MultiwayPalette::accent);
    undoButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    undoButton.onClick = [this] { undoLast(); };
    setUndoAvailable (false);
    addAndMakeVisible (undoButton);

    bandSpectrum.onBandSelected = [this] (int band) { selectBand (band); };
    addAndMakeVisible (bandSpectrum);

    // The selected mode (band or ALL = BandSpectrumComponent::allBands) and Link are stored in the APVTS
    // state: they survive closing and reopening the editor and reloading the project.
    const auto& state = processorRef.apvts.state;
    activeBandCount = processorRef.getActiveBandCount();
    setLinked (processorRef.isDelayLinked());
    selectBand ((int) state.getProperty (selectedBandProperty, Processor::referenceBand));
    selectLfo ((int) state.getProperty (selectedLfoProperty, (int) Processor::lfoDelayL));

    bandsAttachment = std::make_unique<juce::ParameterAttachment> (
        *processorRef.apvts.getParameter ("bands"),
        [this] (float choiceIndex) { bandCountChanged (juce::roundToInt (choiceIndex)); });
    bandsAttachment->sendInitialUpdate();

    fftSizeAttachment = std::make_unique<juce::ParameterAttachment> (
        *processorRef.apvts.getParameter ("fftSize"),
        [this] (float choiceIndex) { fftSizeChanged (juce::roundToInt (choiceIndex)); });
    fftSizeAttachment->sendInitialUpdate();

    for (auto& attachment : syncAttachments)
        attachment->sendInitialUpdate();

    editorConstrainer.setSizeLimits (minWidth, minWidth * referenceHeight / referenceWidth,
                                     maxWidth, maxWidth * referenceHeight / referenceWidth);
    editorConstrainer.setFixedAspectRatio ((double) referenceWidth / (double) referenceHeight);
    setConstrainer (&editorConstrainer);
    setResizable (true, true);

    const auto width = juce::jlimit (minWidth, maxWidth, (int) state.getProperty (editorWidthProperty, referenceWidth));
    setSize (width, width * referenceHeight / referenceWidth);

    startTimerHz (30);   // in General mode the knobs follow the band average; LFO rings and shape phase
}

MultiwayAudioProcessorEditor::~MultiwayAudioProcessorEditor()
{
    stopTimer();
    endKnobGesture();

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

//==============================================================================
void MultiwayAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& name,
                                                  std::function<juce::String (double)> format)
{
    knob.format = std::move (format);

    knob.slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    knob.slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    knob.slider.onValueChange = [this, &knob] { updateValueLabel (knob); };
    addAndMakeVisible (knob.slider);

    knob.nameLabel.setText (name, juce::dontSendNotification);
    knob.nameLabel.setJustificationType (juce::Justification::centred);
    knob.nameLabel.setColour (juce::Label::textColourId, MultiwayPalette::ink);
    knob.nameLabel.setFont (knobNameFont());
    knob.nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (knob.nameLabel);

    knob.valueLabel.setJustificationType (juce::Justification::centred);
    knob.valueLabel.setColour (juce::Label::textColourId, MultiwayPalette::ink.withAlpha (0.7f));
    knob.valueLabel.setFont (knobValueFont());
    knob.valueLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (knob.valueLabel);
}

void MultiwayAudioProcessorEditor::updateValueLabel (Knob& knob)
{
    knob.valueLabel.setText (knob.format (knob.slider.getValue()), juce::dontSendNotification);
}

//==============================================================================
// band: 0..active band count - 1, or BandSpectrumComponent::allBands (General mode).
void MultiwayAudioProcessorEditor::selectBand (int band)
{
    if (band != BandSpectrumComponent::allBands)
        band = juce::jlimit (0, activeBandCount - 1, band);

    if (band == selectedBand && knobsBound)
        return;

    endKnobGesture();

    selectedBand = band;
    processorRef.apvts.state.setProperty (selectedBandProperty, band, nullptr);

    bindBandKnobs();
    knobsBound = true;

    bandSpectrum.setSelectedBand (band);
    repaint();   // band name in the Time panel header
}

void MultiwayAudioProcessorEditor::bindBandKnobs()
{
    // Remove the old bindings first so the knobs don't write to the old parameter while moving to the new band.
    delayLKnob.attachment.reset();
    delayRKnob.attachment.reset();
    feedbackKnob.attachment.reset();

    auto& apvts = processorRef.apvts;

    for (auto [knob, side] : { std::pair { &delayLKnob, Processor::delayLParam },
                               std::pair { &delayRKnob, Processor::delayRParam },
                               std::pair { &feedbackKnob, Processor::feedbackParam } })
    {
        if (isGeneralMode())
        {
            // General: the knob isn't bound to a parameter. Range and default are the same for all bands, taken from band0.
            const auto* param = apvts.getParameter (delayParamID (0, side));
            const auto range = param->getNormalisableRange();

            knob->slider.setNormalisableRange ({ (double) range.start, (double) range.end, (double) range.interval });
            knob->slider.setDoubleClickReturnValue (true, (double) range.convertFrom0to1 (param->getDefaultValue()));
        }
        else
        {
            // SliderAttachment sets the range and the double-click value (setDoubleClickReturnValue) from the parameter.
            knob->attachment = std::make_unique<SliderAttachment> (apvts, delayParamID (selectedBand, side), knob->slider);
        }

        // If the value stayed the same the attachment doesn't trigger onValueChange; refresh the labels manually.
        updateValueLabel (*knob);
    }

    refreshGeneralKnobs();
}

// In General mode the knobs show the average of the active bands, except the knob the user is holding.
void MultiwayAudioProcessorEditor::refreshGeneralKnobs()
{
    if (! isGeneralMode())
        return;

    for (auto [knob, side] : { std::pair { &delayLKnob, Processor::delayLParam },
                               std::pair { &delayRKnob, Processor::delayRParam },
                               std::pair { &feedbackKnob, Processor::feedbackParam } })
    {
        if (knob->slider.isMouseButtonDown())
            continue;

        float sum = 0.0f;
        for (int band = 0; band < activeBandCount; ++band)
        {
            const auto* param = processorRef.apvts.getParameter (delayParamID (band, side));
            sum += param->convertFrom0to1 (param->getValue());
        }

        knob->slider.setValue ((double) (sum / (float) activeBandCount), juce::dontSendNotification);
        updateValueLabel (*knob);
    }
}

void MultiwayAudioProcessorEditor::setLinked (bool shouldBeLinked)
{
    linked = shouldBeLinked;
    linkButton.setToggleState (linked, juce::dontSendNotification);
    bandSpectrum.setLinked (linked);
    processorRef.setDelayLinked (linked);

    // With Link on, R Delay uses the shared (L Delay) LFO; the Shaper shows that one.
    if (selectedLfo == Processor::lfoDelayR)
        selectLfo (Processor::lfoDelayR);

    repaint();   // Shaper header and modulation rings
}

// The Bands parameter changed (Settings, Reset, host automation or project load).
void MultiwayAudioProcessorEditor::bandCountChanged (int choiceIndex)
{
    for (size_t i = 0; i < bandCountButtons.size(); ++i)
        bandCountButtons[i].setToggleState ((int) i == choiceIndex, juce::dontSendNotification);

    // The order of parameter listeners is undefined; the band count comes from the choice, not the processor's atomic.
    activeBandCount = Processor::bandCountForChoice (choiceIndex);

    // Inactive bands lose their solo.
    processorRef.setSoloMask (processorRef.getSoloMask() & ((1u << activeBandCount) - 1u));

    if (! isGeneralMode() && selectedBand >= activeBandCount)
        selectBand (activeBandCount - 1);

    refreshGeneralKnobs();
    bandSpectrum.repaint();
    repaint();   // band centre in the Time panel header
}

// From fftSizeAttachment (on the message thread): only shows the selected Resolution button.
void MultiwayAudioProcessorEditor::fftSizeChanged (int choiceIndex)
{
    for (size_t i = 0; i < resolutionButtons.size(); ++i)
        resolutionButtons[i].setToggleState ((int) i == choiceIndex, juce::dontSendNotification);
}

//==============================================================================
// Switches the Shaper to lfo (LfoTarget): the Hz | RATE and LFO knobs are bound to that LFO's
// parameters, and the drawing area and pencil show that LFO's shape. The selection is stored in state.
// With Link on, R Delay is redirected to the L Delay LFO it shares.
void MultiwayAudioProcessorEditor::selectLfo (int lfo)
{
    lfo = processorRef.getSourceLfo (juce::jlimit (0, Processor::numLfos - 1, lfo));

    if (lfo == selectedLfo && lfoSyncAttachment != nullptr)
        return;

    selectedLfo = lfo;
    processorRef.apvts.state.setProperty (selectedLfoProperty, lfo, nullptr);

    // On its first update the sync attachment sets lfoSynced and binds the knobs (bindLfoKnobs).
    lfoSyncAttachment = std::make_unique<juce::ParameterAttachment> (
        *processorRef.apvts.getParameter (Processor::lfoParamID (lfo, Processor::lfoSyncParam)),
        [this] (float value)
        {
            lfoSynced = value >= 0.5f;
            lfoRateSwitch.setRightSelected (lfoSynced);
            bindLfoKnobs();
        });
    lfoSyncAttachment->sendInitialUpdate();

    pencilButton.setToggleState (processorRef.usesDrawnShape (lfo), juce::dontSendNotification);
    shapeDisplay.setLfo (lfo);
    repaint();   // Shaper header
}

// Binds the LFO row to the selected LFO's parameters; in Rate mode the Rate knob goes to the note division.
void MultiwayAudioProcessorEditor::bindLfoKnobs()
{
    auto& apvts = processorRef.apvts;

    for (auto [knob, param] : { std::pair { &lfoRateKnob,   lfoSynced ? Processor::lfoDivisionParam : Processor::lfoRateParam },
                                std::pair { &lfoOffsetKnob, Processor::lfoOffsetParam },
                                std::pair { &lfoJitterKnob, Processor::lfoJitterParam },
                                std::pair { &lfoSmoothKnob, Processor::lfoSmoothParam },
                                std::pair { &lfoAmountKnob, Processor::lfoAmountParam } })
    {
        knob->attachment.reset();
        knob->attachment = std::make_unique<SliderAttachment> (apvts, Processor::lfoParamID (selectedLfo, param), knob->slider);
        updateValueLabel (*knob);
    }
}

// Pencil: whether the selected LFO uses the drawn shape (on) or the sine (off); the drawn shape is not erased.
void MultiwayAudioProcessorEditor::setLfoPen (bool shouldUseDrawn)
{
    processorRef.setUsesDrawnShape (selectedLfo, shouldUseDrawn);
    shapeDisplay.setLfo (selectedLfo);
}

// Position (0..1) of the modulated value on the knob's rotary axis: the processor's latest modulation is
// added to the base value shown by the knob via applyModulation (the same calculation as the DSP).
float MultiwayAudioProcessorEditor::getModulatedProportion (int lfo) const
{
    auto& slider = modulatedKnobs[(size_t) lfo]->slider;   // valueToProportionOfLength isn't const
    const auto modulation = processorRef.getLfoModulation (processorRef.getSourceLfo (lfo));

    if (lfo == Processor::lfoDelayL || lfo == Processor::lfoDelayR)
    {
        const auto side = lfo == Processor::lfoDelayR ? Processor::delayRParam : Processor::delayLParam;

        if (isSynced (side))
        {
            const auto bpm = processorRef.getBpm();
            const auto baseMs = juce::jmin (Processor::maxDelayMs, Processor::divisionToMs (juce::roundToInt (slider.getValue()), bpm));
            const auto position = divisionPosition (Processor::applyModulation (lfo, baseMs, modulation), bpm);
            return position / (float) (Processor::numDivisions - 1);
        }
    }

    return (float) slider.valueToProportionOfLength (Processor::applyModulation (lfo, (float) slider.getValue(), modulation));
}

// Area where the ring is drawn (in reference coordinates): the knob's own bounds.
juce::Rectangle<int> MultiwayAudioProcessorEditor::getRingArea (int lfo) const
{
    return modulatedKnobs[(size_t) lfo]->slider.getBounds();
}

// A thin ring around a knob whose LFO is on; an arc from the base value to the modulated value and
// a dot moving at the modulated value.
void MultiwayAudioProcessorEditor::drawModulationRing (juce::Graphics& g, int lfo) const
{
    auto& slider = modulatedKnobs[(size_t) lfo]->slider;   // valueToProportionOfLength isn't const
    const auto area = getRingArea (lfo).toFloat();
    const auto centre = area.getCentre();
    const auto radius = area.getWidth() * 0.5f - 1.5f;

    const auto rotary = slider.getRotaryParameters();
    auto angleAt = [&rotary] (float proportion)
    {
        return rotary.startAngleRadians + juce::jlimit (0.0f, 1.0f, proportion) * (rotary.endAngleRadians - rotary.startAngleRadians);
    };

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, rotary.startAngleRadians, rotary.endAngleRadians, true);
    g.setColour (MultiwayPalette::ink.withAlpha (0.12f));
    g.strokePath (track, juce::PathStrokeType (1.2f));

    const auto baseAngle = angleAt ((float) slider.valueToProportionOfLength (slider.getValue()));
    const auto modulatedAngle = angleAt (getModulatedProportion (lfo));

    juce::Path range;
    range.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, baseAngle, modulatedAngle, true);
    g.setColour (MultiwayPalette::accent.withAlpha (0.8f));
    g.strokePath (range, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (MultiwayPalette::accent);
    g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre.getPointOnCircumference (radius, modulatedAngle)));
}

//==============================================================================
juce::Array<juce::RangedAudioParameter*> MultiwayAudioProcessorEditor::getKnobTargets (Processor::BandParam side) const
{
    juce::Array<juce::RangedAudioParameter*> targets;
    auto& apvts = processorRef.apvts;

    auto addSide = [&] (Processor::BandParam param)
    {
        if (isGeneralMode())
            for (int band = 0; band < activeBandCount; ++band)
                targets.add (apvts.getParameter (delayParamID (band, param)));
        else
            targets.add (apvts.getParameter (delayParamID (selectedBand, param)));
    };

    // In single-band mode the attachment writes the selected band's own parameter.
    if (isGeneralMode())
        addSide (side);

    // Link writes to the opposite channel only if both channels are in the same mode (both ms or both Rate);
    // an ms value can't be carried over to a division index (or vice versa).
    const auto other = side == Processor::delayLParam ? Processor::delayRParam : Processor::delayLParam;

    if (linked && side != Processor::feedbackParam && isSynced (side) == isSynced (other))
        addSide (other);

    return targets;
}

void MultiwayAudioProcessorEditor::bandKnobChanged (Knob& knob, Processor::BandParam side)
{
    updateValueLabel (knob);

    if (side != Processor::feedbackParam)
        updateClampWarnings();

    // Rebinding on band change, the average display and host automation don't write.
    if (! knob.slider.isUserEditing())
        return;

    const auto value = (float) knob.slider.getValue();

    for (auto* param : getKnobTargets (side))
        writeParameter (*param, param->convertTo0to1 (value));
}

void MultiwayAudioProcessorEditor::beginKnobGesture (Processor::BandParam side)
{
    endKnobGesture();

    for (auto* param : getKnobTargets (side))
    {
        param->beginChangeGesture();
        gestureParams.push_back (param);
    }
}

void MultiwayAudioProcessorEditor::endKnobGesture()
{
    for (auto* param : gestureParams)
        param->endChangeGesture();

    gestureParams.clear();
}

// Writes the parameter, notifying the host. Opens a single-step gesture if not part of an open drag gesture.
void MultiwayAudioProcessorEditor::writeParameter (juce::RangedAudioParameter& param, float normalisedValue)
{
    if (param.getValue() == normalisedValue)
        return;

    if (std::find (gestureParams.begin(), gestureParams.end(), &param) != gestureParams.end())
    {
        param.setValueNotifyingHost (normalisedValue);
        return;
    }

    param.beginChangeGesture();
    param.setValueNotifyingHost (normalisedValue);
    param.endChangeGesture();
}

//==============================================================================
// With sync on, the knob is bound to the band's note division; otherwise to the ms parameter.
juce::String MultiwayAudioProcessorEditor::delayParamID (int band, Processor::BandParam side) const
{
    if (side != Processor::feedbackParam && isSynced (side))
        return Processor::bandDivisionID (band, side);

    return Processor::bandParamID (band, side);
}

// ms | RATE switch; with Link on both channels switch together. Since the switch is global it
// applies to all bands in General mode too.
void MultiwayAudioProcessorEditor::setSync (Processor::BandParam side, bool shouldSync)
{
    const auto value = shouldSync ? 1.0f : 0.0f;
    syncAttachments[side == Processor::delayRParam ? 1 : 0]->setValueAsCompleteGesture (value);

    if (linked)
        syncAttachments[side == Processor::delayRParam ? 0 : 1]->setValueAsCompleteGesture (value);
}

// syncL / syncR changed (switch, host automation, Reset, undo or project load):
// the L / R knobs are rebound between the ms and division parameters.
void MultiwayAudioProcessorEditor::syncChanged()
{
    endKnobGesture();

    if (knobsBound)
        bindBandKnobs();

    updateClampWarnings();
    bandSpectrum.repaint();
}

// In Rate mode, a warning next to the value if the division shown by the knob exceeds maxDelayMs at the current BPM.
void MultiwayAudioProcessorEditor::updateClampWarnings()
{
    const auto bpm = processorRef.getBpm();
    auto changed = false;

    for (auto [knob, side] : { std::pair { &delayLKnob, Processor::delayLParam },
                               std::pair { &delayRKnob, Processor::delayRParam } })
    {
        const auto clamped = isSynced (side)
                          && Processor::divisionToMs (juce::roundToInt (knob->slider.getValue()), bpm) > Processor::maxDelayMs;

        auto& warning = clampWarnings[side == Processor::delayRParam ? 1 : 0];
        changed = changed || warning != clamped;
        warning = clamped;
    }

    if (changed)
        repaint();
}

//==============================================================================
// Randomises the delay and feedback of the active bands; L = R if Link is on. On a channel with sync on,
// a random note division (among those not exceeding randomMaxDelayMs at the current BPM).
void MultiwayAudioProcessorEditor::randomise()
{
    endKnobGesture();
    storeUndoSnapshot();

    auto set = [this] (const juce::String& parameterID, float value)
    {
        auto* param = processorRef.apvts.getParameter (parameterID);
        writeParameter (*param, param->convertTo0to1 (value));
    };

    int maxRandomDivision = 0;
    for (int i = 0; i < Processor::numDivisions; ++i)
        if (Processor::divisionToMs (i, processorRef.getBpm()) <= randomMaxDelayMs)
            maxRandomDivision = i;

    // Division index or ms, depending on the channel's mode.
    auto randomDelay = [this, maxRandomDivision] (Processor::BandParam side)
    {
        return isSynced (side) ? (float) random.nextInt (maxRandomDivision + 1)
                                            : random.nextFloat() * randomMaxDelayMs;
    };

    const auto sameMode = isSynced (Processor::delayLParam) == isSynced (Processor::delayRParam);

    for (int band = 0; band < activeBandCount; ++band)
    {
        const auto delayL = randomDelay (Processor::delayLParam);
        const auto delayR = linked && sameMode ? delayL : randomDelay (Processor::delayRParam);

        set (delayParamID (band, Processor::delayLParam), delayL);
        set (delayParamID (band, Processor::delayRParam), delayR);
        set (Processor::bandParamID (band, Processor::feedbackParam), random.nextFloat() * randomMaxFeedback);
    }
}

// All parameters back to their defaults (delay 150 ms, feedback 0, cuts off, mix 100 %, 10 bands).
void MultiwayAudioProcessorEditor::resetAll()
{
    endKnobGesture();
    storeUndoSnapshot();

    for (auto* param : processorRef.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            writeParameter (*ranged, ranged->getDefaultValue());
}

void MultiwayAudioProcessorEditor::storeUndoSnapshot()
{
    undoSnapshot.clear();

    for (auto* param : processorRef.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            undoSnapshot.emplace_back (ranged, ranged->getValue());

    setUndoAvailable (true);
}

// Returns to the state before the last Random / Reset; a single step.
void MultiwayAudioProcessorEditor::undoLast()
{
    if (undoSnapshot.empty())
        return;

    endKnobGesture();

    for (auto [param, value] : undoSnapshot)
        writeParameter (*param, value);

    undoSnapshot.clear();
    setUndoAvailable (false);
}

void MultiwayAudioProcessorEditor::setUndoAvailable (bool available)
{
    undoButton.setEnabled (available);
    undoButton.setAlpha (available ? 1.0f : 0.3f);
}

// If the state changes externally (the host loaded the project), follows the selected band and Link;
// in General mode the knobs track the band average.
void MultiwayAudioProcessorEditor::timerCallback()
{
    const auto& state = processorRef.apvts.state;

    if (processorRef.isDelayLinked() != linked)
        setLinked (processorRef.isDelayLinked());

    const auto storedBand = (int) state.getProperty (selectedBandProperty, selectedBand);
    if (storedBand != selectedBand)
        selectBand (storedBand);

    const auto storedLfo = (int) state.getProperty (selectedLfoProperty, selectedLfo);
    if (storedLfo != selectedLfo)
        selectLfo (storedLfo);

    refreshGeneralKnobs();
    updateClampWarnings();   // the BPM may have changed

    // The pencil state may change when state is loaded; shape phase and LFO rings on every tick.
    pencilButton.setToggleState (processorRef.usesDrawnShape (selectedLfo), juce::dontSendNotification);
    shapeDisplay.refresh();

    const auto toEditor = juce::AffineTransform::scale (scale);
    for (int lfo = 0; lfo < Processor::numLfos; ++lfo)
    {
        const auto visible = processorRef.getLfoAmount (processorRef.getSourceLfo (lfo)) != 0.0f;
        if (visible || ringVisible[(size_t) lfo])
            repaint (getRingArea (lfo).toFloat().transformedBy (toEditor).getSmallestIntegerContainer().expanded (2));

        ringVisible[(size_t) lfo] = visible;
    }
}

//==============================================================================
void MultiwayAudioProcessorEditor::generateNoiseTexture()
{
    // Film grain as in Clipotype: a tileable noise texture generated once.
    constexpr int tileSize = 128;
    noiseTexture = juce::Image (juce::Image::RGB, tileSize, tileSize, false);

    juce::Random random;
    juce::Image::BitmapData bitmap (noiseTexture, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < tileSize; ++y)
    {
        for (int x = 0; x < tileSize; ++x)
        {
            const auto grey = (juce::uint8) random.nextInt (256);
            bitmap.setPixelColour (x, y, juce::Colour (grey, grey, grey));
        }
    }
}

void MultiwayAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient::vertical (MultiwayPalette::backgroundTop, 0.0f,
                                                       MultiwayPalette::backgroundBottom, (float) getHeight()));
    g.fillAll();

    g.setOpacity (0.015f);
    for (int y = 0; y < getHeight(); y += noiseTexture.getHeight())
        for (int x = 0; x < getWidth(); x += noiseTexture.getWidth())
            g.drawImageAt (noiseTexture, x, y);

    g.setOpacity (1.0f);
    g.addTransform (juce::AffineTransform::scale (scale));

    for (auto panel : { timePanel, shaperPanel, settingsPanel, spectrumPanel })
    {
        g.setColour (MultiwayPalette::panelFill);
        g.fillRoundedRectangle (panel.toFloat(), 10.0f);
        g.setColour (MultiwayPalette::panelOutline);
        g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }

    auto drawPanelTitle = [&g] (juce::Rectangle<int> panel, const juce::String& title)
    {
        g.setFont (panelTitleFont());
        g.setColour (MultiwayPalette::ink.withAlpha (0.55f));
        g.drawText (title, panel.removeFromTop (panelHeaderHeight).reduced (14, 0), juce::Justification::centredLeft);
    };

    drawPanelTitle (timePanel, "TIME");
    const auto lfoName = linked && selectedLfo == Processor::lfoDelayL ? juce::String ("L + R DELAY")
                                                                      : juce::String (Processor::lfoTargetNames[(size_t) selectedLfo]);
    drawPanelTitle (shaperPanel, juce::String (juce::CharPointer_UTF8 ("SHAPER \xc2\xb7 ")) + lfoName);

    // To the right of the Time header: the band written by the L, R and Feedback knobs (all of them in General mode).
    g.setColour (MultiwayPalette::accent);
    g.drawText (isGeneralMode() ? juce::String ("ALL BANDS") : "BAND " + bandCentreName (selectedBand, activeBandCount),
                timePanel.withHeight (panelHeaderHeight).reduced (14, 0), juce::Justification::centredRight);

    // Band knobs are framed in the colour of the band label in the header; global knobs have no frame.
    for (auto frame : bandKnobFrames)
    {
        g.setColour (MultiwayPalette::accent.withAlpha (0.05f));
        g.fillRoundedRectangle (frame.toFloat(), 8.0f);
        g.setColour (MultiwayPalette::accent.withAlpha (0.45f));
        g.drawRoundedRectangle (frame.toFloat().reduced (0.5f), 8.0f, 1.0f);
    }

    // Group dividers in the Settings row.
    g.setColour (MultiwayPalette::panelOutline);
    for (auto x : settingsDividers)
        g.drawVerticalLine (x, (float) settingsPanel.getY() + 9.0f, (float) settingsPanel.getBottom() - 9.0f);

    // In Rate mode, a warning to the right of the value text if the division exceeds 2000 ms (clamped).
    for (auto [knob, warning] : { std::pair { &delayLKnob, clampWarnings[0] }, std::pair { &delayRKnob, clampWarnings[1] } })
    {
        if (! warning)
            continue;

        const auto value = knob->valueLabel.getBounds().toFloat();
        const auto textWidth = juce::GlyphArrangement::getStringWidth (knobValueFont(), knob->valueLabel.getText());
        drawWarningIcon (g, juce::Rectangle<float> (11.0f, 10.0f).withCentre ({ value.getCentreX() + textWidth * 0.5f + 10.0f,
                                                                                value.getCentreY() }));
    }

    // Header of the LFO row in the Shaper (the drawing area is in ShapeDisplay).
    g.setFont (panelTitleFont());
    g.setColour (MultiwayPalette::ink.withAlpha (0.55f));
    g.drawText ("LFO", lfoTitleArea, juce::Justification::centredLeft);

    // Modulation ring around knobs whose LFO is on (stays beneath the knob components,
    // visible outside the heptagon).
    for (int lfo = 0; lfo < Processor::numLfos; ++lfo)
        if (processorRef.getLfoAmount (processorRef.getSourceLfo (lfo)) != 0.0f)
            drawModulationRing (g, lfo);
}

void MultiwayAudioProcessorEditor::layoutKnob (Knob& knob, juce::Rectangle<int> cell)
{
    knob.slider.setBounds (cell.removeFromTop (knobDiameter).withSizeKeepingCentre (knobDiameter, knobDiameter));
    cell.removeFromTop (2);
    knob.nameLabel.setBounds (cell.removeFromTop (18));
    knob.valueLabel.setBounds (cell.removeFromTop (16));
}

// Small knob in the Shaper: a smaller heptagon and text.
void MultiwayAudioProcessorEditor::layoutSmallKnob (Knob& knob, juce::Rectangle<int> cell)
{
    knob.slider.setBounds (cell.removeFromTop (smallKnobDiameter).withSizeKeepingCentre (smallKnobDiameter, smallKnobDiameter));
    cell.removeFromTop (2);
    knob.nameLabel.setBounds (cell.removeFromTop (14));
    knob.valueLabel.setBounds (cell.removeFromTop (13));
}

void MultiwayAudioProcessorEditor::resized()
{
    processorRef.apvts.state.setProperty (editorWidthProperty, getWidth(), nullptr);

    // Children are laid out in 900 x 720 reference coordinates and scaled up with a transform;
    // that way the text scales with the window too.
    scale = (float) getWidth() / (float) referenceWidth;

    constexpr int margin = 20, gap = 12;
    auto area = juce::Rectangle<int> (referenceWidth, referenceHeight).reduced (margin, 0);

    const auto titleRow = area.removeFromTop (60);
    titleLabel.setBounds (titleRow);

    // BopsAudio header: left of the title row; the logo is inset by its shadow, aligned with the panels' left edge.
    brandBadge.setBounds (titleRow.withWidth (brandBadge.getIdealWidth()).withSizeKeepingCentre (brandBadge.getIdealWidth(), BrandBadge::idealHeight)
                                  .translated (-3, 0));

    // Time: header + 3 knob rows (ms | RATE under L / R); the Shaper has the same height.
    auto top = area.removeFromTop (panelHeaderHeight + 3 * knobRowHeight + syncRowHeight + 2 * knobRowGap + 10);
    timePanel   = top.removeFromLeft (520);
    top.removeFromLeft (gap);
    shaperPanel = top;

    area.removeFromTop (8);
    settingsPanel = area.removeFromTop (36);

    area.removeFromTop (8);
    spectrumPanel = area.removeFromTop (area.getHeight() - 16);

    // Time: 3 rows x 2 columns. L Delay | R Delay, Feedback | Mix, Low Cut | High Cut.
    auto time = timePanel.withTrimmedTop (panelHeaderHeight).reduced (10, 0);
    const auto columnWidth = time.getWidth() / 2;

    auto nextRow = [&time]
    {
        auto row = time.removeFromTop (knobRowHeight);
        time.removeFromTop (knobRowGap);
        return row;
    };

    auto delayRow = time.removeFromTop (knobRowHeight);
    auto syncRow  = time.removeFromTop (syncRowHeight);
    time.removeFromTop (knobRowGap);
    auto mixRow = nextRow(), cutRow = nextRow();

    layoutKnob (delayLKnob,   delayRow.removeFromLeft (columnWidth));
    layoutKnob (delayRKnob,   delayRow);
    syncLSwitch.setBounds (syncRow.removeFromLeft (columnWidth).withSizeKeepingCentre (84, 16).withY (syncRow.getY() + 1));
    syncRSwitch.setBounds (syncRow.withSizeKeepingCentre (84, 16).withY (syncRow.getY() + 1));
    layoutKnob (lowCutKnob,   cutRow.removeFromLeft (columnWidth));
    layoutKnob (highCutKnob,  cutRow);
    layoutKnob (feedbackKnob, mixRow.removeFromLeft (columnWidth));
    layoutKnob (mixKnob,      mixRow);

    // Link sits exactly between the L and R knobs, level with the knob centres.
    const auto leftKnob  = delayLKnob.slider.getBounds().getCentre();
    const auto rightKnob = delayRKnob.slider.getBounds().getCentre();
    linkButton.setBounds (juce::Rectangle<int> (44, 28).withCentre ({ (leftKnob.x + rightKnob.x) / 2, leftKnob.y }));

    // Highlight frame of the band knobs: from the knob down to below the value text.
    auto frameFor = [] (const Knob& knob)
    {
        return knob.slider.getBounds().withBottom (knob.valueLabel.getBottom())
                   .withSizeKeepingCentre (bandFrameWidth, knobRowHeight)
                   .expanded (0, 3);
    };

    // The L / R frame also covers the ms | RATE switch beneath it.
    bandKnobFrames = { frameFor (delayLKnob).withBottom (syncLSwitch.getBottom() + 3),
                       frameFor (delayRKnob).withBottom (syncRSwitch.getBottom() + 3),
                       frameFor (feedbackKnob) };

    // Shaper: an empty drawing area at the top (pencil at its top right), the LFO row at the bottom:
    // Rate (Hz | RATE beneath it), Offset, Jitter, Smooth, Amount.
    auto shaper = shaperPanel.withTrimmedTop (panelHeaderHeight).reduced (12, 0);
    shaper.removeFromBottom (10);

    auto lfo = shaper.removeFromBottom (16 + smallKnobDiameter + 2 + 14 + 13 + 18);
    shaper.removeFromBottom (8);
    shaperDrawArea = shaper;
    shapeDisplay.setBounds (shaperDrawArea);

    pencilButton.setBounds (juce::Rectangle<int> (22, 22).withPosition (shaperDrawArea.getRight() - 26, shaperDrawArea.getY() + 4));

    lfoTitleArea = lfo.removeFromTop (16).withTrimmedLeft (2);
    const auto lfoCellWidth = lfo.getWidth() / 5;

    for (auto* knob : { &lfoRateKnob, &lfoOffsetKnob, &lfoJitterKnob, &lfoSmoothKnob, &lfoAmountKnob })
        layoutSmallKnob (*knob, lfo.removeFromLeft (lfoCellWidth));

    lfoRateSwitch.setBounds (juce::Rectangle<int> (58, 14).withCentre ({ lfoRateKnob.valueLabel.getBounds().getCentreX(),
                                                                         lfoRateKnob.valueLabel.getBottom() + 10 }));

    // Settings: BANDS 10 4 2 | RESOLUTION 512 1024 2048 4096 | ... RANDOM RESET undo
    constexpr int groupGap = 28;
    auto settings = settingsPanel.reduced (14, 0);

    bandsLabel.setBounds (settings.removeFromLeft (62));
    for (auto& button : bandCountButtons)
        button.setBounds (settings.removeFromLeft (40));

    settingsDividers[0] = settings.getX() + groupGap / 2;
    settings.removeFromLeft (groupGap);

    resolutionLabel.setBounds (settings.removeFromLeft (112));
    for (auto& button : resolutionButtons)
        button.setBounds (settings.removeFromLeft (56));

    undoButton.setBounds (settings.removeFromRight (22).withSizeKeepingCentre (20, 18));
    settings.removeFromRight (6);
    resetButton.setBounds (settings.removeFromRight (70));
    randomButton.setBounds (settings.removeFromRight (86));
    settingsDividers[1] = randomButton.getX() - groupGap / 2;

    bandSpectrum.setBounds (spectrumPanel.reduced (4));

    // All children are scaled except the window's own resize corner.
    for (auto* child : getChildren())
        if (child != resizableCorner.get())
            child->setTransform (juce::AffineTransform::scale (scale));
}
