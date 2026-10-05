// Multiway STFT tests. Each run calls prepareToPlay with a different FFT size and switches to the
// target size inside processBlock (the audio-thread switch path); it also checks that the reported
// latency is updated from the message thread.
//
//  1. Null test: with delay 0, feedback 0, mix 100 %, the output must equal the input delayed by the
//     reported latency sample for sample (difference < -90 dB, excluding the first/last window). Run at
//     different sample rates and block sizes (to show the FIFO works independently of block boundaries).
//  2. Delay accuracy: delay 150 ms on all bands, mix 100 %; the impulse's peak at the output must be at
//     latency + round(0.150 * Fs) samples (±1). Verifies that the frame count is recomputed for
//     the hop.
//  3. LFO: in Rate mode the phase is locked to the DAW position (same values when playback restarts),
//     applyModulation is linear for delay and in octaves for the cuts, and the drawn shape comes back from state.
//
// UI snapshot: MultiwayTests --snapshot <output.png> [lfo]
//  Opens the editor offscreen; the L Delay (+40 %) and Low Cut (-50 %) LFOs are on, and lfo (LfoTarget
//  index, default 0) is selected in the Shaper using a pencil-drawn shape. Audio is processed briefly,
//  then the editor's image is written as a PNG.
//
// Measurement mode (no pass/fail): MultiwayTests --scan <output.csv>
//  Fractional delay scan: N = 2048, delay k frames + r samples (r = 0..H, in steps of 32), 44.1 and
//  48 kHz. For each case the impulse peak's amplitude, the position error and the energy outside ±H of
//  the expected position (wrap-around / aliasing) are written to the CSV. Since wrap-around depends on the
//  impulse's position within the hop, each r is measured at 8 impulse positions in the hop and the worst case is written.

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{
using Processor = MultiwayAudioProcessor;

constexpr double nullThresholdDb = -90.0;
constexpr double noiseSeconds    = 2.0;
constexpr float  testDelayMs     = 150.0f;
constexpr int    delayToleranceSamples = 1;
constexpr int    numChannels     = 2;

constexpr double sampleRates[] = { 44100.0, 48000.0 };
constexpr int    nullBlockSizes[] = { 37, 64, 512, 1024 };
constexpr int    delayBlockSize = 37;

void setParameter (Processor& processor, const juce::String& id, float value)
{
    auto* param = processor.apvts.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

// On every band both channels get delayMs and feedback 0; sync off, cuts off, mix 100 %.
void setDelayParameters (Processor& processor, float delayMs)
{
    for (int band = 0; band < Processor::numBands; ++band)
    {
        setParameter (processor, Processor::bandParamID (band, Processor::delayLParam), delayMs);
        setParameter (processor, Processor::bandParamID (band, Processor::delayRParam), delayMs);
        setParameter (processor, Processor::bandParamID (band, Processor::feedbackParam), 0.0f);
    }

    setParameter (processor, Processor::syncParamID (Processor::delayLParam), 0.0f);
    setParameter (processor, Processor::syncParamID (Processor::delayRParam), 0.0f);
    setParameter (processor, "lowCut",  Processor::minLowCutHz);
    setParameter (processor, "highCut", Processor::maxHighCutHz);
    setParameter (processor, "mix", 100.0f);
}

// Prepares the processor with a size different from fftChoice, then moves the fftSize parameter to the target;
// the switch happens in the first processBlock.
void prepareWithRuntimeSwitch (Processor& processor, double sampleRate, int blockSize, int fftChoice)
{
    const int prepareChoice = (fftChoice + 1) % (int) Processor::fftSizeChoices.size();

    setParameter (processor, "fftSize", (float) prepareChoice);
    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    setParameter (processor, "fftSize", (float) fftChoice);
}

// Processes the buffer in place in blockSize chunks (the last chunk may be shorter).
void processInBlocks (Processor& processor, juce::AudioBuffer<float>& io, int blockSize)
{
    juce::MidiBuffer midi;

    for (int start = 0; start < io.getNumSamples(); start += blockSize)
    {
        const int length = juce::jmin (blockSize, io.getNumSamples() - start);
        juce::AudioBuffer<float> block (io.getArrayOfWritePointers(), numChannels, start, length);
        processor.processBlock (block, midi);
    }
}

// The latency is reported to the host from the processor's timer (message thread); the message loop
// is run until the expected value appears (at most 1 s). Returns the last reported value.
int waitForReportedLatency (Processor& processor, int expected)
{
    for (int elapsed = 0; elapsed < 1000 && processor.getLatencySamples() != expected; elapsed += 10)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    return processor.getLatencySamples();
}

// Output buffer: input + silence so the largest latency and the delay reach the output.
juce::AudioBuffer<float> makeIoBuffer (const juce::AudioBuffer<float>& input, int tailSamples)
{
    juce::AudioBuffer<float> io (numChannels, input.getNumSamples() + tailSamples);
    io.clear();

    for (int channel = 0; channel < numChannels; ++channel)
        io.copyFrom (channel, 0, input, channel, 0, input.getNumSamples());

    return io;
}

//==============================================================================
struct NullResult
{
    int reportedLatency = 0;
    int measuredImpulseDelay = 0;
    double maxErrorDb = 0.0;
};

// One window of silence, an impulse not aligned to the hop, silence, then 2 s of white noise
// (a different seed per channel). The impulse and noise are in the comparison region (excluding the first/last window).
juce::AudioBuffer<float> makeNullInput (double sampleRate, int impulseIndex, int windowLength)
{
    const int noiseStart  = impulseIndex + 2 * windowLength;
    const int noiseLength = juce::roundToInt (noiseSeconds * sampleRate);

    juce::AudioBuffer<float> input (numChannels, noiseStart + noiseLength);
    input.clear();

    for (int channel = 0; channel < numChannels; ++channel)
    {
        juce::Random random (0x4d57 + channel);
        auto* data = input.getWritePointer (channel);

        data[impulseIndex] = 1.0f;

        for (int n = 0; n < noiseLength; ++n)
            data[noiseStart + n] = random.nextFloat() * 2.0f - 1.0f;
    }

    return input;
}

NullResult runNullTest (double sampleRate, int blockSize, int fftChoice)
{
    const int fftSize = Processor::fftSizeForChoice (fftChoice);
    const int windowLength = fftSize;
    const int impulseIndex = windowLength + windowLength / 2 + 13;

    Processor processor;
    setDelayParameters (processor, 0.0f);
    prepareWithRuntimeSwitch (processor, sampleRate, blockSize, fftChoice);

    const auto input = makeNullInput (sampleRate, impulseIndex, windowLength);
    auto io = makeIoBuffer (input, Processor::latencyForFftSize (Processor::fftSizeChoices.back()));
    processInBlocks (processor, io, blockSize);

    NullResult result;
    result.reportedLatency = waitForReportedLatency (processor, Processor::latencyForFftSize (fftSize));
    const int latency = result.reportedLatency;

    // Where the impulse is largest at the output: an independent measurement of the delay.
    {
        const auto* out = io.getReadPointer (0);
        int peak = 0;
        for (int n = 1; n < impulseIndex + 2 * windowLength + latency; ++n)
            if (std::abs (out[n]) > std::abs (out[peak]))
                peak = n;
        result.measuredImpulseDelay = peak - impulseIndex;
    }

    // out[n + latency] is compared with in[n]; excluding the first and last window.
    float maxError = 0.0f;
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* in  = input.getReadPointer (channel);
        const auto* out = io.getReadPointer (channel);

        for (int n = windowLength; n < input.getNumSamples() - windowLength; ++n)
            maxError = juce::jmax (maxError, std::abs (out[n + latency] - in[n]));
    }

    result.maxErrorDb = juce::Decibels::gainToDecibels ((double) maxError, -400.0);
    processor.releaseResources();
    return result;
}

//==============================================================================
struct DelayResult
{
    int reportedLatency = 0;
    int expectedPeak = 0;                       // latency + round(0.150 * Fs), counted from the impulse
    std::array<int, numChannels> peak {};       // measured peak per channel (counted from the impulse)
    std::array<float, numChannels> peakGain {};
};

DelayResult runDelayTest (double sampleRate, int fftChoice)
{
    const int fftSize = Processor::fftSizeForChoice (fftChoice);
    const int delaySamples = juce::roundToInt (testDelayMs * 0.001 * sampleRate);
    const int impulseIndex = fftSize + 13;

    Processor processor;
    setDelayParameters (processor, testDelayMs);
    prepareWithRuntimeSwitch (processor, sampleRate, delayBlockSize, fftChoice);

    juce::AudioBuffer<float> input (numChannels, impulseIndex + 1);
    input.clear();
    for (int channel = 0; channel < numChannels; ++channel)
        input.setSample (channel, impulseIndex, 1.0f);

    auto io = makeIoBuffer (input, Processor::latencyForFftSize (Processor::fftSizeChoices.back()) + delaySamples + 2 * fftSize);
    processInBlocks (processor, io, delayBlockSize);

    DelayResult result;
    result.reportedLatency = waitForReportedLatency (processor, Processor::latencyForFftSize (fftSize));
    result.expectedPeak = result.reportedLatency + delaySamples;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        const auto* out = io.getReadPointer (channel);
        int peak = 0;
        for (int n = 1; n < io.getNumSamples(); ++n)
            if (std::abs (out[n]) > std::abs (out[peak]))
                peak = n;

        result.peak[(size_t) channel] = peak - impulseIndex;
        result.peakGain[(size_t) channel] = out[peak];
    }

    processor.releaseResources();
    return result;
}

