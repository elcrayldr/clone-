# CRAY LDR — DRUM LAB V1

Windows x64 JUCE 8.0.6 VST3 instrument. This project has been rebuilt as a clean plugin using the same small host-facing JUCE/CMake structure as the working RiserLab project.

## Current implementation

- Native VST3 instrument with MIDI input and stereo output.
- 64 trigger pads mapped to MIDI notes 36–99.
- Audio import for WAV, AIFF, FLAC and Ogg Vorbis through JUCE audio formats.
- Mono downmix and transient/onset slicing, assigning up to 64 slices chronologically.
- Sample-accurate MIDI note-on playback with linear sample-rate conversion.
- WAV kit export as individual 24-bit mono files.
- Plugin state stores loaded pad samples and validates the incoming state before publishing it.
- Audio-thread sample access uses immutable retained sample objects and atomic pointers; file decoding and state mutation do not run in the audio callback.
- JUCE 8.0.6 and CMake build, targeting Windows x64 VST3.

## Important scope boundary

This initial clean rebuild is a transient slicer and 64-pad sampler. It does **not** claim AI source separation or reliable kick/snare/hat classification. A full-mix source separator, drum-family classifier, similarity-based variation grouping, waveform editor, per-pad controls and pattern sequencer remain separate development milestones.

## Build

Requirements:
- Windows 10/11 x64
- Visual Studio 2022 with Desktop development with C++
- CMake 3.22+
- Git
- Internet access during first configure to fetch JUCE 8.0.6

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

The GitHub Actions workflow builds the VST3, verifies the bundle layout and runs pluginval at strictness level 10.

## FL Studio

This is an instrument, not an insert effect. Load it from the instrument/plugin list in the Channel Rack. MIDI notes 36–99 trigger pads 1–64.
