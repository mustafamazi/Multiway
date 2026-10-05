#include "BandSpectrumComponent.h"
#include "MultiwayPalette.h"

namespace
{
    constexpr float labelAreaHeight  = 22.0f;
    constexpr float plotPadding      = 6.0f;
    constexpr float columnGapRatio   = 0.12f;   // this fraction of the octave width is left empty on both sides of a column
    constexpr float spectrumTiltDb   = 3.0f;    // tilt per octave (relative to 1 kHz), makes music look flatter
    constexpr float spectrumDecayDb  = 1.5f;    // fall per timer tick (~45 dB/s @ 30 Hz)
    constexpr float linkedEqualMs    = 0.5f;    // if L and R are this close, a single bar is drawn
    constexpr float allColumnWidth   = 52.0f;   // leftmost ALL column
    constexpr float maxBarBase       = 60.0f;   // in wide columns (4 / 2 bands) bars are drawn relative to this width
    constexpr float soloButtonSize   = 14.0f;
    constexpr float soloButtonGap    = 4.0f;    // gap between the band name and the S button

    // 31.25 -> "31", 62.5 -> "63", 1000 -> "1k", 16000 -> "16k".
    juce::String bandName (int band, int activeBands)
    {
        const auto hz = MultiwayAudioProcessor::bandCentreHz (band, activeBands);
        return hz < 1000.0f ? juce::String ((int) std::floor (hz + 0.5f))
                            : juce::String ((int) std::floor (hz / 1000.0f + 0.5f)) + "k";
    }

    juce::Font labelFont (bool bold)   { return juce::Font (juce::FontOptions (12.0f, bold ? juce::Font::bold : juce::Font::plain)); }

    // The spectrum axis is independent of the band count: 10 octaves starting at 31.25 / sqrt 2.
    // With 10 bands each column fits exactly one octave; with 4 and 2 bands the columns are split evenly.
    constexpr float axisOctaves = 10.0f;
    const float axisMinHz = MultiwayAudioProcessor::lowestCentreHz / juce::MathConstants<float>::sqrt2;
}

//==============================================================================
BandSpectrumComponent::BandSpectrumComponent (MultiwayAudioProcessor& p)
    : processor (p),
      ring ((size_t) fftSize, 0.0f),
      fftData ((size_t) (2 * fftSize), 0.0f),
      displayDb ((size_t) numBins, minDb)
{
    for (int band = 0; band < numBands; ++band)
    {
        delayLParams[(size_t) band] = processor.apvts.getParameter (Processor::bandParamID (band, Processor::delayLParam));
        delayRParams[(size_t) band] = processor.apvts.getParameter (Processor::bandParamID (band, Processor::delayRParam));
        feedbackParams[(size_t) band] = processor.apvts.getParameter (Processor::bandParamID (band, Processor::feedbackParam));
        divisionLParams[(size_t) band] = processor.apvts.getParameter (Processor::bandDivisionID (band, Processor::delayLParam));
        divisionRParams[(size_t) band] = processor.apvts.getParameter (Processor::bandDivisionID (band, Processor::delayRParam));
        jassert (delayLParams[(size_t) band] != nullptr && delayRParams[(size_t) band] != nullptr
                 && feedbackParams[(size_t) band] != nullptr
                 && divisionLParams[(size_t) band] != nullptr && divisionRParams[(size_t) band] != nullptr);
    }

    lowCutParam  = processor.apvts.getRawParameterValue ("lowCut");
    highCutParam = processor.apvts.getRawParameterValue ("highCut");

    // While the editor was closed the FIFO may have filled up with stale samples; they are dropped
    // so a frozen spectrum doesn't show on the first frame (consumer side, lock-free).
    processor.spectrumTap.pull ([] (const float*, int) {});

    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    startTimerHz (30);
}

BandSpectrumComponent::~BandSpectrumComponent()
{
    stopTimer();
}

void BandSpectrumComponent::setSelectedBand (int band)
{
    selectedBand = band == allBands ? allBands : juce::jlimit (0, numBands - 1, band);
    repaint();
}

//==============================================================================
void BandSpectrumComponent::timerCallback()
{
    processor.spectrumTap.pull ([this] (const float* samples, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            ring[(size_t) ringPosition] = samples[i];
            ringPosition = (ringPosition + 1) % fftSize;
        }
    });

    computeSpectrum();
    repaint();
}