//==============================================================================
// Fractional delay scan. The delay parameters have 1 ms steps; for sample-accurate delay
// sync mode is used: 1/4 note on all bands, and the playhead's BPM gives the delay exactly
// (ms = 60000 / BPM, not rounded).
constexpr int scanFftChoice   = 2;      // N = 2048
constexpr int scanWholeFrames = 10;     // k
constexpr int scanStep        = 32;     // r step (samples)
constexpr int scanBlockSize   = 512;
constexpr int scanImpulsePhases = 8;    // evenly spaced impulse positions within the hop (+ scanPhaseOffset)
constexpr int scanPhaseOffset = 13;     // start not aligned to the hop
constexpr double scanWarmUpSeconds = 0.5;   // so the BPM is read and the band smoothing (100 ms) settles
constexpr double silenceDb = -200.0;

struct FixedTempoPlayHead final : juce::AudioPlayHead
{
    double bpm = 120.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        return info;
    }
};

int quarterNoteDivision()
{
    for (int i = 0; i < Processor::numDivisions; ++i)
        if (Processor::divisionWholeNotes[(size_t) i] == 0.25)
            return i;

    jassertfalse;
    return 0;
}

struct ScanPoint
{
    float peakGain = 0.0f;       // signed value of the peak
    int positionError = 0;       // peak - expected position
    double aliasEnergyDb = 0.0;  // energy outside ±H of the expected position, relative to the impulse energy (1)
};

