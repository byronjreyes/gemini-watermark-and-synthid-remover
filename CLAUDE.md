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
- **Release (1.7.0+):** single full-package build per (OS, arch) — no lean/full split, no separate AI tarball. The `build` matrix has 4 legs (mac arm64 native + bundled MoltenVK; mac x86_64 **cross-compiled** via `x64-osx` triplet + Rosetta on the arm64 runner — the only Intel runner `macos-13` was retired; linux; windows), all `WMR_BUILD_AI_DENOISE=ON`. A separate `tests` job builds AI+TESTS ON to cover the AI/routing unit tests. The mac arm64 binary's only non-system dynamic deps are the Vulkan loader (`libvulkan.1.dylib`, a hard dyld load command forced by passing `-DVulkan_LIBRARY`) + MoltenVK (`libMoltenVK.dylib`, dlopened at runtime) — everything else is statically linked. `scripts/bundle_macos_vulkan.sh` bundles both (load cmd → `@rpath`, `@loader_path/lib` rpath, co-located `MoltenVK_icd.json`, ad-hoc re-sign) + a `wmr` launcher that sets `VK_ICD_FILENAMES`, so the tarball runs on a clean macOS (no SDK/Homebrew/MoltenVK). The mac **x86_64** leg is built **`-DWMR_NCNN_VULKAN=OFF`** (CPU-only AI): on APPLE NCNN's simplevk does *static* Vulkan linkage and needs a build-time `libvulkan`, but there's no x86_64 Vulkan dylib on the arm64 cross-build runner, so Vulkan is compiled out (`ai_denoise.cpp` guards the GPU calls behind `#if NCNN_VULKAN`; NCNN propagates it via `platform.h`'s `#cmakedefine01`). The **linux / windows** legs pass **no** `Vulkan_LIBRARY` → NCNN `simplevk` (runtime `dlopen`, no Vulkan at build time, graceful CPU fallback). CI installs `vulkan-loader` + `molten-vk` on the arm64 leg only. **Windows CI gotchas (1.7.1):** (1) the project `cmake` step must run inside the MSVC dev env (`vswhere` + `vcvarsall.bat x64`, `shell: cmd` in `release.yml`) or CMake picks MinGW gcc from PATH → MSVC-vs-MinGW link failure; (2) `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` is set as a *variable* (not just the wmr target property) so the `ncnn` subproject inherits `/MT` to match the `x64-windows-static` deps — else LNK2005 (`msvcprt` vs `libcpmt`) + LNK2019 (`__imp_*` ucrt stubs). Licenses ship in `LICENSE-THIRD-PARTY.md`.

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

### NotebookLM Video Watermark (opt-in via `--notebooklm`)

`NotebookLMDetector` (video/notebooklm_detector.cpp) + `VideoProcessor::process_notebooklm` — removes the NotebookLM rainbow logo + "NotebookLM" wordmark from generated videos (cinematic / explainer / short-portrait exports).