void BandSpectrumComponent::computeSpectrum()
{
    // ringPosition points at the oldest sample: unroll into time order and apply the window.
    for (int n = 0; n < fftSize; ++n)
        fftData[(size_t) n] = ring[(size_t) ((ringPosition + n) % fftSize)];

    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const auto sampleRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;

    // With a Hann window a full-scale sine gives |X| = N / 4; scaling by 4 / N maps it to 0 dBFS.
    const float magnitudeScale = 4.0f / (float) fftSize;

    for (int k = 1; k < numBins; ++k)
    {
        // The tilt is added only to the signal, not the floor, then clipped at minDb; otherwise a fixed
        // ramp (tilted floor) would show above 1 kHz even in silence.
        const auto hz = (float) ((double) k * sampleRate / (double) fftSize);
        const auto db = juce::jmax (minDb, juce::Decibels::gainToDecibels (fftData[(size_t) k] * magnitudeScale, 2.0f * minDb)
                                           + spectrumTiltDb * std::log2 (hz / Processor::referenceHz));

        // Fast attack, slow release.
        displayDb[(size_t) k] = juce::jmax (db, displayDb[(size_t) k] - spectrumDecayDb);
    }
}

//==============================================================================
juce::Rectangle<float> BandSpectrumComponent::getPlotArea() const
{
    auto area = getLocalBounds().toFloat().reduced (plotPadding);
    area.removeFromBottom (labelAreaHeight);
    area.removeFromLeft (allColumnWidth + plotPadding);
    return area;
}

juce::Rectangle<float> BandSpectrumComponent::getAllColumnArea() const
{
    auto area = getLocalBounds().toFloat().reduced (plotPadding);
    area.removeFromBottom (labelAreaHeight);
    return area.removeFromLeft (allColumnWidth);
}

float BandSpectrumComponent::frequencyToX (float hz, juce::Rectangle<float> plot) const
{
    return plot.getX() + plot.getWidth() * std::log2 (hz / axisMinHz) / axisOctaves;
}

float BandSpectrumComponent::xToFrequency (float x, juce::Rectangle<float> plot) const
{
    return axisMinHz * std::pow (2.0f, (x - plot.getX()) / plot.getWidth() * axisOctaves);
}

// Active bands share the plot width equally.
juce::Rectangle<float> BandSpectrumComponent::getColumnBounds (int band, juce::Rectangle<float> plot) const
{
    const auto slotWidth = plot.getWidth() / (float) processor.getActiveBandCount();
    const auto left = plot.getX() + slotWidth * (float) band;

    return { left + slotWidth * columnGapRatio, plot.getY(),
             slotWidth * (1.0f - 2.0f * columnGapRatio), plot.getHeight() };
}

// The S button sits to the right of the band name under the column; name + button are centred on the column together.
juce::Rectangle<float> BandSpectrumComponent::getSoloButtonBounds (int band, juce::Rectangle<float> plot) const
{
    const auto column = getColumnBounds (band, plot);
    const auto nameWidth = juce::GlyphArrangement::getStringWidth (labelFont (true), bandName (band, processor.getActiveBandCount()));
    const auto groupWidth = nameWidth + soloButtonGap + soloButtonSize;

    return juce::Rectangle<float> (soloButtonSize, soloButtonSize)
               .withCentre ({ column.getCentreX() - groupWidth * 0.5f + nameWidth + soloButtonGap + soloButtonSize * 0.5f,
                              plot.getBottom() + labelAreaHeight * 0.5f });
}

int BandSpectrumComponent::getBandAt (float x, juce::Rectangle<float> plot) const
{
    const auto activeBands = processor.getActiveBandCount();
    return juce::jlimit (0, activeBands - 1, (int) std::floor ((x - plot.getX()) / plot.getWidth() * (float) activeBands));
}

int BandSpectrumComponent::getSoloButtonAt (juce::Point<float> position, juce::Rectangle<float> plot) const
{
    for (int band = 0; band < processor.getActiveBandCount(); ++band)
        if (getSoloButtonBounds (band, plot).expanded (3.0f).contains (position))
            return band;

    return -1;
}

bool BandSpectrumComponent::isSoloed (int band) const
{
    return ((processor.getSoloMask() >> band) & 1u) != 0;
}