// A single measurement: delay delaySamples, impulse impulseOffset samples after the warm-up. Since the warm-up
// is a multiple of H, the impulse's position within the hop is impulseOffset % H (independent of the sample rate).
ScanPoint runScanPoint (double sampleRate, int delaySamples, int impulseOffset)
{
    const int fftSize = Processor::fftSizeForChoice (scanFftChoice);
    const int hopSize = fftSize / 4;

    ScanPoint point;

    FixedTempoPlayHead playHead;
    playHead.bpm = 60000.0 / ((double) delaySamples * 1000.0 / sampleRate);

    Processor processor;
    processor.setPlayHead (&playHead);
    setDelayParameters (processor, 0.0f);

    const auto division = (float) quarterNoteDivision();
    for (auto side : { Processor::delayLParam, Processor::delayRParam })
    {
        setParameter (processor, Processor::syncParamID (side), 1.0f);
        for (int band = 0; band < Processor::numBands; ++band)
            setParameter (processor, Processor::bandDivisionID (band, side), division);
    }

    setParameter (processor, "fftSize", (float) scanFftChoice);
    processor.setRateAndBufferSizeDetails (sampleRate, scanBlockSize);
    processor.prepareToPlay (sampleRate, scanBlockSize);

    // Warm-up: silence; the BPM is read in processBlock and the delay smoother settles on its target.
    const int warmUpHops = (int) std::ceil (scanWarmUpSeconds * sampleRate / hopSize);
    juce::AudioBuffer<float> warmUp (numChannels, warmUpHops * hopSize);
    warmUp.clear();
    processInBlocks (processor, warmUp, scanBlockSize);

    const int latency = processor.getLatencySamples();
    const int expected = impulseOffset + latency + delaySamples;

    juce::AudioBuffer<float> io (numChannels, expected + 3 * fftSize);
    io.clear();
    for (int channel = 0; channel < numChannels; ++channel)
        io.setSample (channel, impulseOffset, 1.0f);

    processInBlocks (processor, io, scanBlockSize);
    processor.setPlayHead (nullptr);

    // L channel (R gets the same delay).
    const auto* out = io.getReadPointer (0);
    int peak = 0;
    double aliasEnergy = 0.0;

    for (int n = 0; n < io.getNumSamples(); ++n)
    {
        if (std::abs (out[n]) > std::abs (out[peak]))
            peak = n;

        if (std::abs (n - expected) > hopSize)
            aliasEnergy += (double) out[n] * (double) out[n];
    }

    point.peakGain = out[peak];
    point.positionError = peak - expected;
    point.aliasEnergyDb = aliasEnergy > 0.0 ? juce::jmax (silenceDb, 10.0 * std::log10 (aliasEnergy)) : silenceDb;
    return point;
}

