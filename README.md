# Multiway — Spectral Delay

**Multiway** is a spectral-domain delay effect in which each frequency
component of the input can be delayed independently.

This repository contains two things:

| Folder | What it is |
| --- | --- |
| [`prototype/`](prototype/) | An offline Python reference implementation, used to verify the algorithm (STFT, per-bin delay, overlap-add). |
| [`plugin/`](plugin/) | The real-time audio plugin, written in C++ with JUCE (AU, VST3 and Standalone on macOS; VST3 and Standalone on Windows). Current version: **v1.1**. |

---

## Plugin (`plugin/`)

The plugin runs the same STFT algorithm as the prototype in real time
(N = 512 / 1024 / 2048 / 4096, default 2048, hop N/4, square-root periodic
Hann window, N samples of reported latency) and adds a band-based control
layer on top of it.

### Features

- **10, 4 or 2 bands.** Band centres are spaced evenly on a log-frequency
  axis from 31.25 Hz to 16 kHz. Each FFT bin interpolates between its two
  neighbouring band centres, so there are no hard steps at band edges.
- **Per-band L / R delay** (0–2000 ms). The left and right channels have
  separate delay values. Fractional delays are applied as a phase rotation
  (Shift Theorem), so delays are not limited to whole STFT frames.
- **Per-band feedback** (0–95 %).
- **Low cut** (20 Hz–2 kHz) and **high cut** (200 Hz–20 kHz). Each cut
  fades the delay and feedback out over 1/3 octave. The cuts are off at the
  ends of their ranges.
- **Mix** (dry/wet). The dry signal is aligned with the latency.
- **Tempo sync.** Each channel (L and R) has its own `ms | RATE` switch. In
  RATE mode, every band picks a note division: 1/64, 1/32, 1/16T, 1/16,
  1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2 or 1/1. The tempo comes from
  the host (120 BPM if the host does not provide one). Divisions longer
  than 2000 ms are clamped, and a warning sign is shown next to the value.
- **Solo** per band.
- **Resolution** (FFT size): 512, 1024, 2048 or 4096. The reported latency
  follows the selected size.
- **Shaper / LFO.** L Delay, R Delay, Feedback, Mix, Low Cut and High Cut
  each have their own LFO. Click a knob to show its LFO in the Shaper panel.
  - The shape is a sine, or a freehand shape (128 points) drawn while the
    pencil is on. The drawn shape is saved with the project.
  - Rate is in Hz (0.05–20 Hz) or a note division (1/16T–4/1). In RATE mode
    the phase is locked to the host position while playing.
  - Offset (phase), Jitter (random step per cycle), Smooth and a bipolar
    Amount (−100 to +100 %; 0 = off).
  - The delay and feedback LFOs add the same offset to every band. The cut
    LFOs modulate in octaves.
  - A thin ring around a modulated knob shows the current modulation.
- **Editor:**
  - a live input spectrum with one draggable delay bar per band;
  - an ALL (general) mode that edits every active band at once;
  - an L/R link;
  - Random, Reset and one-step undo.

### Building with JUCE

Requirements:

