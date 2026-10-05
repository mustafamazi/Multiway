/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SpectrumTap.h"
#include "MultiwayLfo.h"

//==============================================================================
/**
*/
class MultiwayAudioProcessor  : public juce::AudioProcessor,
                                private juce::Timer
{
public:
    //==============================================================================
    MultiwayAudioProcessor();
    ~MultiwayAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    SpectrumTap spectrumTap;             // input signal -> editor spectrum (lock-free)

    //==============================================================================
    // Parameter layout; also used by the editor.
    static constexpr float maxDelayMs   = 2000.0f;   // the history is allocated for this value (1/1 at 120 BPM)
    static constexpr float maxFeedback  = 0.95f;     // upper limit of per-bin feedback
    static constexpr float minLowCutHz  = 20.0f;     // low-cut is off at this value
    static constexpr float maxLowCutHz  = 2000.0f;
    static constexpr float lowCutRampOctaves = 1.0f / 3.0f;  // the ramp starts this far below the cutoff
    static constexpr float minHighCutHz = 200.0f;
    static constexpr float maxHighCutHz = 20000.0f;  // high-cut is off at this value
    static constexpr float highCutRampOctaves = 1.0f / 3.0f; // the ramp ends this far above the cutoff

    // At most 10 bands; their parameters always exist. The Bands parameter selects how many are active
    // (starting from band0). The centres of the N active bands are evenly spaced in log frequency
    // between 31.25 Hz and 16 kHz: 9 / (N - 1) octaves. With 10 bands, 1 kHz * 2^(b - 5).
    static constexpr int numBands = 10;
    static constexpr int referenceBand = 5;          // the band centred at 1 kHz with 10 bands
    static constexpr float referenceHz = 1000.0f;
    static constexpr float lowestCentreHz  = 31.25f;
    static constexpr float bandSpanOctaves = 9.0f;   // 31.25 Hz -> 16 kHz
    static constexpr std::array<int, 3> bandCountChoices { 10, 4, 2 };   // choices of the Bands parameter

    static int bandCountForChoice (int choiceIndex);
    static float bandCentreHz (int band, int activeBands);
    int getActiveBandCount() const;

    // Solo: not a parameter but UI state (bit b = band b). Not saved with the project.
    // While solo is on, only the wet signal of the soloed bands is heard at the output.
    void setSoloMask (juce::uint32 mask) noexcept   { soloMask.store (mask); }
    juce::uint32 getSoloMask() const noexcept       { return soloMask.load(); }

    // Per-band parameters; paramIndex = band * paramsPerBand + BandParam.
    enum BandParam { delayLParam = 0, delayRParam, feedbackParam, paramsPerBand };
    static constexpr int numBandParams = numBands * paramsPerBand;
    static constexpr int bandParamIndex (int band, BandParam param) { return band * paramsPerBand + param; }
    static juce::String bandParamID (int band, BandParam param);

    // Tempo sync: while syncL / syncR is on, that channel's delay is computed from the per-band note division
    // (e.g. band3_divL): ms = division (in whole notes) * 4 * 60000 / BPM,
    // clamped at maxDelayMs. The BPM is read from the host, otherwise 120.
    static constexpr int numDivisions = 13;
    static constexpr int defaultDivision = 6;        // 1/8
    static constexpr std::array<const char*, numDivisions> divisionNames
        { "1/64", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1" };
    static constexpr std::array<double, numDivisions> divisionWholeNotes
        { 1.0 / 64, 1.0 / 32, 1.0 / 24, 1.0 / 16, 3.0 / 32, 1.0 / 12, 1.0 / 8, 3.0 / 16, 1.0 / 6, 1.0 / 4, 3.0 / 8, 1.0 / 2, 1.0 };
    static constexpr double defaultBpm = 120.0;

    static juce::String syncParamID (BandParam side);                  // "syncL", "syncR"
    static juce::String bandDivisionID (int band, BandParam side);     // "band3_divL"
    static float divisionToMs (int divisionIndex, double bpm);         // unclamped

    double getBpm() const noexcept                   { return hostBpm.load(); }
    bool isSynced (BandParam side) const noexcept    { return syncParams[side == delayRParam ? 1 : 0]->load() >= 0.5f; }

    // Delay used by the DSP (from the division if sync is on, LFO included, clamped); also shown by the UI.
    float getEffectiveDelayMs (int band, BandParam side) const   { return getEffectiveValue (bandParamIndex (band, side)); }

    //==============================================================================
    // Shaper / LFO: every modulatable knob has its own LFO. The L / R Delay and Feedback LFOs
    // add the same offset to all bands. Effective value = base + LFO x amount x range (applyModulation).
    enum LfoTarget { lfoDelayL = 0, lfoDelayR, lfoFeedback, lfoMix, lfoLowCut, lfoHighCut, numLfos };
    enum LfoParam  { lfoRateParam = 0, lfoSyncParam, lfoDivisionParam, lfoOffsetParam, lfoJitterParam,
                     lfoSmoothParam, lfoAmountParam, lfoParamsPerLfo };

    static constexpr std::array<const char*, numLfos> lfoTargetNames { "L DELAY", "R DELAY", "FEEDBACK", "MIX", "LOW CUT", "HIGH CUT" };
    static constexpr float minLfoRateHz = 0.05f, maxLfoRateHz = 20.0f, defaultLfoRateHz = 1.0f;

    // Length of one LFO cycle in Rate mode (1/16T ... 4/1).
    static constexpr int numLfoDivisions = 20;
    static constexpr int defaultLfoDivision = 7;     // 1/4
    static constexpr std::array<const char*, numLfoDivisions> lfoDivisionNames
        { "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2T",
          "1/2", "1/2D", "1/1T", "1/1", "1/1D", "2/1T", "2/1", "2/1D", "4/1T", "4/1" };
    static constexpr std::array<double, numLfoDivisions> lfoDivisionWholeNotes
        { 1.0 / 24, 1.0 / 16, 3.0 / 32, 1.0 / 12, 1.0 / 8, 3.0 / 16, 1.0 / 6, 1.0 / 4, 3.0 / 8, 1.0 / 3,
          1.0 / 2, 3.0 / 4, 2.0 / 3, 1.0, 3.0 / 2, 4.0 / 3, 2.0, 3.0, 8.0 / 3, 4.0 };

    static juce::String lfoParamID (int lfo, LfoParam param);          // "lfoDelayL_rate"
    static LfoTarget lfoForBandParam (BandParam param) noexcept        { return (LfoTarget) param; }

    // Link: while L and R Delay are linked, R Delay also uses L Delay's LFO (shape, settings, phase);
    // R's own LFO is kept and comes back when Link is turned off. Link is not a parameter but an
    // APVTS state property; setDelayLinked is for the message thread.
    void setDelayLinked (bool shouldBeLinked);
    bool isDelayLinked() const noexcept                                { return delayLinked.load(); }
    int getSourceLfo (int lfo) const noexcept    { return lfo == lfoDelayR && isDelayLinked() ? (int) lfoDelayL : lfo; }

    // Adds the modulation (LFO output x amount, -1..1) to the base value and clips it to the parameter limits.
    // Linear over the Delay, Feedback and Mix ranges; in octaves for Low / High Cut.
    static float applyModulation (int lfo, float baseValue, float modulation) noexcept;

    // The audio thread's values from the last frame (for the UI): the modulation and the phase the shape is read at.
    float getLfoModulation (int lfo) const noexcept   { return lfoModulation[(size_t) lfo].load (std::memory_order_relaxed); }
    double getLfoPhase (int lfo) const noexcept       { return lfoPhase[(size_t) lfo].load (std::memory_order_relaxed); }
    float getLfoAmount (int lfo) const noexcept;      // -1..1; 0 = off

    // Shape: a sine or a drawn 128-point table (while the pencil is on). The table is stored in a ValueTree
    // child of the APVTS state (not a parameter). The writing functions are for the message thread.
    MultiwayLfo::Table getLfoDrawnShape (int lfo) const;
    void setLfoDrawnShape (int lfo, const MultiwayLfo::Table& points, bool storeInState);
    bool usesDrawnShape (int lfo) const noexcept      { return lfoShapes[(size_t) lfo].useDrawn.load(); }
    void setUsesDrawnShape (int lfo, bool shouldUseDrawn);
    float evaluateLfoShape (int lfo, double phase) const noexcept;
    juce::uint32 getLfoShapeVersion() const noexcept  { return lfoShapeVersion.load(); }   // incremented when state is loaded

    // FFT size (fftSize parameter): N = 512 / 1024 / 2048 / 4096, the hop is H = N / 4 for every size.
    // The latency changes with the selected N.
    static constexpr std::array<int, 4> fftSizeChoices { 512, 1024, 2048, 4096 };
    static constexpr int defaultFftSizeChoice = 2;   // 2048
    static int fftSizeForChoice (int choiceIndex);

    // Latency of the STFT chain (samples). An input sample is read out after the last frame that contains
    // it has been processed, so the latency is exactly N, not N - H. The dry signal is delayed by the same amount.
    static constexpr int latencyForFftSize (int size) noexcept   { return size; }

private:
    //==============================================================================
    static constexpr int numFftSizes = (int) fftSizeChoices.size();
    static constexpr int minFftOrder = 9;                    // fftSizeChoices[i] = 1 << (minFftOrder + i)
    static constexpr int maxFftSize  = fftSizeChoices.back();
    static constexpr int maxNumBins  = maxFftSize / 2 + 1;   // 2049

    // Active STFT size; changed on the audio thread by applyFftSize. Buffers are allocated for the largest size.
    int fftSizeChoice = defaultFftSizeChoice;
    int fftSize = 2048;
    int hopSize = fftSize / 4;       // 75 % overlap
    int numBins = fftSize / 2 + 1;   // DC..Nyquist

    // Per-channel STFT state: input FIFO, overlap-add buffer and counters.
    struct ChannelState
    {
        std::vector<float> inputFifo;    // last fftSize input samples (circular, maxFftSize allocated)
        std::vector<float> outputAccum;  // overlap-add accumulation buffer (circular, maxFftSize allocated)
        int position = 0;                // shared write/read index for the FIFO and the output buffer

        std::vector<std::complex<float>> history;  // historyRows x numBins (row stride is the active numBins), circular spectrum buffer
        juce::int64 frameIndex = 0;                // number of frames processed on this channel

        std::vector<int> delayFrames;                 // whole-frame part of the per-bin delay (maxNumBins allocated)
        std::vector<std::complex<float>> phaseShift;  // per-bin fractional delay phase factor (maxNumBins allocated)
        std::vector<float> fractionalGain;            // per-bin fractional shift gain compensation (output only)
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    float getBaseValue (int paramIndex) const;
    float getLfoParam (int lfo, LfoParam param) const noexcept   { return lfoParams[(size_t) (lfo * lfoParamsPerLfo + param)]->load(); }
    void advanceLfos (int samplePosition);
    void storeLfoShape (int lfo);
    void loadLfoShapes();
    int getFftSizeChoice() const;
    void applyFftSize (int choiceIndex);
    float processSample (ChannelState& state, float input, float mix);
    void processFrame (ChannelState& state);
    void overlapAdd (ChannelState& state);
    void applySpectralDelay (ChannelState& state);
    void updateFrameParameters();
    void computeBinBands (int activeBands);
    void computeSoloTargets (juce::uint32 mask);
    void smoothSoloWeights();
    void computeBinTables();
    float getEffectiveValue (int paramIndex) const;
    float interpolateBands (int bin, BandParam param) const;
    static float lowCutWeight (float binFrequency, float lowCutHz);
    static float highCutWeight (float binFrequency, float highCutHz);

    // Per-size FFT, window and COLA gain; prepared once in the constructor.
    std::array<std::unique_ptr<juce::dsp::FFT>, numFftSizes> ffts;
    std::array<std::vector<float>, numFftSizes> windows;   // sqrt(periodic Hann), analysis + synthesis
    std::array<float, numFftSizes> colaGains {};           // 1 / (overlap sum of the squared window)
    std::vector<float> fftData;          // 2 * maxFftSize, FFT workspace (shared between channels)
    std::vector<ChannelState> channels;
    size_t historyCapacity = 0;          // per-channel history size: the largest historyRows * numBins over all sizes

    // The audio thread writes the active N; timerCallback (message thread) reports the latency to the host.
    // 0 before prepareToPlay: nothing to report.
    std::atomic<int> latencyFftSize { 0 };

    int hopCounter = 0;                  // samples received since the last frame (all channels in sync)
    double currentSampleRate = 44100.0;

    std::vector<int> binBand;            // per-bin lower neighbouring band (0..numBands-2), in prepareToPlay
    std::vector<float> binBandFrac;      // per-bin log-frequency fraction towards the upper neighbouring band (0..1)
    std::vector<float> binFeedback;      // per-bin feedback (0..maxFeedback), shared by the channels
    std::vector<float> soloTarget;       // per-bin solo weight target (0..1); always 1 with no solo
    std::vector<float> soloWeight;       // smoothed solo weight, the wet output is multiplied by it
    float soloSmoothing = 1.0f;          // per-frame one-pole coefficient (~30 ms)
    int maxDelayFrames = 0;              // number of frames corresponding to maxDelayMs at the active hop
    int historyRows = 1;                 // maxDelayFrames + 1

    std::array<std::atomic<float>*, numBandParams> bandParams {};   // delay ms, feedback %
    std::atomic<float>* lowCutParam  = nullptr;  // Hz
    std::atomic<float>* highCutParam = nullptr;  // Hz
    std::atomic<float>* mixParam     = nullptr;  // %
    std::atomic<float>* bandsParam   = nullptr;  // choice index (bandCountChoices)
    std::atomic<float>* fftSizeParam = nullptr;  // choice index (fftSizeChoices)
    std::array<std::atomic<float>*, 2> syncParams {};              // syncL, syncR (0 / 1)
    std::array<std::atomic<float>*, numBands * 2> divisionParams {};   // band * 2 + channel, division index
    std::atomic<double> hostBpm { defaultBpm };  // read from the host in processBlock
    double blockPpq = 0.0;                       // DAW position at the start of the block (quarter notes)
    bool blockHasPpq = false, blockIsPlaying = false;

    // LFO: audio-thread state, parameters, values sent to the UI and shape tables.
    struct LfoShapeTable
    {
        std::array<std::atomic<float>, MultiwayLfo::shapeSize> points;   // drawn shape, -1..1
        std::atomic<bool> useDrawn { false };                            // pencil on: drawn shape
    };

    std::array<MultiwayLfo, numLfos> lfos;
    std::array<std::atomic<float>*, numLfos * lfoParamsPerLfo> lfoParams {};
    std::array<std::atomic<float>, numLfos> lfoModulation {};   // output x amount, -1..1
    std::array<std::atomic<double>, numLfos> lfoPhase {};
    std::array<LfoShapeTable, numLfos> lfoShapes;
    std::atomic<juce::uint32> lfoShapeVersion { 0 };
    std::atomic<bool> delayLinked { false };     // written from the UI or state, read at the start of a frame
    float mixModulation = 0.0f, mixModulationStep = 0.0f;       // Mix LFO, linear per sample between frames
    std::atomic<juce::uint32> soloMask { 0 };    // written from the UI, read at the start of a frame
    std::array<juce::SmoothedValue<float>, numBandParams> smoothedBandParams;  // advances at frame rate
    juce::SmoothedValue<float> smoothedMix;      // 0..1, advances at sample rate

    // The values the bin tables were last computed with (smoothed band values and cuts).
    std::array<float, numBandParams> frameBandValues {};
    float lastLowCutHz = -1.0f, lastHighCutHz = -1.0f;
    int frameActiveBands = numBands;             // band count the binBand tables were computed for
    juce::uint32 frameSoloMask = 0;              // mask soloTarget was computed for (limited to the active bands)

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultiwayAudioProcessor)
};
