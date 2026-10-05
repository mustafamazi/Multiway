#include "BrandBadge.h"
#include "ClipotypeLookAndFeel.h"

namespace
{
    const juce::String brandUrl { "https://bopsaudio.com" };

    const juce::String brandName { "BOPSAUDIO" };

    constexpr float markRadius  = 14.0f; // ~28px heptagon
    constexpr float markInset   = 3.0f;  // room for the drop shadow
    constexpr float markGap     = 10.0f;

    // Pointer angles (radians, 0 = 12 o'clock): idle sits at ~10 o'clock, hover turns it to ~2.
    constexpr float idlePointerAngle  = -1.05f;
    constexpr float hoverPointerAngle =  1.05f;

    constexpr float idleTextAlpha    = 0.8f;
    constexpr float hoverTextAlpha   = 1.0f;
    constexpr float versionAlpha     = 0.55f; // relative to the name, so the version reads fainter

    // ~150ms time constant at 60fps, matching the editor's label cross-fades
    constexpr float hoverSmoothingCoeff = 0.894f;

    juce::Font versionFont()
    {
        return juce::Font (juce::FontOptions (9.5f)).withExtraKerningFactor (0.14f);
    }
}

//==============================================================================
BrandBadge::BrandBadge (juce::Colour textColourToUse)
    : textColour (textColourToUse),
      versionText ("v" + juce::String (JucePlugin_VersionString))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTitle (brandName);
    setDescription ("Opens " + brandUrl);
}

BrandBadge::~BrandBadge()
{
    stopTimer();
}

void BrandBadge::setWordmarkTypeface (juce::Typeface::Ptr typeface)
{
    wordmarkTypeface = std::move (typeface);
    repaint();
}

juce::Font BrandBadge::nameFont() const
{
    const auto options = wordmarkTypeface != nullptr ? juce::FontOptions (wordmarkTypeface).withHeight (17.0f)
                                                     : juce::FontOptions (17.0f, juce::Font::bold);
    return juce::Font (options).withExtraKerningFactor (0.06f);
}

int BrandBadge::getIdealWidth() const
{
    const auto textWidth = juce::jmax (juce::GlyphArrangement::getStringWidth (nameFont(), brandName),
                                       juce::GlyphArrangement::getStringWidth (versionFont(), versionText));

    return (int) std::ceil (markInset + markRadius * 2.0f + markGap + textWidth) + 2;
}

void BrandBadge::mouseEnter (const juce::MouseEvent&)
{
    hoverTarget = 1.0f;
    startTimerHz (60);
}

void BrandBadge::mouseExit (const juce::MouseEvent&)
{
    hoverTarget = 0.0f;
    startTimerHz (60);
}

void BrandBadge::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && getLocalBounds().contains (e.getPosition()))
        juce::URL (brandUrl).launchInDefaultBrowser();
}

void BrandBadge::timerCallback()
{
    hoverAmount = hoverSmoothingCoeff * hoverAmount + (1.0f - hoverSmoothingCoeff) * hoverTarget;

    if (std::abs (hoverAmount - hoverTarget) < 0.001f)
    {
        hoverAmount = hoverTarget;
        stopTimer();
    }

    repaint();
}

void BrandBadge::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    bounds.removeFromLeft (markInset);

    // Mark: a small version of the plugin's knob, with the same flat ink body and white pointer.
    const auto centre = juce::Point<float> (bounds.getX() + markRadius, bounds.getCentreY() - 1.0f);
    const auto heptagon = ClipotypeLookAndFeel::buildHeptagonPath (centre, markRadius);

    juce::DropShadow (juce::Colours::black.withAlpha (0.22f), 5, { 0, 2 }).drawForPath (g, heptagon);
    g.setColour (textColour);
    g.fillPath (heptagon);

    const auto angle = juce::jmap (hoverAmount, idlePointerAngle, hoverPointerAngle);
    juce::Path pointer;
    pointer.startNewSubPath (centre);
    pointer.lineTo (centre.getPointOnCircumference (markRadius * 0.78f, angle));
    g.setColour (juce::Colours::white);
    g.strokePath (pointer, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    bounds.removeFromLeft (markRadius * 2.0f + markGap);

    // Wordmark: name and version stacked so the block is centred on the mark.
    const auto textAlpha = juce::jmap (hoverAmount, idleTextAlpha, hoverTextAlpha);
    const auto x = (int) bounds.getX();

    g.setColour (textColour.withAlpha (textAlpha));
    g.setFont (nameFont());
    g.drawSingleLineText (brandName, x, (int) std::round (centre.y + 1.0f));

    g.setColour (textColour.withAlpha (textAlpha * versionAlpha));
    g.setFont (versionFont());
    g.drawSingleLineText (versionText, x, (int) std::round (centre.y + 13.0f));
}
