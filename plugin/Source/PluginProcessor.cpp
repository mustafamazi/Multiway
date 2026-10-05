/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
MultiwayAudioProcessor::MultiwayAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#else
     :
#endif
       apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (int band = 0; band < numBands; ++band)
        for (auto param : { delayLParam, delayRParam, feedbackParam })
            bandParams[(size_t) bandParamIndex (band, param)] = apvts.getRawParameterValue (bandParamID (band, param));

    lowCutParam  = apvts.getRawParameterValue ("lowCut");
    highCutParam = apvts.getRawParameterValue ("highCut");
    mixParam     = apvts.getRawParameterValue ("mix");
    bandsParam   = apvts.getRawParameterValue ("bands");
    fftSizeParam = apvts.getRawParameterValue ("fftSize");

    for (auto side : { delayLParam, delayRParam })
    {
        const int sideIndex = side == delayRParam ? 1 : 0;
        syncParams[(size_t) sideIndex] = apvts.getRawParameterValue (syncParamID (side));

        for (int band = 0; band < numBands; ++band)
            divisionParams[(size_t) (band * 2 + sideIndex)] = apvts.getRawParameterValue (bandDivisionID (band, side));
    }

    for (int lfo = 0; lfo < numLfos; ++lfo)
        for (int param = 0; param < lfoParamsPerLfo; ++param)
            lfoParams[(size_t) (lfo * lfoParamsPerLfo + param)] = apvts.getRawParameterValue (lfoParamID (lfo, (LfoParam) param));

    // Drawn shapes start as a sine: the first time the pencil is turned on, drawing starts from the sine.
    const auto sine = MultiwayLfo::sineTable();
    for (auto& shape : lfoShapes)
        for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
            shape.points[(size_t) i].store (sine[(size_t) i]);

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        lfoModulation[(size_t) lfo].store (0.0f);
        lfoPhase[(size_t) lfo].store (0.0);
    }

    // FFT objects and windows don't depend on the sample rate; they are prepared once for all four sizes.
    for (int i = 0; i < numFftSizes; ++i)
    {
        const int size = fftSizeChoices[(size_t) i];
        jassert (size == 1 << (minFftOrder + i));

        ffts[(size_t) i] = std::make_unique<juce::dsp::FFT> (minFftOrder + i);

        // sqrt(periodic Hann): N in the denominator (not N-1), so the window fits the hop exactly.
        auto& window = windows[(size_t) i];
        window.resize ((size_t) size);
        for (int n = 0; n < size; ++n)
            window[(size_t) n] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) size));

        // COLA: analysis * synthesis = Hann. Periodic Hann sums to a constant 2.0 at 75 % overlap;
        // for any n, the hop-spaced sum of w^2 gives this constant.
        float overlapSum = 0.0f;
        for (int n = 0; n < size; n += size / 4)
            overlapSum += window[(size_t) n] * window[(size_t) n];
        colaGains[(size_t) i] = 1.0f / overlapSum;
    }

    // The latency of an FFT size changed on the audio thread is reported to the host from here.
    startTimerHz (20);
}

// FFT size choice index -> N (512, 1024, 2048, 4096).
int MultiwayAudioProcessor::fftSizeForChoice (int choiceIndex)
{
    return fftSizeChoices[(size_t) juce::jlimit (0, numFftSizes - 1, choiceIndex)];
}

// Bands choice index -> number of active bands (10, 4, 2).
int MultiwayAudioProcessor::bandCountForChoice (int choiceIndex)
{
    return bandCountChoices[(size_t) juce::jlimit (0, (int) bandCountChoices.size() - 1, choiceIndex)];
}

// Centre frequency of band among N active bands: starting at 31.25 Hz, each band 9 / (N - 1) octaves higher.
float MultiwayAudioProcessor::bandCentreHz (int band, int activeBands)
{
    const float octavesPerBand = bandSpanOctaves / (float) juce::jmax (1, activeBands - 1);
    return lowestCentreHz * std::pow (2.0f, octavesPerBand * (float) band);
}

int MultiwayAudioProcessor::getActiveBandCount() const
{
    return bandCountForChoice (juce::roundToInt (bandsParam->load()));
}

// APVTS ID such as "band3_delayL".
juce::String MultiwayAudioProcessor::bandParamID (int band, BandParam param)
{
    static constexpr const char* suffixes[] = { "delayL", "delayR", "feedback" };
    return "band" + juce::String (band) + "_" + suffixes[param];
}

juce::String MultiwayAudioProcessor::syncParamID (BandParam side)
{
    return side == delayRParam ? "syncR" : "syncL";
}

// APVTS ID such as "band3_divL".
juce::String MultiwayAudioProcessor::bandDivisionID (int band, BandParam side)
{
    return "band" + juce::String (band) + (side == delayRParam ? "_divR" : "_divL");
}

// Note division -> ms: division (in whole notes) * 4 beats * 60000 / BPM.
float MultiwayAudioProcessor::divisionToMs (int divisionIndex, double bpm)
{
    const auto index = (size_t) juce::jlimit (0, numDivisions - 1, divisionIndex);
    return (float) (divisionWholeNotes[index] * 4.0 * 60000.0 / bpm);
}