int runFractionalDelayScan (const juce::File& csvFile)
{
    const int fftSize = Processor::fftSizeForChoice (scanFftChoice);
    const int hopSize = fftSize / 4;

    // peak_gain / position_error / alias_energy_db: the worst case across impulse positions
    // (smallest |amplitude|, largest |position error|, largest aliasing); peak_gain_max is the best.
    juce::String csv ("sample_rate,k_frames,r_samples,r_over_N,delay_samples,peak_gain,peak_gain_db,"
                      "peak_gain_max,position_error,alias_energy_db\n");

    std::printf ("Kesirli gecikme taramasi: N = %d, H = %d, gecikme = %d frame + r ornek, %d impuls konumunun en kotusu\n\n",
                 fftSize, hopSize, scanWholeFrames, scanImpulsePhases);
    std::printf ("  sampleRate     r    r/N  genlik min  genlik dB  genlik max  konum hatasi  aliasing\n");

    for (double sampleRate : sampleRates)
    {
        for (int r = 0; r <= hopSize; r += scanStep)
        {
            const int delaySamples = scanWholeFrames * hopSize + r;
            ScanPoint worst, best;
            worst.peakGain = std::numeric_limits<float>::max();
            worst.aliasEnergyDb = silenceDb;

            for (int phase = 0; phase < scanImpulsePhases; ++phase)
            {
                const auto point = runScanPoint (sampleRate, delaySamples, scanPhaseOffset + phase * hopSize / scanImpulsePhases);

                if (std::abs (point.peakGain) < std::abs (worst.peakGain))  worst.peakGain = point.peakGain;
                if (std::abs (point.peakGain) > std::abs (best.peakGain))   best.peakGain  = point.peakGain;
                if (std::abs (point.positionError) > std::abs (worst.positionError)) worst.positionError = point.positionError;
                worst.aliasEnergyDb = juce::jmax (worst.aliasEnergyDb, point.aliasEnergyDb);
            }

            const double gainDb = juce::Decibels::gainToDecibels ((double) std::abs (worst.peakGain), silenceDb);

            std::printf ("  %7.0f Hz  %4d  %.4f  %10.4f  %9.2f  %10.4f  %12d  %6.1f dB\n",
                         sampleRate, r, (double) r / fftSize, (double) worst.peakGain, gainDb,
                         (double) best.peakGain, worst.positionError, worst.aliasEnergyDb);

            csv << juce::String (sampleRate, 0) << ',' << scanWholeFrames << ',' << r << ','
                << juce::String ((double) r / fftSize, 6) << ',' << delaySamples << ','
                << juce::String ((double) worst.peakGain, 6) << ',' << juce::String (gainDb, 3) << ','
                << juce::String ((double) best.peakGain, 6) << ','
                << worst.positionError << ',' << juce::String (worst.aliasEnergyDb, 2) << '\n';
        }
    }

    if (! csvFile.getParentDirectory().createDirectory() || ! csvFile.replaceWithText (csv))
    {
        std::printf ("\nCSV yazilamadi: %s\n", csvFile.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("\nCSV: %s\n", csvFile.getFullPathName().toRawUTF8());
    return 0;
}

//==============================================================================
// DAW transport for the LFO tests: tempo, position (quarter notes) and play state.
struct TransportPlayHead final : juce::AudioPlayHead
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = false;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        return info;
    }
};

