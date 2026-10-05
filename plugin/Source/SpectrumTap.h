#pragma once

#include <JuceHeader.h>

//==============================================================================
// Passes the input signal from the audio thread to the UI. Works lock-free with juce::AbstractFifo
// between a single producer (processBlock) and a single consumer (the editor's timer).
// The buffer is allocated once in the constructor; push() takes no locks and doesn't allocate.
// If the FIFO is full (e.g. while the editor is closed), samples that don't fit are dropped.
class SpectrumTap
{
public:
    static constexpr int capacity = 1 << 15;   // ~0.7 s @ 48 kHz

    SpectrumTap() : buffer ((size_t) capacity, 0.0f) {}

    // Audio thread: writes the average of the channels (mono) into the FIFO.
    void push (const juce::AudioBuffer<float>& input, int numChannels) noexcept
    {
        if (numChannels <= 0)
            return;

        const auto scope = fifo.write (input.getNumSamples());
        const float gain = 1.0f / (float) numChannels;

        auto copy = [&] (int start, int size, int sourceOffset)
        {
            for (int i = 0; i < size; ++i)
            {
                float sum = 0.0f;
                for (int channel = 0; channel < numChannels; ++channel)
                    sum += input.getReadPointer (channel)[sourceOffset + i];

                buffer[(size_t) (start + i)] = sum * gain;
            }
        };

        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    // Message thread: hands over all ready samples in order via consume (const float*, int).
    template <typename Consumer>
    void pull (Consumer&& consume)
    {
        const auto scope = fifo.read (fifo.getNumReady());

        if (scope.blockSize1 > 0) consume (buffer.data() + scope.startIndex1, scope.blockSize1);
        if (scope.blockSize2 > 0) consume (buffer.data() + scope.startIndex2, scope.blockSize2);
    }

private:
    juce::AbstractFifo fifo { capacity };
    std::vector<float> buffer;

    JUCE_DECLARE_NON_COPYABLE (SpectrumTap)
};