// APVTS ID such as "lfoDelayL_rate".
juce::String MultiwayAudioProcessor::lfoParamID (int lfo, LfoParam param)
{
    static constexpr const char* targets[] = { "lfoDelayL", "lfoDelayR", "lfoFeedback", "lfoMix", "lfoLowCut", "lfoHighCut" };
    static constexpr const char* suffixes[] = { "rate", "sync", "div", "offset", "jitter", "smooth", "amount" };
    return juce::String (targets[lfo]) + "_" + suffixes[param];
}

// Effective value = base + modulation x target range, clipped to the limits. For the cuts the range is in
// octaves (multiplicative in Hz), so the same modulation sounds the same at low and high cutoffs.
float MultiwayAudioProcessor::applyModulation (int lfo, float baseValue, float modulation) noexcept
{
    auto octaves = [&] (float minHz, float maxHz)
    {
        const auto rangeOctaves = std::log2 (maxHz / minHz);
        return juce::jlimit (minHz, maxHz, baseValue * std::exp2 (modulation * rangeOctaves));
    };

    auto linear = [&] (float maxValue)
    {
        return juce::jlimit (0.0f, maxValue, baseValue + modulation * maxValue);
    };

    switch (lfo)
    {
        case lfoLowCut:   return octaves (minLowCutHz, maxLowCutHz);
        case lfoHighCut:  return octaves (minHighCutHz, maxHighCutHz);
        case lfoMix:      return linear (100.0f);
        case lfoFeedback: return linear (maxFeedback * 100.0f);
        default:          return linear (maxDelayMs);
    }
}