- macOS with Xcode
- [JUCE](https://juce.com) 9 and the Projucer

The plugin shares its look with the other BopsAudio plugins. The shared UI
files are vendored in [`plugin/Shared/`](plugin/Shared/), so the
repository builds on its own:

- `ClipotypeLookAndFeel` and `PillToggleButton`, copied from
  [Clipotype](https://github.com/mustafamazi/Clipotype) (by the same author)
- the fonts in [`plugin/Shared/Fonts/`](plugin/Shared/Fonts/), each with its
  licence file

1. Open `plugin/Multiway.jucer` in the Projucer. Make sure the global JUCE
   module path points to your JUCE `modules` folder.
2. Save the project. This generates `plugin/Builds/` and
   `plugin/JuceLibraryCode/`, which are not tracked in git.
3. Open `plugin/Builds/MacOSX/Multiway.xcodeproj` and build the
   **Multiway - All** target. Or build from the command line:

   ```bash
   cd plugin/Builds/MacOSX
   xcodebuild -project Multiway.xcodeproj -target "Multiway - All" -configuration Release build
   ```

   The AU and VST3 builds are copied to `~/Library/Audio/Plug-Ins/`. The
   Standalone app is placed in `plugin/Builds/MacOSX/build/<config>/`.

[`scripts/package_mac.sh`](scripts/package_mac.sh) builds the signed and
notarized macOS installer (`.pkg`).

#### Windows

Windows builds use [`plugin/CMakeLists.txt`](plugin/CMakeLists.txt), which
fetches JUCE by itself and mirrors the settings of `Multiway.jucer`
(including the version). Requires Visual Studio 2022 and CMake 3.22+:

```bash
cmake -S plugin -B build -A x64
cmake --build build --config Release
```

The **Windows Installer** GitHub Actions workflow builds the VST3 and
Standalone app and packages them with Inno Setup
([`installer/Multiway.iss`](installer/Multiway.iss)) into
`Multiway-v<version>-Windows-Setup.exe`.

---

## Prototype (`prototype/`)

A Python prototype of Multiway. It was used to verify the algorithm before
the real-time port to the JUCE plugin.

### How it works

1. **Normalisation.** The input is scaled to the range [-1, 1]. Stereo input
   is reduced to its left channel for the prototype.
2. **STFT analysis.** The signal is split into frames of N = 2048 samples
   with a hop size of H = N/4 = 512 (75% overlap). Each frame is multiplied
   by the square root of a periodic Hann window and transformed with a
   real FFT, giving 1025 frequency bins per frame.
3. **Per-bin delay.** The requested delay in seconds is converted to frames:
   `d = floor(tau * Fs / H)`. Every spectrum is stored in a circular buffer of
   `d_max + 1` spectra. For each bin k, the output is read from the spectrum
   `d_k` frames earlier: `Y[m, k] = X[m - d_k, k]`. Bins with no earlier frame
   yet available are left silent.
4. **Overlap-add reconstruction.** Each delayed spectrum is inverse-transformed,
   multiplied by the synthesis window and added to the output at its
   original position. The output is divided by the accumulated squared
   window (COLA normalisation) to restore the original level.

### Requirements

- Python 3.10+
- NumPy, SciPy, Matplotlib

```bash
pip install numpy scipy matplotlib
```

### Usage

Place a WAV file in `prototype/` and set its name in the `wavfile.read(...)`
line of `multiway_prototype.py`, then run:

```bash
cd prototype
python3 multiway_prototype.py
```

The script produces:

- a plot of the accumulated window sum (COLA check — flat in the interior),
- input/output comparison plots,
- `output_test.wav`, the processed signal.

The delay profile is set by the `delay_seconds` array (one value per bin).
The default applies the same delay to every bin:

```python
delay_seconds = np.full(num_bins, 0.15)
```

A frequency-dependent profile, for example
`np.linspace(0.0, 0.4, num_bins)`, delays high frequencies more than low ones.

### Limitations of the prototype

The JUCE plugin addresses the first three of these.

- **Frame-resolution delay only.** The fractional part of the delay is
  discarded by the floor operation, so 0.15 s is realised as 12 frames
  (≈139 ms at 44.1 kHz). The plugin restores the fraction with a phase
  factor (Shift Theorem).
- **Mono.** Only the left channel is processed.
- **Offline.** The whole file is processed at once.
- Per-bin delays introduce phase incoherence between neighbouring bins,
  which is audible as a metallic colouration. This is expected and is a
  subject of further study.

---

## Related

- Theoretical background: *Spectral Domain Processing of Audio Signals via
  FFT-based Multiband Architecture* (draft).
- [Clipotype](https://github.com/mustafamazi/Clipotype) — time-domain
  multiband saturation plugin by the same author.

## Author

Mustafa Mazı — Department of Physics, Yeditepe University
