# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Recommended (macOS/Homebrew) — robust to `brew upgrade`:**
```bash
scripts/build.sh                 # Release build + tests; self-heals a stale cache
BUILD_TYPE=Debug scripts/build.sh
RUN_TESTS=0 scripts/build.sh     # build only
```
`scripts/build.sh` verifies the required Homebrew formulas, auto-wipes + reconfigures when a cached dependency path has vanished after an upgrade, and configures against the stable `/opt/homebrew/opt/…` symlinks. Binaries: `build/wmr`, `build/tests/wmr_tests`. Test suite needs Catch2 (`brew install catch2`).

Or, arm64 preset: `cmake --preset mac-homebrew-Release && cmake --build --preset mac-homebrew-Release`.

**System libs (macOS/Homebrew) — manual, no vcpkg:**
```bash
cmake -B build -S . -GNinja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix opencv);$(brew --prefix fftw);$(brew --prefix ffmpeg);$(brew --prefix catch2);$(brew --prefix fmt);$(brew --prefix spdlog);$(brew --prefix cli11)" \
  -DOpenCV_DIR=$(brew --prefix opencv)/lib/cmake/opencv4 \
  -DFFTW3f_DIR=$(brew --prefix fftw)/lib/cmake/fftw3 \
  -DFFMPEG_ROOT=$(brew --prefix ffmpeg) \
  -DWMR_BUILD_TESTS=ON
cmake --build build
```

**vcpkg (all platforms):**
```bash
cmake -B build -S . -GNinja
cmake --build build
```

**Tests:**
```bash
ctest --test-dir build --output-on-failure
./build/tests/wmr_tests "[v2]"          # single tag (path: tests/wmr_tests)
```
Integration tests need project root as CWD (they look for `test-images/` relative to CWD). Tests use `SKIP` macro if test data is absent, so they don't fail without it.

## Architecture

Single-pass C++20 CLI tool. No libraries — everything compiles into one `wmr` executable.

### Pipeline: Detect → Remove → Inpaint

`WatermarkEngine` (src/core/) orchestrates the image pipeline:

1. **NccDetector** (detection/) — 3-stage NCC: spatial template match (cv::matchTemplate), gradient match (Sobel magnitudes), variance analysis. Fusion: spatial×0.50 + gradient×0.30 + variance×0.20. Threshold: 0.35.
2. **Reverse alpha blend** (core/blend_modes) — `original = (watermarked - alpha*255) / (1-alpha)`. Alpha maps decoded from embedded PNGs (assets/embedded_assets.hpp).
3. **Inpaint** (core/inpaint) — Gaussian soft blend (default), TELEA, or Navier-Stokes. Cleans residual artifacts.

### AI Denoise (optional, OFF by default)

An FDnCNN denoiser (`src/core/ai_denoise.{hpp,cpp}`, NCNN + Vulkan, CPU fallback) is an optional residual-cleanup method, gated on `WMR_BUILD_AI_DENOISE`. When built (ON), AI is the **default** still-image cleanup and transparently falls back to Gaussian on init failure; the lean OFF build is provably AI-free.

- **Build:** `WMR_AI_DENOISE=1 ./scripts/build.sh` (inits the NCNN submodule + checks `vulkan-volk`/`molten-vk`). CI uses the vcpkg `ai-denoise` manifest feature (`volk`) — no Vulkan SDK install. NCNN is a git submodule; volk comes from vcpkg.
- **CLI (ON only):** `--denoise {ai|soft|ns|telea|off}`, `--sigma` (1–150), `--strength` (0–300 %), `--radius` (1–25) on `remove`/`visible`. `--denoise off` skips cleanup (reverse-blend only). OFF build has none of these — `--inpaint-strength` remains the only knob.
- **Dispatch:** `WatermarkEngine::remove_watermark_detected` takes an `InpaintConfig` overload (the `float` overload forwards). AI dispatches in the engine (engine-level, not in `inpaint.cpp`) — keeps ncnn headers out of the inpaint TU. All AI symbols are `#ifdef WMR_AI_DENOISE`-guarded so the OFF build compiles with zero AI knowledge.
- **Singleton lifetime:** `WatermarkEngine::denoiser()` returns an **intentionally-leaked** heap `NcnnDenoiser` (never destroyed). Destroying the embedded `ncnn::Net` during C++ static teardown races ncnn's global Vulkan-device teardown → EXC_BAD_ACCESS in `VulkanDevice::vkdevice()` at exit (only on the GPU path). Leaking the singleton is the standard fix for a process singleton owning a Vulkan context. Do NOT turn it back into a static-local.
- **Release:** a separate `ai-denoise` CI job ships `wmr-macos-arm64-ai.tar.gz` (macOS arm64 only) with `LICENSE-AI.md`; the 4 default lean binaries stay AI-free. The AI binary's only non-system dynamic deps are the Vulkan loader (`libvulkan.1.dylib`, a hard dyld load command) + MoltenVK (`libMoltenVK.dylib`, dlopened at runtime) — everything else is statically linked. `scripts/bundle_macos_vulkan.sh` bundles both (load cmd → `@rpath`, `@loader_path/lib` rpath, co-located `MoltenVK_icd.json`, ad-hoc re-sign) + a `wmr` launcher that sets `VK_ICD_FILENAMES`, so the tarball runs on a clean macOS (no SDK/Homebrew/MoltenVK). CI installs `vulkan-loader` (source of `libvulkan`) **and** `molten-vk` (source of `libMoltenVK`).