// Delay used by the DSP: if sync is on, the division's ms value at the current BPM (clamped).
float BandSpectrumComponent::getDelayMs (int band, Processor::BandParam side) const
{
    return processor.getEffectiveDelayMs (band, side);
}

// Parameter written by dragging: the band's note division if sync is on, otherwise ms.
juce::RangedAudioParameter* BandSpectrumComponent::getDelayTarget (int band, Processor::BandParam side) const
{
    const auto left = side == Processor::delayLParam;

    if (processor.isSynced (side))
        return left ? divisionLParams[(size_t) band] : divisionRParams[(size_t) band];

    return left ? delayLParams[(size_t) band] : delayRParams[(size_t) band];
}

// Converts an ms value to the target parameter; for the division parameter, the nearest division in log time.
float BandSpectrumComponent::msToNormalised (const juce::RangedAudioParameter& param, float ms) const
{
    if (dynamic_cast<const juce::AudioParameterChoice*> (&param) == nullptr)
        return param.convertTo0to1 (ms);

    const auto bpm = processor.getBpm();
    int nearest = 0;
    auto nearestDistance = std::numeric_limits<float>::max();

    for (int i = 0; i < Processor::numDivisions; ++i)
    {
        const auto distance = std::abs (std::log (juce::jmax (1.0f, Processor::divisionToMs (i, bpm)) / juce::jmax (1.0f, ms)));
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = i;
        }
    }

    return param.convertTo0to1 ((float) nearest);
}

// For the selected band's label: the division name ("1/8D") if sync is on, otherwise the ms value.
juce::String BandSpectrumComponent::delayText (int band, Processor::BandParam side) const
{
    if (processor.isSynced (side))
    {
        const auto* param = side == Processor::delayLParam ? divisionLParams[(size_t) band] : divisionRParams[(size_t) band];
        return Processor::divisionNames[(size_t) juce::roundToInt (param->convertFrom0to1 (param->getValue()))];
    }

    return juce::String (juce::roundToInt (getDelayMs (band, side)));
}

//==============================================================================
void BandSpectrumComponent::paint (juce::Graphics& g)
{
    const auto plot = getPlotArea();
    const auto allArea = getAllColumnArea();

    g.setColour (MultiwayPalette::plotFill);
    g.fillRoundedRectangle (plot, 6.0f);
    g.fillRoundedRectangle (allArea, 6.0f);

    // Thin lines at 25 % intervals for the delay axis.
    g.setColour (MultiwayPalette::ink.withAlpha (0.06f));
    for (int i = 1; i < 4; ++i)
    {
        const auto y = (int) std::round (plot.getBottom() - plot.getHeight() * 0.25f * (float) i);
        g.drawHorizontalLine (y, plot.getX(), plot.getRight());
        g.drawHorizontalLine (y, allArea.getX(), allArea.getRight());
    }

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (plot.toNearestInt());

        drawSpectrum (g, plot);
        drawBands (g, plot);
        drawCutShading (g, plot);
    }

    drawAllColumn (g);
    drawBandLabels (g, plot);
}

void BandSpectrumComponent::drawSpectrum (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto sampleRate = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
    const auto binsPerHz = (float) ((double) fftSize / sampleRate);

    auto dbAt = [this, &plot, binsPerHz] (float x0, float x1)
    {
        const auto k0 = xToFrequency (x0, plot) * binsPerHz;
        const auto k1 = xToFrequency (x1, plot) * binsPerHz;

        if (k0 >= (float) (numBins - 1))
            return minDb;

        // Where the bin spacing is narrower than a pixel (low frequencies), interpolate between neighbouring bins;
        // where it is wider (high frequencies), take the highest value in the range.
        if (k1 - k0 < 1.0f)
        {
            const auto k = juce::jlimit (1.0f, (float) (numBins - 1), 0.5f * (k0 + k1));
            const auto i = juce::jmin ((int) k, numBins - 2);
            const auto t = k - (float) i;
            return displayDb[(size_t) i] * (1.0f - t) + displayDb[(size_t) i + 1] * t;
        }

        auto peak = minDb;
        for (int k = juce::jmax (1, (int) std::ceil (k0)); k <= juce::jmin (numBins - 1, (int) k1); ++k)
            peak = juce::jmax (peak, displayDb[(size_t) k]);

        return peak;
    };

    constexpr float step = 2.0f;
    juce::Path line, fill;
    fill.startNewSubPath (plot.getX(), plot.getBottom());

    for (auto x = plot.getX(); x <= plot.getRight(); x += step)
    {
        const auto db = juce::jlimit (minDb, maxDb, dbAt (x, x + step));
        const auto y  = juce::jmap (db, minDb, maxDb, plot.getBottom(), plot.getY());

        if (x == plot.getX())
            line.startNewSubPath (x, y);
        else
            line.lineTo (x, y);

        fill.lineTo (x, y);
    }

    fill.lineTo (plot.getRight(), plot.getBottom());
    fill.closeSubPath();

    g.setColour (MultiwayPalette::spectrum.withAlpha (0.28f));
    g.fillPath (fill);

    g.setColour (MultiwayPalette::spectrum.withAlpha (0.65f));
    g.strokePath (line, juce::PathStrokeType (1.2f));
}

