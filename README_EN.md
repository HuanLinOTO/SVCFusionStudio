<div align="center">
  <h1>SVCFusion Studio</h1>
  <img src="efc4695dc0bf6bdf922dd174b6acf3d1_720.png" alt="Logo" width="300" />
  <p>A lightweight pitch editor built with JUCE, featuring neural pitch detection and vocoder resynthesis for real-time preview.</p>
</div>

> **Note for agents/LLMs**: Agent-specific build instructions are maintained in `AGENTS.md`. See that file for guidance on local defaults, CI invariants, and CPU-only overrides.

## Overview

SVCFusion Studio is a standalone app and audio plugin for editing vocal pitch in a piano roll. It analyzes audio with neural pitch detectors, segments notes, and resynthesizes edits with a neural vocoder. The plugin supports both ARA hosts and a non-ARA capture workflow.
It can significantly improve the problem of distortion after adjustment

## Features

- Piano roll editing for pitch, timing, and note parameters
- Neural pitch detection (RMVPE default, FCPE optional)
- SOME-based note segmentation with F0 fallback
- Real-time preview with neural vocoder resynthesis
- Export audio and MIDI
- Standalone app plus VST3/AU plugins (AAX optional)
- GPU acceleration via CUDA or DirectML on Windows; CoreML path on macOS
- Multi-language UI

## Requirements

- CMake 3.22+
- C++17 compiler
- Git (for submodules)

### Platform notes

| Platform | Requirements                                       |
| -------- | -------------------------------------------------- |
| Windows  | Visual Studio 2019+ with C++ workload, Windows SDK |
| macOS    | Xcode + command line tools                         |
| Linux    | GCC/Clang, ALSA development libraries              |

## Models and Assets

The build expects required model files in `Resources/models` and will fail if any are missing. Models are downloaded from Hugging Face (`SVCFusion/things`) during CI; local builds must place them manually:

### Core models

- `pc_nsf_hifigan.onnx` (vocoder)
- `contentvec768l12.onnx` (content encoder)
- `rmvpe.onnx`
- `fcpe.onnx`
- `some.onnx`
- `cent_table.bin`
- `mel_filterbank.bin`

### HNSep (harmonic/percussive separation)

- `hnsep/config.json`
- `hnsep/hnsep_VR.onnx`
- `hnsep/hnsep_VR_convstft.onnx`
- `hnsep/split/hnsep_pre.onnx`
- `hnsep/split/hnsep_core.onnx`
- `hnsep/split/hnsep_post.onnx`

### GAME (optional, for GAME-based analysis)

- `GAME/bd2dur.onnx`
- `GAME/config.json`
- `GAME/dur2bd.onnx`
- `GAME/encoder.onnx`
- `GAME/estimator.onnx`
- `GAME/segmenter.onnx`

CI resolves model variants automatically (preferring `_fp32_slim.onnx` → `_fp32.onnx` → base name).

Other runtime assets live in `Resources/lang` and `Resources/fonts`.

## Build

### Build helper (Windows)

The easiest way to build on Windows is with the included `build.ps1` script, which defaults to a DirectML build:

```powershell
.\build.ps1                              # configure + build (DirectML, Release)
.\build.ps1 -Configuration Debug         # Debug build
.\build.ps1 -Reconfigure                 # force re-run CMake configure
.\build.ps1 -CMakeArgs @('-DUSE_DIRECTML=OFF')  # CPU-only build
```

See `.\build.ps1 -h` or read the script for all options.

### Manual build

#### Submodules

```bash
git submodule update --init --recursive
```

#### Configure

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

#### Build

```bash
cmake --build build --config Release
```

### Optional flags

- `-DUSE_CUDA=ON` Enable CUDA execution provider (Windows)
- `-DUSE_DIRECTML=ON` Enable DirectML execution provider (Windows)
- `-DUSE_ASIO=ON` Enable ASIO support (Windows)
- `-DUSE_BUNDLED_CUDA_RUNTIME=ON` Bundle CUDA redistributables (Windows)
- `-DUSE_BUNDLED_DIRECTML_RUNTIME=ON` Bundle DirectML runtime (Windows)
- `-DONNXRUNTIME_VERSION=1.17.3` Override ONNX Runtime version

Notes:

- CUDA and DirectML cannot be enabled at the same time.
- ONNX Runtime is downloaded during CMake configure.

## Plugin Formats

- VST3 and AU are built by default.
- AAX is built only when `AAX_SDK_PATH` is set and valid.
- ARA is enabled when the `third_party/ARA_SDK` submodule is present.

## Usage

1. Open an audio file (WAV/MP3/FLAC/OGG).
2. Analyze to extract pitch and notes.
3. Edit notes and pitch curves in the piano roll.
4. Preview changes in real time.
5. Export audio or MIDI.

### Plugin modes

- **ARA mode**: Direct audio access in supported hosts (e.g., Studio One, Cubase, Logic).
- **Non-ARA mode**: Auto-capture and process in hosts without ARA.

## Project Structure

```
SVCFusion Studio/
  Source/
    Audio/        # Audio engine, pitch detection, vocoder
    Models/       # Project and note data
    UI/           # UI components
    Utils/        # Utilities, localization, undo
    Plugin/       # VST3/AU/AAX/ARA integration
  Resources/
    models/       # Required ONNX + data files
    lang/         # Localization JSON
    fonts/        # UI fonts
  third_party/
    JUCE/         # JUCE submodule
    ARA_SDK/      # ARA SDK submodule (optional)
  CMakeLists.txt
```

## License

See `LICENSE`.
