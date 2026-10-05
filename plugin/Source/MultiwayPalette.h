#pragma once

#include <JuceHeader.h>
#include "ClipotypeLookAndFeel.h"

//==============================================================================
// Multiway colours: a pastel background between purple and pink. Text and knobs share
// Clipotype's ink colour (ClipotypeLookAndFeel::textColourArgb); the accent and spectrum
// colours were chosen dark enough to stay readable on the pastel background.
namespace MultiwayPalette
{
    inline const juce::Colour ink              { ClipotypeLookAndFeel::textColourArgb };

    inline const juce::Colour backgroundTop    { 0xFFCDC2E0 };   // lavender
    inline const juce::Colour backgroundBottom { 0xFFE1C1D3 };   // pastel pink

    inline const juce::Colour panelFill        { 0x4DFFFFFF };   // white, 30 %
    inline const juce::Colour panelOutline     = ink.withAlpha (0.08f);
    inline const juce::Colour plotFill         { 0xFFD1B9DE };   // spectrum panel background (lilac)

    inline const juce::Colour accent           { 0xFF8A3F86 };   // selected band, Link, band label (plum)
    inline const juce::Colour spectrum         { 0xFF6F58A8 };   // live spectrum (violet)

    inline const juce::Colour bar              = ink.withAlpha (0.55f);   // unselected band bars
    inline const juce::Colour cutShade         = ink.withAlpha (0.20f);   // region removed by low/high cut
}