// In Rate mode (1/4, amount 100 %, sine) each frame's modulation while playing must be sin(2π · ppq / cycle);
// after stopping (the LFO runs free) and playing from the start, the same values must come back. Block = hop, so
// each block processes a single frame at its end. Returns the largest deviation.
double runLfoPositionLockTest (double sampleRate)
{
    constexpr int blockSize = 512;   // hop at N = 2048
    constexpr int playBlocks = 200, stopBlocks = 37;
    const int division = 7;          // 1/4 -> one cycle per beat
    jassert (Processor::lfoDivisionWholeNotes[(size_t) division] == 0.25);

    TransportPlayHead playHead;
    Processor processor;
    processor.setPlayHead (&playHead);

    setParameter (processor, "fftSize", 2.0f);
    setParameter (processor, Processor::lfoParamID (Processor::lfoDelayL, Processor::lfoSyncParam), 1.0f);
    setParameter (processor, Processor::lfoParamID (Processor::lfoDelayL, Processor::lfoDivisionParam), (float) division);
    setParameter (processor, Processor::lfoParamID (Processor::lfoDelayL, Processor::lfoAmountParam), 100.0f);

    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> block (numChannels, blockSize);
    juce::MidiBuffer midi;
    const double beatsPerBlock = blockSize / sampleRate * playHead.bpm / 60.0;
    double maxError = 0.0;

    auto play = [&]
    {
        playHead.playing = true;
        playHead.ppq = 0.0;

        for (int i = 0; i < playBlocks; ++i)
        {
            block.clear();
            processor.processBlock (block, midi);

            const auto ppqAtFrame = playHead.ppq + beatsPerBlock;   // frame at the end of the block
            const auto expected = std::sin (juce::MathConstants<double>::twoPi * (ppqAtFrame - std::floor (ppqAtFrame)));
            maxError = juce::jmax (maxError, std::abs ((double) processor.getLfoModulation (Processor::lfoDelayL) - expected));

            playHead.ppq += beatsPerBlock;
        }
    };

    play();

    playHead.playing = false;          // while stopped the position is fixed and the LFO runs free
    for (int i = 0; i < stopBlocks; ++i)
    {
        block.clear();
        processor.processBlock (block, midi);
    }

    play();                            // from the start: the lock must give the same values

    processor.setPlayHead (nullptr);
    return maxError;
}

