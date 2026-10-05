#pragma once

#include <JuceHeader.h>

//==============================================================================
// Audio-thread state of a single LFO. Each frame (at hop rate) the processor advances the
// position or locks it to the DAW position, then reads the output via process(). No allocations.
//
// The position is in cycles: the integer part is the cycle count, the fractional part is the phase (0..1).
class MultiwayLfo
{
public:
    static constexpr int shapeSize = 128;   // number of points in the drawn shape table
    using Table = std::array<float, shapeSize>;

    // Resets the phase and state; seed determines the jitter sequence (different per LFO).
    void reset (juce::int64 newSeed) noexcept
    {
        seed = newSeed;
        position = 0.0;
        smoothed = 0.0f;
        cycle = std::numeric_limits<juce::int64>::min();
    }

    // Free running: advances the position by the given number of cycles.
    void advance (double cycles) noexcept              { position += cycles; }

    // Tempo lock: the position comes directly from the DAW position (in cycles).
    void setPosition (double cycles) noexcept          { position = cycles; }

    // Phase with offset applied (0..1); the shape is read at this phase, and the UI marker shows it too.
    double getPhase (float offset) const noexcept
    {
        const auto phase = position + (double) offset;
        return phase - std::floor (phase);
    }

    // Output (-1..1) with shape (shape(phase) -> -1..1), jitter and smoothing applied.
    // jitter 0..1: the shape is blended by this amount towards a random value per cycle (1 = a step per cycle).
    // smoothCoefficient 0..1: per-frame one-pole coefficient (1 = no smoothing).
    template <typename ShapeFunction>
    float process (ShapeFunction&& shape, float offset, float jitter, float smoothCoefficient) noexcept
    {
        updateJitterValue();

        const auto shaped = shape (getPhase (offset));
        const auto target = shaped + (jitterValue - shaped) * jitter;

        smoothed += (target - smoothed) * smoothCoefficient;
        return smoothed;
    }

    // Table: 128 points, point i at phase i / 128; linear and circular in between.
    template <typename PointReader>
    static float readTable (PointReader&& point, double phase) noexcept
    {
        const auto x = phase * shapeSize;
        const auto index = juce::jlimit (0, shapeSize - 1, (int) x);
        const auto frac = (float) (x - index);
        const auto a = point (index), b = point ((index + 1) % shapeSize);
        return a + (b - a) * frac;
    }

    static float sine (double phase) noexcept   { return (float) std::sin (juce::MathConstants<double>::twoPi * phase); }

    static Table sineTable() noexcept
    {
        Table table {};
        for (int i = 0; i < shapeSize; ++i)
            table[(size_t) i] = sine ((double) i / shapeSize);
        return table;
    }

private:
    // Refreshes the jitter value when a new cycle starts. The value is derived from the cycle count:
    // when playback restarts from the same position, the same sequence repeats.
    void updateJitterValue() noexcept
    {
        const auto currentCycle = (juce::int64) std::floor (position);
        if (currentCycle == cycle)
            return;

        cycle = currentCycle;
        const auto mixed = (juce::uint64) seed ^ ((juce::uint64) currentCycle * 0x9E3779B97F4A7C15ULL);
        juce::Random random ((juce::int64) mixed);
        jitterValue = random.nextFloat() * 2.0f - 1.0f;
    }

    double position = 0.0;
    float smoothed = 0.0f;
    float jitterValue = 0.0f;
    juce::int64 cycle = std::numeric_limits<juce::int64>::min();
    juce::int64 seed = 0;
};