// A single column: background (highlighted if selected) and L / R delay bars (a single bar if equal).
// highlighted: bands written by ALL in General mode, a lighter highlight.
void BandSpectrumComponent::drawColumn (juce::Graphics& g, juce::Rectangle<float> column,
                                        float delayL, float delayR, bool selected, bool highlighted) const
{
    if (selected)
    {
        g.setColour (MultiwayPalette::accent.withAlpha (0.12f));
        g.fillRoundedRectangle (column, 4.0f);
        g.setColour (MultiwayPalette::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (column.reduced (0.75f), 4.0f, 1.5f);
    }
    else if (highlighted)
    {
        g.setColour (MultiwayPalette::accent.withAlpha (0.06f));
        g.fillRoundedRectangle (column, 4.0f);
        g.setColour (MultiwayPalette::accent.withAlpha (0.35f));
        g.drawRoundedRectangle (column.reduced (0.5f), 4.0f, 1.0f);
    }
    else
    {
        g.setColour (MultiwayPalette::ink.withAlpha (0.04f));
        g.fillRoundedRectangle (column, 4.0f);
    }

    const auto heightOf = [&column] (float ms) { return column.getHeight() * juce::jlimit (0.0f, 1.0f, ms / Processor::maxDelayMs); };
    const auto barBase  = juce::jmin (column.getWidth(), maxBarBase);

    g.setColour (selected || highlighted ? MultiwayPalette::accent : MultiwayPalette::bar);

    auto drawBar = [&g, &column] (float centreX, float width, float height)
    {
        if (height <= 0.0f)
            return;

        juce::Path path;
        path.addRoundedRectangle (centreX - width * 0.5f, column.getBottom() - height, width, height,
                                  2.5f, 2.5f, true, true, false, false);
        g.fillPath (path);
    };

    if (std::abs (delayL - delayR) < linkedEqualMs)
    {
        drawBar (column.getCentreX(), barBase * 0.5f, heightOf (delayL));
    }
    else
    {
        const auto barWidth = barBase * 0.22f;
        const auto offset   = barBase * 0.14f;
        drawBar (column.getCentreX() - offset, barWidth, heightOf (delayL));
        drawBar (column.getCentreX() + offset, barWidth, heightOf (delayR));
    }
}

void BandSpectrumComponent::drawBands (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto activeBands = processor.getActiveBandCount();
    const auto anySoloed = (processor.getSoloMask() & ((1u << activeBands) - 1u)) != 0;

    for (int band = 0; band < activeBands; ++band)
    {
        const auto column   = getColumnBounds (band, plot);
        const auto selected = band == selectedBand;
        const auto delayL   = getDelayMs (band, Processor::delayLParam);
        const auto delayR   = getDelayMs (band, Processor::delayRParam);

        drawColumn (g, column, delayL, delayR, selected, selectedBand == allBands);

        // Bands that aren't heard while solo is on are dimmed.
        if (anySoloed && ! isSoloed (band))
        {
            g.setColour (MultiwayPalette::plotFill.withAlpha (0.6f));
            g.fillRoundedRectangle (column, 4.0f);
        }

        // The selected band's value, above the tallest bar.
        if (selected)
        {
            // A channel with sync on shows the division name ("1/8D"), a channel with sync off shows ms.
            const auto syncL = processor.isSynced (Processor::delayLParam);
            const auto syncR = processor.isSynced (Processor::delayRParam);
            const auto textL = delayText (band, Processor::delayLParam);
            const auto textR = delayText (band, Processor::delayRParam);
            const auto same  = std::abs (delayL - delayR) < linkedEqualMs && textL == textR;

            auto text = same ? textL : textL + (! syncL && syncR ? " ms" : "") + " | " + textR;
            if (! (same ? syncL : syncR))
                text += " ms";

            const auto font = juce::Font (juce::FontOptions (11.0f, juce::Font::bold));
            const auto textWidth = juce::GlyphArrangement::getStringWidth (font, text) + 10.0f;

            // The text may overflow the column frame; it sits on a background-coloured pill to stay readable.
            const auto top = column.getBottom() - column.getHeight() * juce::jmax (delayL, delayR) / Processor::maxDelayMs;
            const auto textArea = juce::Rectangle<float> (textWidth, 16.0f)
                                      .withCentre ({ column.getCentreX(), top - 12.0f })
                                      .constrainedWithin (plot);

            g.setColour (MultiwayPalette::plotFill);
            g.fillRoundedRectangle (textArea, 8.0f);
            g.setColour (MultiwayPalette::accent);
            g.drawRoundedRectangle (textArea.reduced (0.5f), 8.0f, 1.0f);
            g.setFont (font);
            g.drawText (text, textArea, juce::Justification::centred);
        }
    }
}

// ALL column: the average L / R delay of the active bands; General mode when selected.
void BandSpectrumComponent::drawAllColumn (juce::Graphics& g) const
{
    const auto activeBands = processor.getActiveBandCount();
    float sumL = 0.0f, sumR = 0.0f;

    for (int band = 0; band < activeBands; ++band)
    {
        sumL += getDelayMs (band, Processor::delayLParam);
        sumR += getDelayMs (band, Processor::delayRParam);
    }

    const auto area = getAllColumnArea();
    drawColumn (g, area.reduced (area.getWidth() * columnGapRatio, 0.0f),
                sumL / (float) activeBands, sumR / (float) activeBands, selectedBand == allBands, false);
}

void BandSpectrumComponent::drawCutShading (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto shade = MultiwayPalette::cutShade;

    // Low cut: fully below 1/3 octave under the cutoff, then fading out up to the cutoff like the DSP ramp.
    const auto lowCutHz = lowCutParam->load();
    if (lowCutHz > Processor::minLowCutHz)
    {
        const auto rampStart = frequencyToX (lowCutHz * std::pow (2.0f, -Processor::lowCutRampOctaves), plot);
        const auto cutX      = frequencyToX (lowCutHz, plot);

        g.setColour (shade);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), plot.getY(), rampStart, plot.getBottom()));

        g.setGradientFill (juce::ColourGradient (shade, rampStart, 0.0f, shade.withAlpha (0.0f), cutX, 0.0f, false));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (rampStart, plot.getY(), cutX, plot.getBottom()));
    }

    // High cut: the mirror image, increasing from the cutoff up to 1/3 octave above it, fully beyond that.
    const auto highCutHz = highCutParam->load();
    if (highCutHz < Processor::maxHighCutHz)
    {
        const auto cutX    = frequencyToX (highCutHz, plot);
        const auto rampEnd = frequencyToX (highCutHz * std::pow (2.0f, Processor::highCutRampOctaves), plot);

        g.setGradientFill (juce::ColourGradient (shade.withAlpha (0.0f), cutX, 0.0f, shade, rampEnd, 0.0f, false));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (cutX, plot.getY(), rampEnd, plot.getBottom()));

        g.setColour (shade);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (rampEnd, plot.getY(), plot.getRight(), plot.getBottom()));
    }
}

