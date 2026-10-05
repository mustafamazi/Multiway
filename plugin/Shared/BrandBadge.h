#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// BopsAudio header lockup for the editor's top-left corner: a heptagon knob mark
// (the same shape as the plugin's knobs) next to a two-line wordmark, "BOPSAUDIO"
// over the version. On hover the mark's pointer turns up like a knob being dialled
// in and the text brightens; clicking opens the brand URL.
class BrandBadge final : public juce::Component,
                          private juce::Timer
{
public:
    explicit BrandBadge (juce::Colour textColourToUse);
    ~BrandBadge() override;

    // Wordmark face (e.g. the plugin's Space Mono Bold title font). Without one the
    // default bold font is used.
    void setWordmarkTypeface (juce::Typeface::Ptr);

    // Size needed to fit the mark and wordmark without clipping the mark's shadow.
    int getIdealWidth() const;
    static constexpr int idealHeight = 40;

    void paint (juce::Graphics&) override;

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    juce::Font nameFont() const;

    juce::Colour textColour;
    juce::String versionText;
    juce::Typeface::Ptr wordmarkTypeface;

    float hoverAmount = 0.0f; // 0 = idle, 1 = hovered
    float hoverTarget = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrandBadge)
};