// applyModulation: delay linear (ms), cuts symmetric in octaves, clipping at the limits.
bool checkApplyModulation()
{
    const auto near = [] (float a, float b) { return std::abs (a - b) <= 1.0e-3f * juce::jmax (1.0f, std::abs (b)); };
    const auto lowCutOctaves = std::log2 (Processor::maxLowCutHz / Processor::minLowCutHz);

    return near (Processor::applyModulation (Processor::lfoDelayL, 500.0f, 0.1f), 700.0f)
        && near (Processor::applyModulation (Processor::lfoDelayR, 1900.0f, 0.5f), Processor::maxDelayMs)
        && near (Processor::applyModulation (Processor::lfoFeedback, 10.0f, -0.5f), 0.0f)
        && near (Processor::applyModulation (Processor::lfoMix, 50.0f, 0.25f), 75.0f)
        && near (Processor::applyModulation (Processor::lfoLowCut, 100.0f, 0.25f), 100.0f * std::exp2 (0.25f * lowCutOctaves))
        && near (Processor::applyModulation (Processor::lfoLowCut, 100.0f, -0.25f), 100.0f / std::exp2 (0.25f * lowCutOctaves))
        && near (Processor::applyModulation (Processor::lfoHighCut, 15000.0f, 0.5f), Processor::maxHighCutHz)
        && Processor::applyModulation (Processor::lfoLowCut, 440.0f, 0.0f) == 440.0f;
}

// The drawn shape and pencil state are carried to a new instance via getStateInformation / setStateInformation.
bool checkShapeStateRoundTrip()
{
    MultiwayLfo::Table ramp {};
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        ramp[(size_t) i] = -1.0f + 2.0f * (float) i / (float) (MultiwayLfo::shapeSize - 1);

    juce::MemoryBlock state;
    {
        Processor source;
        source.setLfoDrawnShape (Processor::lfoHighCut, ramp, true);
        source.setUsesDrawnShape (Processor::lfoHighCut, true);
        source.getStateInformation (state);
    }

    Processor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());

    if (! restored.usesDrawnShape (Processor::lfoHighCut) || restored.usesDrawnShape (Processor::lfoDelayL))
        return false;

    const auto points = restored.getLfoDrawnShape (Processor::lfoHighCut);
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        if (std::abs (points[(size_t) i] - ramp[(size_t) i]) > 1.0e-4f)
            return false;

    return std::abs (restored.evaluateLfoShape (Processor::lfoHighCut, 0.5) - ramp[64]) < 1.0e-4f;
}
} // namespace