- **Why a separate path**: the NotebookLM mark is semi-transparent, color-adaptive (light-on-dark / dark-on-light, scene-dependent), and H.264-compressed — NOT a reversible constant-alpha overlay. A temporal reverse-alpha recovery was investigated and **ruled out** (α≈0; the mark is adaptive with no mathematical inverse). Removal is spatial inpaint, chosen **per scene** by an adaptive dispatch.
- **Per-scene dispatch** (`process_notebooklm`, 1.6.0+; FSR routing 1.7.0; always-inpaint 1.7.1): `SceneDetector::detect_boundaries` splits the video; **every scene is inpainted** via `inpaint_mark_roi`, with the method chosen per scene by `resolve_inpaint_method` (notebooklm_gates — pure, unit-tested) from a per-scene **complexity gate** (`background_complexity` — Sobel gradient-energy in a gapped band around the mark): intricate (score ≥ `notebooklm_complexity_threshold`) → **FSR** (`cv::xphoto`, `WMR_HAS_XPHOTO`), uniform → **NS**. Single-file output; audio copied once. (A per-scene **presence gate** existed in 1.6.0–1.7.0 and was **removed in 1.7.1** — template matching couldn't reliably separate a faint-but-present mark from a genuinely-absent scene for this semi-transparent mark, so skipping risked leaving watermarks; inpainting an already-clean patch is imperceptible.)
- **Detection** (whole-video, for the bbox): template matching — multi-scale `|TM_CCOEFF_NORMED|` against each of ~12 sampled frames, keep the highest-scoring (polarity-invariant; robust across scene cuts). Template = embedded `notebooklm_mark_png` (98×14 grayscale, `assets/embedded_assets.hpp`). The detected bbox snaps to user-measured exact coordinates per known export mode (`kKnownModes`); unknown resolutions use the raw detection. Min confidence 0.45.
- **CLI**: `wmr video in.mp4 -o out.mp4 --notebooklm` (auto-detect); `--rect x,y,w,h` manual override; `--notebooklm-method {auto|ns|fsr}` (default `auto`; `CLI::IsMember`-validated); `--complexity-threshold`. Config: `VideoWatermarkConfig::{notebooklm_rect, notebooklm_method, notebooklm_complexity_threshold, notebooklm_presence_threshold}`.
- **Methods** (`inpaint_mark_roi`, static in video_processor.cpp): padded-crop inpaint. **FSR** = `cv::xphoto::INPAINT_FSR_FAST` (NOT `_BEST` — BEST is ~1.3 s/frame, infeasible for video; FAST ~64 ms with near-identical quality on this ~130×16 hole), 30 px context crop (`kFsrContextPad`) since it reconstructs the hole from surrounding texture; **NS** keeps the lean `radius+4` crop (local PDE — extra context doesn't help it). xphoto mask is **inverted** vs `cv::inpaint` (non-zero=valid). FSR is behind `WMR_HAS_XPHOTO` (CMake `if(TARGET opencv_xphoto)`; vcpkg `opencv4[contrib]`) and falls back to NS without it (`auto` == v1.6.0 NS, no regression). SHIFTMAP was A/B'd and rejected (copy-paste artifact). **Phase C** (optional, `WMR_BUILD_AI_LAMA`): LaMa ONNX (~2.4 s/frame CPU → infeasible as a video default).
- **Known limitation**: FSR kills the NS diamond/envelope PDE artifact on textured backgrounds, but the fill is still softly blurred on the most intricate backgrounds (partial fix). Phase C (LaMa) is the true-quality path.

### CLI

CLI11 subcommands in src/cli/: `remove` (default), `visible`, `synthid`, `detect`, `video`, `build-codebook`. Directory inputs to remove/visible/synthid trigger batch mode (sequential, outputs to `cleaned/` subdirectory).

## Key Conventions

- Alpha maps are constexpr PNG byte arrays decoded at runtime via `cv::imdecode`
- Still-image watermark geometry is profile-aware (`WatermarkVariant::V1`/`V2`, default V2 with auto V2→V1 fallback; `--legacy` pins V1): V1 (legacy, pre-3.5) → 48×48 if either dim ≤ 1024 else 96×96, margins {32,32}/{64,64}; V2 (Gemini 3.5+) → large 96×96 @192px, small 36×36 with aspect-aware margin (`v2_small_config_from_dims`) + ±3px NCC snap (trusted iff spatial NCC ≥ 0.60). `WatermarkSize` (Small/Large) is a size class, not a pixel count (V2 Small = 36px alpha). Still `WatermarkVariant` is distinct from video `VideoVariant`.
- Video encoding defaults: libx264, CRF 14, High profile, slow preset
- Test executable re-compiles library sources (doesn't link main binary) — add new sources to both CMakeLists.txt and tests/CMakeLists.txt
- `wmr --version` is the `APP_VERSION` define (`=project(wmr VERSION …)`) baked at CMake **configure** time (cached as `CMAKE_PROJECT_VERSION`). Editing the version doesn't change `build/wmr` until a reconfigure — `cmake --build build` reconfigures automatically when `CMakeLists.txt` changed.

## Platform Quirks

- CMakePresets.json is macOS-only (arm64, despite "x64" naming). Linux/Windows use manual cmake invocation.
- FFmpeg found via custom `cmake/FindFFMPEG.cmake` (pkg-config primary, `FFMPEG_ROOT` fallback). Creates imported targets `FFMPEG::avformat` etc.
- FFTW3 linked via variables in main build (`${FFTW3f_LIBRARIES}`) but via imported target in tests (`FFTW3::fftw3f`) — inconsistency inherited from vcpkg vs system lib resolution.
- Linux links static libgcc/libstdc++; MSVC uses static CRT.
- **Local build is DYNAMIC; CI is STATIC.** The Homebrew `build/wmr` links OpenCV/FFmpeg/fmt/spdlog dynamically (~10 MB); CI's vcpkg build is fully static (lean release binaries are ~29 MB single self-contained files — `otool -L` shows only system frameworks). Don't judge CI portability from the local binary — inspect the downloaded release binary (`gh release download`).
- GitHub macOS runners use a paravirtualized Metal GPU (`AppleParavirtDevice`) that throws during MoltenVK `vkCreateInstance` (`newArgumentEncoderWithLayout:`). The GPU path can't run in CI — the `ai-denoise` job verifies CPU only (`VK_ICD_FILENAMES=/nonexistent`); verify GPU out-of-band on real Apple Silicon.

## CI & Releases

- `.github/workflows/release.yml` builds the 4 lean binaries + the `ai-denoise` job (macOS arm64 AI tarball); `release` (`needs: [build, ai-denoise]`) attaches all six on a `v*` tag. **Validate a changed job off-cycle via `workflow_dispatch` before tagging** — avoids tag-force-move churn on failure.
- `gh run view --log` returns empty until the *whole run* completes (per-job logs too). To read a finished job's log fast, `gh run cancel` the run (preserves completed jobs' logs), then read — or wait for completion.
