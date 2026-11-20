# CRAY LDR — DRUMKIT CLONE

Windows x64 VST3 drum sampler and transient-slicing project.

## Current development status

**Milestone 0.4 — sampler workflow, adaptive transient slicing, kit export and MIDI sequencer**

Implemented:

- JUCE 8.0.6 / CMake VST3 project targeting Windows x64.
- 64 pads mapped to MIDI notes 36–99.
- Procedural drum voice fallback for empty pads.
- WAV/AIFF audio decoding through JUCE audio formats.
- Import runs on a background worker; the editor remains responsive while analysis runs.
- Adaptive transient detection combining short-window RMS energy, high-frequency change, local median thresholding and a 55 ms refractory interval.
- Up to 64 detected segments, ranked by transient strength and then arranged chronologically.
- Mono sample playback with linear interpolation and MIDI note-on triggering.
- Transactional kit replacement: the currently loaded kit remains available if a new import fails.
- DAW state serialization for imported samples, 16-step patterns and tempo; reader remains compatible with state format v1.
- Export loaded pads as mono PCM 24-bit WAV files with MIDI-note-based filenames and a KIT_MANIFEST.txt.
- 16-step sequencer for all 64 pads, tempo range 40–300 BPM, and Standard MIDI File export.
- Automated CTest target for synthetic transient-detection cases.
- Windows GitHub Actions build and test workflow.

**This is not yet the finished Drumkit Clone product.** The current detector slices transients from the supplied audio; it does not separate drums from a full instrumental. It does not yet provide neural source separation, reliable drum-family classification, similarity clustering/variation generation, a waveform editor, per-pad trim/gain controls, or a full kit browser. Do not interpret the transient slicer as AI drum extraction.

## Target product

- Import full songs and instrumentals.
- Separate the drum stem locally and offline.
- Split kick, snare, claps, hats, toms, percussion and cymbals where the source supports it.
- Group similar hits into variations and alternate takes.
- 64 pads across four banks, MIDI notes 36–99.
- Per-pad waveform editing, trim, fades, gain, audition and choke groups.
- Export individual WAV files, complete kits and MIDI patterns.
- Operate offline after the model/runtime has been installed.

## AI model and licensing gate

A commercial source-separation model cannot be selected by architecture or file format alone. The model weights and relevant training-data rights must permit commercial use and redistribution. Candidate research and the current release gate are documented in [docs/AI_MODEL_LICENSING.md](docs/AI_MODEL_LICENSING.md).

No AI separation is included or claimed in the current build.

## Build requirements

- Windows 10/11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.22 or newer
- Git
- Internet access on first configure to fetch JUCE 8.0.6

## Build

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions performs the same Windows x64 build and runs the automated CTest suite before publishing a VST3 artifact.

## Technical limits of the current slicer

- Maximum import duration: 10 minutes.
- Input formats: WAV, AIFF, FLAC and Ogg Vorbis; mono or stereo, 8–384 kHz. MP3 decoding is not enabled in the commercial target pending codec/licensing review.
- Decoded source buffer limit: 256 MB.
- Maximum retained samples per plugin instance: 256; restart the plugin to reset this safety limit.
- Maximum segment duration: 1.25 seconds.
- Maximum segments assigned per import: 64.
- Playback is mono; stereo sample preservation is not implemented.
- The detector is DSP-based and can miss quiet hits or select non-drum transients in a full mix.
- Pads are populated chronologically; there is no automatic drum-family classification yet.

## Architecture direction

1. Audio import and decoding
2. Optional licensed local AI source separation
3. Transient/onset detection and segmentation
4. Feature extraction and drum-family classification
5. Similarity clustering and variation grouping
6. Sample editing and pad management
7. MIDI sequencer and WAV/MIDI kit export

## FL Studio installation and routing

The Drumkit Clone is built as a **VST3 instrument**, not a mixer insert effect.

1. Extract the release ZIP once. The folder `CRAY LDR - DRUMKIT CLONE.vst3` must be directly inside the Windows VST3 directory:
   `C:\\Program Files\\Common Files\\VST3\\`
2. In FL Studio, open **Options → Manage plugins** and run **Find plugins**.
3. Load the plugin from the instrument/plugin list in the Channel Rack. Do not try to insert it as an effect on a Mixer track.

Stutter Lab and RiserLab are effects; those belong in Mixer effect slots. This distinction matters because the VST3 component categories are different.

## Compatibility target

FL Studio 26 and Cubase 13 on Windows x64. Compatibility is not claimed until tested in both hosts. DAW validation is deliberately deferred until the repository-side build and automated tests are clean.