### SynthID Removal (two strategies)

Both operate in the frequency domain via `FftContext` (FFTW3 wrapper with plan caching):

- **CodebookSubtractor** — multi-pass spectral subtraction using a .wcb codebook. DC exclusion ramp, magnitude caps, per-channel weights (B=0.85, G=1.0, R=0.70).
- **NoiseResidualSubtractor** — codebook-free. Bilateral filter denoise → noise residual → FFT → estimate carrier. Two regimes: uniform images (replace with mean color) vs content images (phase noise in carrier band).

### Video Processing

`VideoProcessor` → `VideoReader` + `VideoWriter` (video/):

- Shot-level detection: samples 12 frames across first 90% of video, takes median position
- Per-frame: occlusion gate (skip if NCC < 0.35), position refinement (±4px tolerance vs shot anchor)
- Audio passthrough via fresh input context with timestamp rescaling
- Audio streams created before MP4 header write (valid moov atom)

### Scene Detection and Splitting (opt-in via `--scenes`)

`SceneDetector` (video/scene_detector) — combined BGR Bhattacharyya + MAD:

- Per-channel BGR histogram distance (max across channels) + mean absolute pixel difference
- Combined metric: `max(per_channel_bhatt, mad)` — catches chromatic and structural scene changes
- Default threshold 0.30, minimum scene length 15 frames
- Scans for scene boundaries, splits video into separate MP4 files at cuts
- `SceneInfo` contains only `start_frame`/`end_frame` (half-open interval)
- Single full-video watermark detection via `detect_in_shot()` (default params), applied uniformly across all split files
- `VideoWriter::copy_audio_range(start_sec, end_sec)` — seek-based audio copy with PTS offset subtraction
- Reader reads sequentially across scenes (no seeking within the loop)
- Each output file: I-frame at start, trimmed audio, correct container duration
- `-o` specifies output directory (defaults to `<input>_scenes/`); rejects file paths
- Output naming: `<stem>_<NNN>.mp4` with dynamic zero-padding

### CLI

CLI11 subcommands in src/cli/: `remove` (default), `visible`, `synthid`, `detect`, `video`, `build-codebook`. Directory inputs to remove/visible/synthid trigger batch mode (sequential, outputs to `cleaned/` subdirectory).

## Key Conventions

- Alpha maps are constexpr PNG byte arrays decoded at runtime via `cv::imdecode`
- Still-image watermark geometry is profile-aware (`WatermarkVariant::V1`/`V2`, default V2 with auto V2→V1 fallback; `--legacy` pins V1): V1 (legacy, pre-3.5) → 48×48 if either dim ≤ 1024 else 96×96, margins {32,32}/{64,64}; V2 (Gemini 3.5+) → large 96×96 @192px, small 36×36 with aspect-aware margin (`v2_small_config_from_dims`) + ±3px NCC snap (trusted iff spatial NCC ≥ 0.60). `WatermarkSize` (Small/Large) is a size class, not a pixel count (V2 Small = 36px alpha). Still `WatermarkVariant` is distinct from video `VideoVariant`.
- Video encoding defaults: libx264, CRF 14, High profile, slow preset
- Test executable re-compiles library sources (doesn't link main binary) — add new sources to both CMakeLists.txt and tests/CMakeLists.txt

## Platform Quirks

- CMakePresets.json is macOS-only (arm64, despite "x64" naming). Linux/Windows use manual cmake invocation.
- FFmpeg found via custom `cmake/FindFFMPEG.cmake` (pkg-config primary, `FFMPEG_ROOT` fallback). Creates imported targets `FFMPEG::avformat` etc.
- FFTW3 linked via variables in main build (`${FFTW3f_LIBRARIES}`) but via imported target in tests (`FFTW3::fftw3f`) — inconsistency inherited from vcpkg vs system lib resolution.
- Linux links static libgcc/libstdc++; MSVC uses static CRT.