void BandSpectrumComponent::drawBandLabels (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto activeBands = processor.getActiveBandCount();

    for (int band = 0; band < activeBands; ++band)
    {
        const auto selected = band == selectedBand;
        const auto name     = bandName (band, activeBands);
        const auto solo     = getSoloButtonBounds (band, plot);
        const auto nameWidth = juce::GlyphArrangement::getStringWidth (labelFont (true), name);

        g.setColour (selected ? MultiwayPalette::accent : MultiwayPalette::ink.withAlpha (0.6f));
        g.setFont (labelFont (selected));
        g.drawText (name, juce::Rectangle<float> (solo.getX() - soloButtonGap - nameWidth, plot.getBottom(), nameWidth, labelAreaHeight),
                    juce::Justification::centred);

        // S button: filled with the accent colour while soloed.
        if (isSoloed (band))
        {
            g.setColour (MultiwayPalette::accent);
            g.fillRoundedRectangle (solo, 4.0f);
            g.setColour (juce::Colours::white);
        }
        else
        {
            g.setColour (MultiwayPalette::ink.withAlpha (0.3f));
            g.drawRoundedRectangle (solo.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (MultiwayPalette::ink.withAlpha (0.5f));
        }

        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText ("S", solo, juce::Justification::centred);
    }

    const auto allSelected = selectedBand == allBands;
    const auto allArea = getAllColumnArea();

    g.setColour (allSelected ? MultiwayPalette::accent : MultiwayPalette::ink.withAlpha (0.6f));
    g.setFont (labelFont (allSelected));
    g.drawText ("ALL", juce::Rectangle<float> (allArea.getX(), allArea.getBottom(), allArea.getWidth(), labelAreaHeight),
                juce::Justification::centred);
}

// Resets the double-clicked band's L / R delay, division and feedback to their defaults.
void BandSpectrumComponent::resetBand (int band)
{
    for (auto* param : { delayLParams[(size_t) band], delayRParams[(size_t) band], feedbackParams[(size_t) band],
                         divisionLParams[(size_t) band], divisionRParams[(size_t) band] })
    {
        if (param->getValue() == param->getDefaultValue())
            continue;

        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getDefaultValue());
        param->endChangeGesture();
    }
}