// Writes the editor's image to a PNG (see the top of the file).
int writeEditorSnapshot (const juce::File& pngFile, int selectedLfo)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    TransportPlayHead playHead;
    Processor processor;
    processor.setPlayHead (&playHead);

    setParameter (processor, Processor::lfoParamID (Processor::lfoDelayL, Processor::lfoAmountParam), 40.0f);
    setParameter (processor, Processor::lfoParamID (Processor::lfoLowCut, Processor::lfoAmountParam), -50.0f);
    setParameter (processor, "lowCut", 150.0f);

    MultiwayLfo::Table zigzag {};
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        zigzag[(size_t) i] = i < 32 ? (float) i / 32.0f : i < 96 ? 1.0f - (float) (i - 32) / 32.0f : -1.0f + (float) (i - 96) / 32.0f;

    processor.setLfoDrawnShape (selectedLfo, zigzag, true);
    processor.setUsesDrawnShape (selectedLfo, true);
    processor.apvts.state.setProperty ("selectedLfo", selectedLfo, nullptr);

    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> block (numChannels, blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < 30; ++i)
    {
        block.clear();
        processor.processBlock (block, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setSize (900, 720);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (150);   // editor timer: phase, rings

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    editor.reset();
    processor.setPlayHead (nullptr);

    pngFile.deleteFile();
    juce::FileOutputStream stream (pngFile);
    juce::PNGImageFormat png;

    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::printf ("PNG yazilamadi: %s\n", pngFile.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("%s\n", pngFile.getFullPathName().toRawUTF8());
    return 0;
}

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // message loop for the APVTS and latency timers

    if (argc >= 3 && std::strcmp (argv[1], "--snapshot") == 0)
        return writeEditorSnapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]),
                                    argc >= 4 ? juce::jlimit (0, Processor::numLfos - 1, std::atoi (argv[3])) : 0);

    if (argc == 3 && std::strcmp (argv[1], "--scan") == 0)
        return runFractionalDelayScan (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    bool allPassed = true;
    const auto numFftSizes = (int) Processor::fftSizeChoices.size();

    std::printf ("1) Null testi (delay 0, mix %%100): latency = N ve fark < %.0f dB\n\n", nullThresholdDb);
    std::printf ("     N  sampleRate  blok  latency  impuls gecikmesi  max fark\n");

    for (int fftChoice = 0; fftChoice < numFftSizes; ++fftChoice)
    {
        const int fftSize = Processor::fftSizeForChoice (fftChoice);

        for (double sampleRate : sampleRates)
        {
            for (int blockSize : nullBlockSizes)
            {
                const auto result = runNullTest (sampleRate, blockSize, fftChoice);
                const bool passed = result.reportedLatency == fftSize
                                 && result.measuredImpulseDelay == result.reportedLatency
                                 && result.maxErrorDb < nullThresholdDb;
                allPassed = allPassed && passed;

                std::printf ("  %4d  %7.0f Hz  %4d  %7d  %16d  %8.1f dB  %s\n",
                             fftSize, sampleRate, blockSize, result.reportedLatency, result.measuredImpulseDelay,
                             result.maxErrorDb, passed ? "OK" : "BASARISIZ");
            }
        }
    }

    std::printf ("\n2) Gecikme dogrulugu (delay %.0f ms, mix %%100, blok %d): tepe = latency + round(%.3f * Fs) +/-%d\n\n",
                 (double) testDelayMs, delayBlockSize, testDelayMs * 0.001, delayToleranceSamples);
    std::printf ("     N  sampleRate  latency  beklenen tepe  tepe L  tepe R  genlik L  genlik R\n");

    for (int fftChoice = 0; fftChoice < numFftSizes; ++fftChoice)
    {
        const int fftSize = Processor::fftSizeForChoice (fftChoice);

        for (double sampleRate : sampleRates)
        {
            const auto result = runDelayTest (sampleRate, fftChoice);

            bool passed = result.reportedLatency == fftSize;
            for (auto peak : result.peak)
                passed = passed && std::abs (peak - result.expectedPeak) <= delayToleranceSamples;
            allPassed = allPassed && passed;

            std::printf ("  %4d  %7.0f Hz  %7d  %13d  %6d  %6d  %8.3f  %8.3f  %s\n",
                         fftSize, sampleRate, result.reportedLatency, result.expectedPeak,
                         result.peak[0], result.peak[1], (double) result.peakGain[0], (double) result.peakGain[1],
                         passed ? "OK" : "BASARISIZ");
        }
    }

    std::printf ("\n3) LFO\n\n");

    for (double sampleRate : sampleRates)
    {
        const auto error = runLfoPositionLockTest (sampleRate);
        const bool passed = error < 1.0e-4;
        allPassed = allPassed && passed;
        std::printf ("  ppq kilidi (1/4, %.0f Hz): en buyuk sapma %.2e  %s\n", sampleRate, error, passed ? "OK" : "BASARISIZ");
    }

    const bool modulationOk = checkApplyModulation();
    const bool shapeOk = checkShapeStateRoundTrip();
    allPassed = allPassed && modulationOk && shapeOk;
    std::printf ("  applyModulation (delay dogrusal, cut oktav, kirpma)  %s\n", modulationOk ? "OK" : "BASARISIZ");
    std::printf ("  cizilen sekil state'ten geri geliyor                %s\n", shapeOk ? "OK" : "BASARISIZ");

    std::printf ("\n%s\n", allPassed ? "Tum testler gecti." : "Test BASARISIZ.");
    return allPassed ? 0 : 1;
}