// Per band delayL / delayR (ms) and feedback (%); global lowCut, highCut (Hz, logarithmic) and mix (%).
juce::AudioProcessorValueTreeState::ParameterLayout MultiwayAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    static constexpr const char* bandNames[numBands] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

    for (int band = 0; band < numBands; ++band)
    {
        const juce::String prefix = juce::String (bandNames[band]) + " Hz ";

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandParamID (band, delayLParam), 1 }, prefix + "Delay L",
            juce::NormalisableRange<float> (0.0f, maxDelayMs, 1.0f), 150.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandParamID (band, delayRParam), 1 }, prefix + "Delay R",
            juce::NormalisableRange<float> (0.0f, maxDelayMs, 1.0f), 150.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandParamID (band, feedbackParam), 1 }, prefix + "Feedback",
            juce::NormalisableRange<float> (0.0f, maxFeedback * 100.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));
    }

    // Logarithmic range: slider position t -> start * (end / start)^t.
    auto logRange = [] (float start, float end)
    {
        return juce::NormalisableRange<float> (start, end,
            [] (float s, float e, float t)     { return s * std::pow (e / s, t); },
            [] (float s, float e, float value) { return std::log (value / s) / std::log (e / s); });
    };

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "lowCut", 1 }, "Low Cut", logRange (minLowCutHz, maxLowCutHz), minLowCutHz,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "highCut", 1 }, "High Cut", logRange (minHighCutHz, maxHighCutHz), maxHighCutHz,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    // Number of active bands; choices in bandCountChoices order, default 10.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "bands", 1 }, "Bands", juce::StringArray { "10", "4", "2" }, 0));

    // STFT size; choices in fftSizeChoices order, default 2048. Latency depends on the choice.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "fftSize", 3 }, "FFT Size", juce::StringArray { "512", "1024", "2048", "4096" }, defaultFftSizeChoice));

    // Tempo sync: a per-channel ms / Rate switch and a per-band note division (default 1/8).
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { syncParamID (delayLParam), 2 }, "Sync L", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { syncParamID (delayRParam), 2 }, "Sync R", false));

    const juce::StringArray divisionChoices (divisionNames.data(), numDivisions);

    for (int band = 0; band < numBands; ++band)
    {
        const juce::String prefix = juce::String (bandNames[band]) + " Hz ";

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { bandDivisionID (band, delayLParam), 2 }, prefix + "Rate L", divisionChoices, defaultDivision));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { bandDivisionID (band, delayRParam), 2 }, prefix + "Rate R", divisionChoices, defaultDivision));
    }

    // Shaper / LFO: for every modulatable knob, Rate (Hz or note division), Offset, Jitter,
    // Smooth and Amount. Amount 0 = off.
    static constexpr const char* lfoNames[numLfos] = { "L Delay", "R Delay", "Feedback", "Mix", "Low Cut", "High Cut" };
    const juce::StringArray lfoDivisionChoices (lfoDivisionNames.data(), numLfoDivisions);
    const auto percent = juce::AudioParameterFloatAttributes().withLabel ("%");

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        const juce::String prefix = juce::String ("LFO ") + lfoNames[lfo] + " ";
        auto id = [lfo] (LfoParam param) { return juce::ParameterID { lfoParamID (lfo, param), 4 }; };

        layout.add (std::make_unique<juce::AudioParameterFloat> (id (lfoRateParam), prefix + "Rate",
            logRange (minLfoRateHz, maxLfoRateHz), defaultLfoRateHz, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        layout.add (std::make_unique<juce::AudioParameterBool> (id (lfoSyncParam), prefix + "Sync", false));
        layout.add (std::make_unique<juce::AudioParameterChoice> (id (lfoDivisionParam), prefix + "Division",
            lfoDivisionChoices, defaultLfoDivision));
        layout.add (std::make_unique<juce::AudioParameterFloat> (id (lfoOffsetParam), prefix + "Offset",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
        layout.add (std::make_unique<juce::AudioParameterFloat> (id (lfoJitterParam), prefix + "Jitter",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
        layout.add (std::make_unique<juce::AudioParameterFloat> (id (lfoSmoothParam), prefix + "Smooth",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
        layout.add (std::make_unique<juce::AudioParameterFloat> (id (lfoAmountParam), prefix + "Amount",
            juce::NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, percent));
    }

    return layout;
}

MultiwayAudioProcessor::~MultiwayAudioProcessor()
{
    stopTimer();
}

//==============================================================================
const juce::String MultiwayAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool MultiwayAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool MultiwayAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool MultiwayAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double MultiwayAudioProcessor::getTailLengthSeconds() const
{
    // The longest delay repeats until the highest feedback decays to -60 dB (with the current parameters).
    // If an LFO is on, the largest value the modulation can reach (|amount|) is used, not the current value.
    float longestDelayMs = 0.0f, strongestFeedback = 0.0f;

    auto reachable = [this] (int band, BandParam param)
    {
        const auto lfo = getSourceLfo (lfoForBandParam (param));
        return applyModulation (lfo, getBaseValue (bandParamIndex (band, param)), std::abs (getLfoAmount (lfo)));
    };

    for (int band = 0; band < getActiveBandCount(); ++band)
    {
        longestDelayMs    = juce::jmax (longestDelayMs, reachable (band, delayLParam), reachable (band, delayRParam));
        strongestFeedback = juce::jmax (strongestFeedback, reachable (band, feedbackParam) * 0.01f);
    }

    strongestFeedback = juce::jlimit (0.0f, maxFeedback, strongestFeedback);
    const double repeats = strongestFeedback > 0.0f ? std::ceil (std::log (0.001) / std::log ((double) strongestFeedback)) : 0.0;

    return (double) juce::jlimit (0.0f, maxDelayMs, longestDelayMs) / 1000.0 * (1.0 + repeats);
}

int MultiwayAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int MultiwayAudioProcessor::getCurrentProgram()
{
    return 0;
}

void MultiwayAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String MultiwayAudioProcessor::getProgramName (int index)
{
    return {};
}

void MultiwayAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void MultiwayAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate = sampleRate;

    // All buffers are allocated once for the largest FFT size; a size change (applyFftSize) on the
    // audio thread only clears and refills them, without allocating.
    fftData.assign ((size_t) (2 * maxFftSize), 0.0f);

    // The history row stride is the active numBins. The required frame count is largest at the smallest hop (128);
    // the largest historyRows * numBins over all sizes is allocated, so every size fits.
    historyCapacity = 0;
    for (auto size : fftSizeChoices)
    {
        const auto rows = (size_t) std::round ((double) maxDelayMs / 1000.0 * sampleRate / (double) (size / 4)) + 1;
        historyCapacity = juce::jmax (historyCapacity, rows * (size_t) (size / 2 + 1));
    }

    binBand.assign ((size_t) maxNumBins, 0);
    binBandFrac.assign ((size_t) maxNumBins, 0.0f);
    binFeedback.assign ((size_t) maxNumBins, 0.0f);
    soloTarget.assign ((size_t) maxNumBins, 1.0f);
    soloWeight.assign ((size_t) maxNumBins, 1.0f);

    channels.resize ((size_t) juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels()));
    for (auto& state : channels)
    {
        state.inputFifo.assign ((size_t) maxFftSize, 0.0f);
        state.outputAccum.assign ((size_t) maxFftSize, 0.0f);
        state.history.assign (historyCapacity, {});
        state.delayFrames.assign ((size_t) maxNumBins, 0);
        state.phaseShift.assign ((size_t) maxNumBins, { 1.0f, 0.0f });
        state.fractionalGain.assign ((size_t) maxNumBins, 1.0f);
    }

    frameActiveBands = getActiveBandCount();
    frameSoloMask = soloMask.load() & ((1u << frameActiveBands) - 1u);
    lastLowCutHz  = lowCutParam->load();
    lastHighCutHz = highCutParam->load();

    smoothedMix.reset (sampleRate, 0.05);
    smoothedMix.setCurrentAndTargetValue (frameSoloMask != 0 ? 1.0f : mixParam->load() * 0.01f);

    // LFOs from the start; the jitter sequence differs per LFO but is the same every time.
    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        lfos[(size_t) lfo].reset (lfo + 1);
        lfoModulation[(size_t) lfo].store (0.0f);
    }

    mixModulation = mixModulationStep = 0.0f;

    // Sets up the active size: N, H, bin tables, frame-rate dependent smoothers, zeroed buffers.
    applyFftSize (getFftSizeChoice());

    // prepareToPlay is called by the host (while no audio is processed); the latency can be reported directly here.
    setLatencySamples (latencyForFftSize (fftSize));
}

int MultiwayAudioProcessor::getFftSizeChoice() const
{
    return juce::jlimit (0, numFftSizes - 1, juce::roundToInt (fftSizeParam->load()));
}

// Changes the active FFT size and resets the STFT state. Called both from prepareToPlay and from the audio
// thread (processBlock); only uses buffers allocated in prepareToPlay.
// Doesn't report the latency itself: setLatencySamples isn't safe on the audio thread (see timerCallback).
void MultiwayAudioProcessor::applyFftSize (int choiceIndex)
{
    fftSizeChoice = choiceIndex;
    fftSize = fftSizeForChoice (choiceIndex);
    hopSize = fftSize / 4;
    numBins = fftSize / 2 + 1;
    latencyFftSize.store (fftSize);

    // History sized for the longest delay; capacity was allocated in prepareToPlay to fit every size.
    // Rounded to the nearest frame like computeBinTables: the frame count of every delay up to maxDelayMs fits.
    maxDelayFrames = (int) std::round ((double) maxDelayMs / 1000.0 * currentSampleRate / (double) hopSize);
    historyRows = maxDelayFrames + 1;
    jassert ((size_t) (historyRows * numBins) <= historyCapacity);

    // Band -> bin mapping and solo targets for the new bin count.
    computeBinBands (frameActiveBands);
    computeSoloTargets (frameSoloMask);
    std::copy (soloTarget.begin(), soloTarget.begin() + numBins, soloWeight.begin());

    // Frame rate (sampleRate / hopSize) changed: solo ~30 ms, band parameters 100 ms smoothing.
    soloSmoothing = 1.0f - (float) std::exp (-(double) hopSize / (0.03 * currentSampleRate));

    for (int i = 0; i < numBandParams; ++i)
    {
        smoothedBandParams[(size_t) i].reset (currentSampleRate / (double) hopSize, 0.1);
        smoothedBandParams[(size_t) i].setCurrentAndTargetValue (getBaseValue (i));
        frameBandValues[(size_t) i] = getEffectiveValue (i);
    }

    hopCounter = 0;

    for (auto& state : channels)
    {
        std::fill (state.inputFifo.begin(), state.inputFifo.end(), 0.0f);
        std::fill (state.outputAccum.begin(), state.outputAccum.end(), 0.0f);
        std::fill (state.history.begin(), state.history.end(), std::complex<float> {});
        state.position = 0;
        state.frameIndex = 0;
    }

    // Per-bin delay is converted again from seconds to frames for the new hop (d = tau * Fs / H) and a fractional phase.
    computeBinTables();
}

// Message thread: reports to the host if the latency of the active N differs from the reported one.
// setLatencySamples iterates the listeners under a lock and posts the host notification to the message queue;
// that's why it is called from here and not from the audio thread. Does nothing if the value is unchanged.
void MultiwayAudioProcessor::timerCallback()
{
    setLatencySamples (latencyForFftSize (latencyFftSize.load()));
}

// Writes one sample into the FIFO, reads the ready wet sample and mixes it with the dry signal.
// The dry signal is the FIFO value before it is overwritten: delayed by fftSize samples, aligned with the wet.
float MultiwayAudioProcessor::processSample (ChannelState& state, float input, float mix)
{
    const auto pos = (size_t) state.position;

    const float dry = state.inputFifo[pos];
    state.inputFifo[pos] = input;

    const float wet = state.outputAccum[pos];
    state.outputAccum[pos] = 0.0f;   // this slot now accumulates for fftSize samples later

    state.position = (state.position + 1) % fftSize;

    return wet * mix + dry * (1.0f - mix);
}

// Windows the last fftSize samples, does forward FFT -> (no processing) -> inverse FFT.
void MultiwayAudioProcessor::processFrame (ChannelState& state)
{
    const auto& window = windows[(size_t) fftSizeChoice];
    auto& fft = *ffts[(size_t) fftSizeChoice];

    // position currently points at the oldest sample in the FIFO.
    for (int n = 0; n < fftSize; ++n)
        fftData[(size_t) n] = state.inputFifo[(size_t) ((state.position + n) % fftSize)] * window[(size_t) n];

    std::fill (fftData.begin() + fftSize, fftData.begin() + 2 * fftSize, 0.0f);

    fft.performRealOnlyForwardTransform (fftData.data(), true);

    applySpectralDelay (state);

    // JUCE applies the 1/N scaling itself in the inverse transform (in all of the vDSP, fallback, FFTW, MKL and IPP
    // engines), so the forward+inverse chain has unity gain; no extra 1/N is needed.
    fft.performRealOnlyInverseTransform (fftData.data());

    overlapAdd (state);
}

// Reads each bin k from delayFrames[k] frames ago into fftData; writes input + feedback[k] * delayed value
// into the history. fftData layout is interleaved: bin k -> [2k] = re, [2k + 1] = im.
void MultiwayAudioProcessor::applySpectralDelay (ChannelState& state)
{
    const auto frame = state.frameIndex++;
    auto* writeRow = state.history.data() + (size_t) (frame % historyRows) * numBins;

    for (int k = 0; k < numBins; ++k)
    {
        const std::complex<float> input { fftData[(size_t) (2 * k)], fftData[(size_t) (2 * k + 1)] };
        const int frames = state.delayFrames[(size_t) k];

        // With frames >= 1 the source row differs from the write row (frames <= historyRows - 1),
        // and the read happens before the write. With frames == 0 the delayed value is the input itself.
        std::complex<float> delayed = input;
        if (frames > 0)
        {
            const auto sourceFrame = frame - frames;
            delayed = sourceFrame >= 0 ? state.history[(size_t) (sourceFrame % historyRows) * numBins + (size_t) k]
                                       : std::complex<float> {};
        }

        // Shift Theorem: the fractional delay is applied as a phase rotation in the frequency domain.
        delayed *= state.phaseShift[(size_t) k];

        // Feedback feeds back the phase-corrected value, so each repeat shifts by the full delay.
        // In a bin with no whole-frame delay, feedback would be a zero-delay loop, so it isn't applied.
        writeRow[k] = frames > 0 ? input + binFeedback[(size_t) k] * delayed : input;

        // Solo and fractional-shift compensation are applied only to the output; the history is untouched. If the
        // compensation entered the feedback loop, the loop gain could be maxFeedback * 1 / cos(pi / 8) > 1.
        const auto output = delayed * (soloWeight[(size_t) k] * state.fractionalGain[(size_t) k]);

        fftData[(size_t) (2 * k)]     = output.real();
        fftData[(size_t) (2 * k + 1)] = output.imag();
    }
}

// The value of a band parameter used by the DSP: base value + the target's LFO (applyModulation).
// The DSP does the same calculation in updateFrameParameters with the smoothed base value.
float MultiwayAudioProcessor::getEffectiveValue (int paramIndex) const
{
    const auto lfo = getSourceLfo (lfoForBandParam ((BandParam) (paramIndex % paramsPerBand)));
    return applyModulation (lfo, getBaseValue (paramIndex), getLfoModulation (lfo));
}

// Unmodulated value of a band parameter. On a channel with sync on, the delay is converted to ms from the band's
// note division and the host BPM (clamped at maxDelayMs); the rest of the DSP only sees ms.
float MultiwayAudioProcessor::getBaseValue (int paramIndex) const
{
    const int band = paramIndex / paramsPerBand;
    const auto param = (BandParam) (paramIndex % paramsPerBand);

    if (param != feedbackParam)
    {
        const int sideIndex = param == delayRParam ? 1 : 0;

        if (syncParams[(size_t) sideIndex]->load() >= 0.5f)
        {
            const auto division = juce::roundToInt (divisionParams[(size_t) (band * 2 + sideIndex)]->load());
            return juce::jmin (maxDelayMs, divisionToMs (division, hostBpm.load()));
        }
    }

    return bandParams[(size_t) paramIndex]->load();
}

// At the start of a frame, advances each LFO by one hop and updates the modulation (output x amount).
// In Rate mode the phase is locked to the DAW position while playing; with no position or when stopped it runs free.
// samplePosition: the frame's sample position within the block (to carry the ppq to the frame's instant).
void MultiwayAudioProcessor::advanceLfos (int samplePosition)
{
    const double frameSeconds = (double) hopSize / currentSampleRate;
    const double bpm = hostBpm.load();

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        auto& state = lfos[(size_t) lfo];
        double periodSeconds;

        if (getLfoParam (lfo, lfoSyncParam) >= 0.5f)
        {
            const auto division = (size_t) juce::jlimit (0, numLfoDivisions - 1, juce::roundToInt (getLfoParam (lfo, lfoDivisionParam)));
            const double cycleBeats = lfoDivisionWholeNotes[division] * 4.0;
            periodSeconds = cycleBeats * 60.0 / bpm;

            if (blockIsPlaying && blockHasPpq)
                state.setPosition ((blockPpq + (double) samplePosition / currentSampleRate * bpm / 60.0) / cycleBeats);
            else
                state.advance (frameSeconds / periodSeconds);
        }
        else
        {
            const double rateHz = juce::jlimit (minLfoRateHz, maxLfoRateHz, getLfoParam (lfo, lfoRateParam));
            periodSeconds = 1.0 / rateHz;
            state.advance (frameSeconds * rateHz);
        }

        // Smooth: one-pole, time constant a quarter period at 100 %; smooths the steps in jitter and drawn shapes.
        const double smooth = getLfoParam (lfo, lfoSmoothParam) * 0.01;
        const float coefficient = smooth > 0.0 ? (float) (1.0 - std::exp (-frameSeconds / (smooth * 0.25 * periodSeconds))) : 1.0f;
        const float offset = getLfoParam (lfo, lfoOffsetParam) * 0.01f;

        const float value = state.process ([this, lfo] (double phase) { return evaluateLfoShape (lfo, phase); },
                                           offset, getLfoParam (lfo, lfoJitterParam) * 0.01f, coefficient);

        lfoModulation[(size_t) lfo].store (value * getLfoAmount (lfo), std::memory_order_relaxed);
        lfoPhase[(size_t) lfo].store (state.getPhase (offset), std::memory_order_relaxed);
    }
}

// Amount (-1..1). Rounding to the parameter's 0.1 step can give ~1e-6 instead of 0 due to float error;
// a value smaller than half a step counts as exactly 0, so Amount 0 reliably turns modulation off.
float MultiwayAudioProcessor::getLfoAmount (int lfo) const noexcept
{
    const auto percent = getLfoParam (lfo, lfoAmountParam);
    return std::abs (percent) < 0.05f ? 0.0f : percent * 0.01f;
}

// LFO shape: the drawn table (linear interpolation) if the pencil is on, otherwise a sine. Readable from any thread.
float MultiwayAudioProcessor::evaluateLfoShape (int lfo, double phase) const noexcept
{
    const auto& shape = lfoShapes[(size_t) lfo];

    if (! shape.useDrawn.load (std::memory_order_relaxed))
        return MultiwayLfo::sine (phase);

    return MultiwayLfo::readTable ([&shape] (int i) { return shape.points[(size_t) i].load (std::memory_order_relaxed); }, phase);
}

MultiwayLfo::Table MultiwayAudioProcessor::getLfoDrawnShape (int lfo) const
{
    MultiwayLfo::Table table {};
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        table[(size_t) i] = lfoShapes[(size_t) lfo].points[(size_t) i].load();
    return table;
}

// Hands the drawn shape to the audio thread (atomic per point); storeInState also writes it to state.
// While drawing only the atomics are updated; the state is updated when the mouse is released.
void MultiwayAudioProcessor::setLfoDrawnShape (int lfo, const MultiwayLfo::Table& points, bool storeInState)
{
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        lfoShapes[(size_t) lfo].points[(size_t) i].store (juce::jlimit (-1.0f, 1.0f, points[(size_t) i]));

    if (storeInState)
        storeLfoShape (lfo);
}

void MultiwayAudioProcessor::setUsesDrawnShape (int lfo, bool shouldUseDrawn)
{
    lfoShapes[(size_t) lfo].useDrawn.store (shouldUseDrawn);
    storeLfoShape (lfo);
}

namespace
{
    const juce::Identifier lfoShapesType  { "LFO_SHAPES" };
    const juce::Identifier lfoShapeType   { "SHAPE" };
    const juce::Identifier lfoIndexProperty  { "lfo" };
    const juce::Identifier lfoDrawnProperty  { "drawn" };
    const juce::Identifier lfoPointsProperty { "points" };
    const juce::Identifier delayLinkProperty { "delayLink" };
}

void MultiwayAudioProcessor::setDelayLinked (bool shouldBeLinked)
{
    delayLinked.store (shouldBeLinked);
    apvts.state.setProperty (delayLinkProperty, shouldBeLinked, nullptr);
}

// Writes the LFO's shape into the LFO_SHAPES child of the APVTS state: pencil state and 128 points.
void MultiwayAudioProcessor::storeLfoShape (int lfo)
{
    auto shapes = apvts.state.getOrCreateChildWithName (lfoShapesType, nullptr);
    auto node = shapes.getChildWithProperty (lfoIndexProperty, lfo);

    if (! node.isValid())
    {
        node = juce::ValueTree (lfoShapeType);
        node.setProperty (lfoIndexProperty, lfo, nullptr);
        shapes.appendChild (node, nullptr);
    }

    juce::StringArray points;
    for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
        points.add (juce::String (lfoShapes[(size_t) lfo].points[(size_t) i].load(), 4));

    node.setProperty (lfoDrawnProperty, lfoShapes[(size_t) lfo].useDrawn.load(), nullptr);
    node.setProperty (lfoPointsProperty, points.joinIntoString (" "), nullptr);
}

// Reads the shapes from the child tree after state is loaded; an LFO with no entry reverts to a sine with the pencil off.
void MultiwayAudioProcessor::loadLfoShapes()
{
    const auto shapes = apvts.state.getChildWithName (lfoShapesType);
    const auto sine = MultiwayLfo::sineTable();

    for (int lfo = 0; lfo < numLfos; ++lfo)
    {
        const auto node = shapes.getChildWithProperty (lfoIndexProperty, lfo);
        auto points = sine;

        juce::StringArray tokens;
        tokens.addTokens (node.getProperty (lfoPointsProperty).toString(), " ", {});
        if (tokens.size() == MultiwayLfo::shapeSize)
            for (int i = 0; i < MultiwayLfo::shapeSize; ++i)
                points[(size_t) i] = tokens[i].getFloatValue();

        setLfoDrawnShape (lfo, points, false);
        lfoShapes[(size_t) lfo].useDrawn.store ((bool) node.getProperty (lfoDrawnProperty, false));
    }

    ++lfoShapeVersion;
}

// At the start of a frame, smooths the band parameters and reads the cuts, band count and solo.
// Doesn't recompute the bin tables if the values haven't changed.
void MultiwayAudioProcessor::updateFrameParameters()
{
    bool changed = false;

    const int activeBands = getActiveBandCount();
    const bool bandCountChanged = activeBands != frameActiveBands;

    if (bandCountChanged)
    {
        frameActiveBands = activeBands;
        computeBinBands (activeBands);
        changed = true;
    }

    const auto mask = soloMask.load() & ((1u << activeBands) - 1u);   // solo on inactive bands is ignored
    if (bandCountChanged || mask != frameSoloMask)
    {
        frameSoloMask = mask;
        computeSoloTargets (mask);
    }

    smoothSoloWeights();

    // The base value is smoothed and the LFO added afterwards: so the smoother (100 ms) doesn't filter the LFO too.
    for (int i = 0; i < numBandParams; ++i)
    {
        auto& smoothed = smoothedBandParams[(size_t) i];
        smoothed.setTargetValue (getBaseValue (i));

        const auto lfo = getSourceLfo (lfoForBandParam ((BandParam) (i % paramsPerBand)));
        const float value = applyModulation (lfo, smoothed.getNextValue(), getLfoModulation (lfo));

        if (value != frameBandValues[(size_t) i])
        {
            frameBandValues[(size_t) i] = value;
            changed = true;
        }
    }

    const float lowCutHz  = applyModulation (lfoLowCut,  lowCutParam->load(),  getLfoModulation (lfoLowCut));
    const float highCutHz = applyModulation (lfoHighCut, highCutParam->load(), getLfoModulation (lfoHighCut));

    if (lowCutHz != lastLowCutHz || highCutHz != lastHighCutHz)
    {
        lastLowCutHz  = lowCutHz;
        lastHighCutHz = highCutHz;
        changed = true;
    }

    if (changed)
        computeBinTables();
}

// Bin -> band mapping: the bin's log-frequency position p = log2(f / 31.25 Hz) / (9 / (N - 1)), clamped to
// [0, N - 1]. binBand is the band below p, binBandFrac the fraction towards the band above; no allocations.
void MultiwayAudioProcessor::computeBinBands (int activeBands)
{
    const double octavesPerBand = (double) bandSpanOctaves / (double) (activeBands - 1);

    binBand[0] = 0;        // DC: log undefined, band 0 value
    binBandFrac[0] = 0.0f;

    for (size_t k = 1; k < (size_t) numBins; ++k)
    {
        const double binFrequency = (double) k * currentSampleRate / (double) fftSize;
        const double position = juce::jlimit (0.0, (double) (activeBands - 1),
                                              std::log2 (binFrequency / (double) lowestCentreHz) / octavesPerBand);
        binBand[k]     = juce::jmin ((int) std::floor (position), activeBands - 2);
        binBandFrac[k] = (float) (position - binBand[k]);
    }
}

// Solo target: 1 at a soloed band's centre, 0 at the other centres; in between the same log-frequency
// interpolation as the band values. With no solo (mask == 0) all bins are 1.
void MultiwayAudioProcessor::computeSoloTargets (juce::uint32 mask)
{
    auto isSoloed = [mask] (int band) { return (mask >> band) & 1u ? 1.0f : 0.0f; };

    for (size_t k = 0; k < (size_t) numBins; ++k)
    {
        const float t = binBandFrac[k];
        soloTarget[k] = mask == 0 ? 1.0f
                                  : isSoloed (binBand[k]) * (1.0f - t) + isSoloed (binBand[k] + 1) * t;
    }
}

// Moves the solo weight towards its target with a per-frame one-pole (~30 ms); no clicks when solo changes.
void MultiwayAudioProcessor::smoothSoloWeights()
{
    for (size_t k = 0; k < (size_t) numBins; ++k)
        soloWeight[k] += (soloTarget[k] - soloWeight[k]) * soloSmoothing;
}

// Band value for bin k: linear interpolation in log frequency between the two neighbouring band centres.
float MultiwayAudioProcessor::interpolateBands (int bin, BandParam param) const
{
    const int band = binBand[(size_t) bin];
    const float t  = binBandFrac[(size_t) bin];

    return frameBandValues[(size_t) bandParamIndex (band, param)] * (1.0f - t)
         + frameBandValues[(size_t) bandParamIndex (band + 1, param)] * t;
}

// Computes the feedback and the per-channel delay (with the cut weight) for each bin; splits the delay
// into a whole-frame part and a fractional phase part, and computes the gain compensation for the fractional part.
// Channel 0 uses the delayL bands, channel 1 the delayR bands.
void MultiwayAudioProcessor::computeBinTables()
{
    for (int k = 0; k < numBins; ++k)
    {
        // Bin frequency: f_k = k * sampleRate / fftSize.
        const double binFrequency = (double) k * currentSampleRate / (double) fftSize;
        const float cutWeight = lowCutWeight ((float) binFrequency, lastLowCutHz)
                              * highCutWeight ((float) binFrequency, lastHighCutHz);

        // No repeats in the cut region either: feedback fades with the same weight.
        binFeedback[(size_t) k] = juce::jlimit (0.0f, maxFeedback, interpolateBands (k, feedbackParam) * 0.01f * cutWeight);

        for (size_t channel = 0; channel < channels.size(); ++channel)
        {
            auto& state = channels[channel];
            const auto side = channel == 0 ? delayLParam : delayRParam;

            const float delayMs = juce::jlimit (0.0f, maxDelayMs, interpolateBands (k, side));
            const double delaySeconds = (double) delayMs * 0.001 * (double) cutWeight;

            // Rounded to the nearest frame; the remaining fraction is -H/2..+H/2. Since the phase shift is circular
            // within a frame, gain loss and wrap-around grow with |fraction|; with floor the fraction went up to H.
            // A negative fraction (shifting forward via phase) only happens with frames >= 1: a past frame's
            // spectrum is read early, and the total delay is still frames * H + fraction >= H/2 > 0.
            const double framesExact = delaySeconds * currentSampleRate / (double) hopSize;
            state.delayFrames[(size_t) k] = juce::jmin ((int) std::round (framesExact), maxDelayFrames);

            // Signed fraction r_k (samples) left over from rounding, -H/2..+H/2.
            const double residualSamples = (framesExact - state.delayFrames[(size_t) k]) * (double) hopSize;
            jassert (std::abs (residualSamples) <= 0.5 * hopSize + 1.0e-6);

            const double residualSeconds = residualSamples / currentSampleRate;
            state.phaseShift[(size_t) k] = (std::complex<float>) std::polar (1.0, -juce::MathConstants<double>::twoPi * binFrequency * residualSeconds);

            // Within a frame, the overlap-add sum of the r_k-shifted analysis window and the synthesis window
            // gives a gain of cos(pi * r_k / N); it is compensated at the output with 1 / cos (|r_k| <= H/2 -> at most 1 / cos(pi / 8)).
            state.fractionalGain[(size_t) k] = (float) (1.0 / std::cos (juce::MathConstants<double>::pi * residualSamples / (double) fftSize));
        }
    }
}

// lowCut weight (in octaves): 0 at 1/3 octave below the cutoff, 1 at and above the cutoff, linear in between.
// If lowCut is at its minimum (20 Hz) the filter is off and all bins get the full delay.
float MultiwayAudioProcessor::lowCutWeight (float binFrequency, float lowCutHz)
{
    if (lowCutHz <= minLowCutHz)
        return 1.0f;

    if (binFrequency <= 0.0f)   // DC: log undefined
        return 0.0f;

    const float octavesFromCut = std::log2 (binFrequency / lowCutHz);
    return juce::jlimit (0.0f, 1.0f, (octavesFromCut + lowCutRampOctaves) / lowCutRampOctaves);
}

// highCut weight, the mirror of lowCut: 1 at and below the cutoff, 0 at 1/3 octave above it, linear in between.
// If highCut is at its maximum (20 kHz) the filter is off and all bins get the full delay.
float MultiwayAudioProcessor::highCutWeight (float binFrequency, float highCutHz)
{
    if (highCutHz >= maxHighCutHz || binFrequency <= 0.0f)
        return 1.0f;

    const float octavesFromCut = std::log2 (binFrequency / highCutHz);
    return juce::jlimit (0.0f, 1.0f, (highCutRampOctaves - octavesFromCut) / highCutRampOctaves);
}

// Adds the inverse FFT output to the output buffer with the synthesis window and COLA gain.
void MultiwayAudioProcessor::overlapAdd (ChannelState& state)
{
    const auto& window = windows[(size_t) fftSizeChoice];
    const float colaGain = colaGains[(size_t) fftSizeChoice];

    // Sample n of the frame lands in slot position + n (i.e. n+1 samples later);
    // so the total delay is exactly fftSize.
    for (int n = 0; n < fftSize; ++n)
        state.outputAccum[(size_t) ((state.position + n) % fftSize)] += fftData[(size_t) n] * window[(size_t) n] * colaGain;
}

void MultiwayAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool MultiwayAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void MultiwayAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    juce::ignoreUnused (midiMessages);

    // Host tempo (for channels with sync on and LFOs in Rate mode); 120 if the host doesn't provide one.
    // Changes go through the band parameter smoothing. Position and play state are for the LFO lock.
    double bpm = defaultBpm;
    blockHasPpq = blockIsPlaying = false;

    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto hostTempo = position->getBpm(); hostTempo.hasValue() && *hostTempo > 0.0)
                bpm = *hostTempo;

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue())
            {
                blockPpq = *ppq;
                blockHasPpq = true;
            }

            blockIsPlaying = position->getIsPlaying();
        }
    }

    hostBpm.store (bpm);

    // If the FFT size changed, the STFT state is reset for the new N / H (without allocating). The new latency
    // is reported to the host from timerCallback; until then the reported value is that of the old size.
    if (const int choice = getFftSizeChoice(); choice != fftSizeChoice)
        applyFftSize (choice);

    const int numChannels = juce::jmin (totalNumInputChannels, (int) channels.size());
    auto* const* channelData = buffer.getArrayOfWritePointers();

    // Unprocessed input for the UI spectrum; lock-free, no allocations, drops samples when full.
    spectrumTap.push (buffer, numChannels);

    // While solo is on only the wet signal plays, regardless of mix; the dry signal fades out over 50 ms.
    const bool soloActive = (soloMask.load() & ((1u << getActiveBandCount()) - 1u)) != 0;
    smoothedMix.setTargetValue (soloActive ? 1.0f : mixParam->load() * 0.01f);

    // Sample outer, channel inner: the smoothers advance once per sample (mix) and per frame
    // (band parameters), independent of the channel count.
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        // The Mix LFO ramps linearly between frames (no steps at the hop); ignored while soloing.
        mixModulation += mixModulationStep;
        const float smoothed = smoothedMix.getNextValue();
        const float mix = soloActive ? smoothed : applyModulation (lfoMix, smoothed * 100.0f, mixModulation) * 0.01f;

        for (int channel = 0; channel < numChannels; ++channel)
            channelData[channel][sample] = processSample (channels[(size_t) channel], channelData[channel][sample], mix);

        if (++hopCounter == hopSize)
        {
            hopCounter = 0;
            advanceLfos (sample + 1);
            mixModulationStep = (getLfoModulation (lfoMix) - mixModulation) / (float) hopSize;
            updateFrameParameters();

            for (int channel = 0; channel < numChannels; ++channel)
                processFrame (channels[(size_t) channel]);
        }
    }
}

//==============================================================================
bool MultiwayAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* MultiwayAudioProcessor::createEditor()
{
    return new MultiwayAudioProcessorEditor (*this);
}

//==============================================================================
void MultiwayAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Writes the APVTS state as XML into the binary block.
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MultiwayAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // Reads the saved XML and restores the APVTS state.
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            loadLfoShapes();   // drawn shapes aren't parameters; they come from a state child tree
            delayLinked.store ((bool) apvts.state.getProperty (delayLinkProperty, false));
        }
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MultiwayAudioProcessor();
}