//==============================================================================
void BandSpectrumComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();

    if (const auto soloBand = getSoloButtonAt (e.position, plot); soloBand >= 0)
    {
        processor.setSoloMask (processor.getSoloMask() ^ (1u << soloBand));
        repaint();
        return;
    }

    // The left side of the plot (the ALL column and the label under it) selects General mode; no dragging.
    if (e.position.x < plot.getX())
    {
        if (onBandSelected != nullptr)
            onBandSelected (allBands);

        return;
    }

    const auto band = getBandAt (e.position.x, plot);

    if (onBandSelected != nullptr)
        onBandSelected (band);

    // With Link off, the left half of the column grabs L and the right half grabs R.
    const auto grabsLeft = e.position.x < getColumnBounds (band, plot).getCentreX();
    const auto grabbedSide = grabsLeft ? Processor::delayLParam : Processor::delayRParam;
    const auto otherSide   = grabsLeft ? Processor::delayRParam : Processor::delayLParam;

    drag.params    = { getDelayTarget (band, grabbedSide), getDelayTarget (band, otherSide) };
    drag.numParams = linked ? 2 : 1;
    drag.startMs   = getDelayMs (band, grabbedSide);

    for (int i = 0; i < drag.numParams; ++i)
        drag.params[(size_t) i]->beginChangeGesture();
}

void BandSpectrumComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (drag.numParams == 0 || ! e.mouseWasDraggedSinceMouseDown())
        return;

    // Dragging up increases the delay; the panel height maps to the full range (0..maxDelayMs).
    const auto deltaMs = -(float) e.getDistanceFromDragStartY() / getPlotArea().getHeight() * Processor::maxDelayMs;
    const auto valueMs = juce::jlimit (0.0f, Processor::maxDelayMs, drag.startMs + deltaMs);

    for (int i = 0; i < drag.numParams; ++i)
    {
        auto* param = drag.params[(size_t) i];
        const auto normalised = msToNormalised (*param, valueMs);

        if (param->getValue() != normalised)
            param->setValueNotifyingHost (normalised);
    }
}

void BandSpectrumComponent::mouseUp (const juce::MouseEvent&)
{
    for (int i = 0; i < drag.numParams; ++i)
        drag.params[(size_t) i]->endChangeGesture();

    drag = {};
}

// A double-click arrives after mouseUp; drag gestures are already closed by then.
void BandSpectrumComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();

    if (e.position.x < plot.getX() || getSoloButtonAt (e.position, plot) >= 0)
        return;

    resetBand (getBandAt (e.position.x, plot));
}

void BandSpectrumComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();
    const auto clickable = e.position.x < plot.getX() || getSoloButtonAt (e.position, plot) >= 0;

    setMouseCursor (clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::UpDownResizeCursor);
}
