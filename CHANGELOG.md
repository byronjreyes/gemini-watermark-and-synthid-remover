# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.6.0] - 2026-07-11

### NotebookLM adaptive per-scene inpaint dispatch

Phase A of the NotebookLM quality roadmap. `process_notebooklm` now dispatches **per scene** instead of inpainting every frame:

- **Presence gate** — per scene, template-match the mark on sampled frames; scenes where the mark is absent are written through unmodified (no needless inpaint, no degradation of clean frames). Reuses `NotebookLMDetector::mark_present_in_scene`.
- **Complexity gate** — per scene, a Sobel gradient-energy score around the mark (`notebooklm_gates`: `background_complexity_score`) classifies the background as uniform vs intricate (logged; routing active in Phase B).
- **ROI crop** — inpaint a padded crop around the mark (not the full frame): faster, smaller visible area.
- Removal is still NS-only; clean-frame copy / `cv::xphoto` SHIFTMAP (Phase B) and LaMa (Phase C, optional) are reserved behind `InpaintMethod::ShiftMap`/`LaMa`.

#### Changed
- `InpaintMethod` enum: `ShiftMap` (+ `LaMa` behind `WMR_AI_LAMA`).
- `VideoWatermarkConfig`: `notebooklm_method`, `notebooklm_complexity_threshold`, `notebooklm_presence_threshold`.
- CLI: `--notebooklm-method {ns|shiftmap|lama}`, `--complexity-threshold`.
- New `src/video/notebooklm_gates.{hpp,cpp}` (VideoReader-free gate logic; unit-tested) + `tests/unit/notebooklm_gates_test.cpp`.
- vcpkg.json version drift fixed (was stale at 1.4.0).

#### Note
A temporal reverse-alpha "recovery" method was investigated and **ruled out** — the NotebookLM mark is adaptive (α≈0) with no mathematical inverse. Remaining quality upgrades are spatial (Phase B: clean-frame copy + SHIFTMAP; Phase C: optional LaMa ONNX).

## [1.5.0] - 2026-07-10

### NotebookLM video watermark removal

New `--notebooklm` mode removes the NotebookLM rainbow logo + "NotebookLM" wordmark from generated videos (cinematic, explainer, short-portrait exports).

- **Detection** (`src/video/notebooklm_detector.cpp`): template matching — multi-scale `|TM_CCOEFF_NORMED|` against each sampled frame, keep the best. Polarity-invariant (handles the adaptive light-on-dark / dark-on-light mark) and robust across scene cuts, where an earlier temporal-median-contrast detector failed. The detected bbox snaps to user-measured exact coordinates per known export mode.
- **Removal**: per-frame Navier-Stokes `cv::inpaint` over the mark bbox (dilated), reusing the existing inpaint primitive. Reverse alpha-blend does **not** apply — the NotebookLM mark is semi-transparent, color-adaptive, and H.264-compressed, so it is not a reversible constant-alpha overlay.
- **CLI**: `wmr video in.mp4 -o out.mp4 --notebooklm` (auto-detect); `--rect x,y,w,h` manual override for edge cases.
- **Known limitation**: on complex/textured backgrounds (e.g. explainer mode), spatial inpaint fabricates the region from its boundary — usable but imperfect. A temporal reverse-alpha method that recovers the true background is planned.

#### Changed
- `VideoProfile` gains `NotebookLM`; `VideoWatermarkConfig` gains `notebooklm_rect`.
- New embedded asset `notebooklm_mark_png` (98×14 grayscale template) in `assets/embedded_assets.hpp`.

## [1.4.0] - 2026-06-29

### Standalone macOS AI Binary

The `wmr-macos-arm64-ai` artifact (deferred in 1.3.0) now ships and runs on a clean macOS install.

#### Added
- `wmr-macos-arm64-ai.tar.gz` release artifact — bundles the Vulkan **loader** (`libvulkan.1.dylib`) + **MoltenVK** (`libMoltenVK.dylib`) + an ICD manifest next to the binary, so the Metal GPU driver travels with it. No Vulkan SDK, Homebrew, or MoltenVK needed at runtime. Layout: `wmr` (launcher) + `wmr.bin` (binary) + `lib/`. The `wmr` launcher sets `VK_ICD_FILENAMES` to the bundled manifest, then `exec`s `wmr.bin`.
- `scripts/bundle_macos_vulkan.sh` — rewrites the binary's `libvulkan` load command to `@rpath` + adds an `@loader_path/lib` rpath, writes a co-located ICD manifest (`library_path` relative to itself), ad-hoc re-signs (Apple Silicon kills stale-signed Mach-Os), and emits the launcher. Used by CI; works locally too.
- The `ai-denoise` CI job now installs `vulkan-loader` (the source of `libvulkan` — `molten-vk` ships only `libMoltenVK`), bundles the dylibs, and verifies the extracted tarball launches and runs a denoise (via the launcher, forcing CPU with `VK_ICD_FILENAMES=/nonexistent` since GitHub's paravirtualized macOS GPU can't run MoltenVK's `vkCreateInstance`; the GPU path is verified out-of-band on real Apple Silicon). The launcher respects a pre-set `VK_ICD_FILENAMES`. Re-enabled on tag pushes and re-coupled to the `release` job, so the AI tarball ships with every release.

#### Fixed
- ncnn/Vulkan teardown crash: `wmr` segfaulted (EXC_BAD_ACCESS in `ncnn::VulkanDevice::vkdevice()`) on process exit whenever the GPU path ran, because the `NcnnDenoiser` process-singleton's `~ncnn::Net` destructor ran during C++ static teardown *after* ncnn's global Vulkan device was already torn down. The singleton is now an intentionally-leaked heap instance (`WatermarkEngine::denoiser()`) so its destructor never runs at exit — the standard pattern for a process singleton that owns a GL/Vulkan context. Per-image output was always saved; the fix makes the exit code clean (was 139/SIGSEGV). Surfaced now because bundling made the GPU path reliable (the CPU fallback path never created the device).

## [1.3.0] - 2026-06-28

### FDnCNN AI Denoise (optional) — Still-Image Residual Cleanup

#### Added
- FDnCNN AI denoiser (NCNN + Vulkan, CPU fallback) as an optional residual-cleanup method after reverse alpha blending, ported from allenk/GeminiWatermarkTool (MIT). Original model from cszn/KAIR (MIT).
- `WMR_BUILD_AI_DENOISE` CMake option (OFF by default) — when ON, AI is the default still-image cleanup with automatic Gaussian fallback on init failure.
- `NcnnDenoiser` module (`src/core/ai_denoise.{hpp,cpp}`) + `src/core/ai_denoise_model.cpp` + `src/core/ncnn_shim.hpp`, all `#ifdef WMR_AI_DENOISE`-guarded.
- `--denoise {ai|soft|ns|telea|off}`, `--sigma`, `--strength`, `--radius` flags on `remove`/`visible` (AI-only; absent from the lean OFF build, which keeps `--inpaint-strength` as the sole cleanup knob).
- `WatermarkEngine::remove_watermark_detected` `InpaintConfig` overload (the `float` overload forwards for backward compatibility).
- `InpaintMethod::AiDenoise` (guarded in-enum); `InpaintConfig::sigma`.
- `[aidenoise]` ctest tag (model load, synthetic residual, masked blend, Gaussian fallback) — runs only on the ON build.
- New `LICENSE-AI.md` at repo root (NCNN BSD-3-Clause + volk MIT + KAIR/FDnCNN MIT + allenk conversion credit) shipped with the AI binary.
- New `ai-denoise` CI job producing the `wmr-macos-arm64-ai` release artifact (macOS arm64 only for the first AI release; x86_64/Linux/Windows AI builds are future work).

#### Build
- NCNN built from a git submodule at `external/ncnn/ncnn-20260113-src`; volk supplied by the vcpkg `ai-denoise` manifest feature (NOT a default dependency — lean binaries stay AI-free).
- Adds ~6.8 MB of embedded-model headers (`assets/model_core.{mem.h,id.h}`), committed so the AI CI job builds from a clean checkout without a network fetch. Provenance: headers captured from a one-time allenk/GeminiWatermarkTool `ENABLE_AI_DENOISE=ON` build (its `ncnn2mem` step); model bytes are upstream's converted weights.
- `scripts/build.sh` AI mode: `WMR_AI_DENOISE=1` (inits the submodule, adds the `vulkan-volk`/`molten-vk` Homebrew deps, passes `-DWMR_BUILD_AI_DENOISE=ON`).
- The 4-job default release matrix is untouched and AI-free (R7): no submodules, no `WMR_BUILD_AI_DENOISE`, no `ai-denoise` vcpkg feature.

#### Out of scope
- Video AI denoise — the video pipeline stays alpha-only (no inpaint, no AI).

## [1.2.0] - 2026-06-27

### Still-Image V2 (Gemini 3.5) Watermark Parity

#### Added
- `WatermarkVariant {V1, V2}` (`src/core/types.hpp`) — still-image profile generations, default V2. Separate from video `VideoVariant`.
- `get_watermark_config(W,H,variant)` + `v2_small_config_from_dims` — V2 large {192,192,96}; V2 small 36×36 with aspect-aware margin (ported verbatim from upstream allenk/GeminiWatermarkTool v0.3.1).
- V2-aware `WatermarkEngine::detect_watermark` (6-arg) / `remove_watermark` (3-arg) — default V2 with auto V2→V1 fallback on miss. The V2 alpha flows through the existing `custom_alpha` channel; `blend_modes` is unchanged.
- ±3px NCC snap refinement for V2+Small in `NccDetector::detect` (trusted iff spatial NCC ≥ 0.60), via a new trailing `enable_snap` param.
- `--legacy` / `--no-legacy` flags on `remove`, `visible`, `detect` (still subcommands) and batch. `--legacy` pins V1; `--no-legacy` pins V2 and disables fallback; both → exit 2. Video `--legacy` (Veo-text) is unchanged (CLI11-scoped per subcommand).
- New `[v2]` ctest tag: synthesized Gemini 3.5 round-trip (≤1 LSB mean), V2 small + snap (36×36 region), and V1-legacy-default tests.

#### Changed
- Still-image default profile flipped V1 → V2 (with auto fallback). Legacy 4-arg `detect_watermark` / 2-arg `remove_watermark` inline overloads keep routing to V1, so the video pipeline and existing tests are provably unchanged.
- Video pipeline, Veo, NotebookLM, SynthID — out of scope, unchanged.

#### Build — robust macOS/Homebrew workflow (survives `brew upgrade`)
- `scripts/build.sh`: self-healing build helper — verifies required Homebrew formulas, auto-wipes + reconfigures when a cached dependency path has vanished after an upgrade, configures against the stable `/opt/homebrew/opt/…` symlinks, then builds + tests.
- `cmake/FindFFMPEG.cmake`: prefer `FFMPEG_ROOT` (stable opt symlink) over pkg-config's versioned Cellar realpath, so the cached FFmpeg path survives an upgrade.
- `CMakePresets.json`: `mac-homebrew-Release`/`-Debug` presets (system libs, no vcpkg).
- `CMakeLists.txt`: synthesize the `FFTW3::fftw3f` imported target from Homebrew's variable-only fftw3 config (no-op under vcpkg) so the test target links.
- `tests/CMakeLists.txt`: exclude unused `scene_detector.cpp` from the test link (it pulled in unlinked FFmpeg via `VideoReader`).
- Test suite needs Catch2 (`brew install catch2`); `scripts/build.sh` verifies deps.

## [1.1.0] - 2026-06-02

### Phase 7: Scene Detection and Splitting — COMPLETE

#### Added
- `SceneDetector` class (`src/video/scene_detector.hpp/cpp`) — combined BGR histogram + pixel difference scene boundary detection:
  - Downsamples frames to longest-side 320px, preserves BGR color
  - Per-channel BGR Bhattacharyya distance (max across channels) + mean absolute pixel difference (MAD)
  - Combined metric: `max(per_channel_bhatt, mad)` — catches both chromatic and structural changes
  - Configurable threshold (default 0.30) and minimum scene length (15 frames)
  - Short scene merging into predecessor
  - Owns its own `VideoReader` — main reader stays pristine for processing
- `--scenes` flag on `video` subcommand — splits video into separate MP4 files at scene boundaries
- `--scene-threshold` option — scene cut sensitivity 0.0-1.0 (default 0.3)
- Scene splitting pipeline: boundary detection → single watermark detection → multi-file output with per-scene audio trimming
- `VideoWriter::copy_audio_range()` — seek-based audio range copy with PTS offset subtraction for clean scene-boundary alignment
- Production-ready split files: I-frame at start, audio trimmed to scene range, correct container duration, independently playable
- Output naming: `<stem>_<NNN>.mp4` with dynamic zero-padding; `-o` specifies directory (defaults to `<input>_scenes/`)
- CLI validation: `--scenes` rejects file output, creates directory automatically

## [1.0.0] - 2026-05-31

### Phase 6: Video Watermark Removal + CLI Polish — COMPLETE

#### Fixed
- Video watermark removal now uses pure reverse alpha blend (`remove_watermark_alpha_only`) instead of alpha blend + Gaussian inpaint — eliminates blur/diamond artifacts on both Gemini and Veo
- All frames are now processed when shot detection confirms the watermark — previously occlusion gate (NCC < 0.35) skipped frames; now falls back to shot anchor position for undetected frames
- Integrated upstream GeminiWatermarkTool V2 diamond alpha maps (36x36 and 96x96) for more accurate Gemini video removal
- Added `correct_alpha_for_background()` to recover true alpha from captures on non-black backgrounds

#### Changed
- Video pipeline: `remove_watermark_detected` → `remove_watermark_alpha_only` for lossless frame restoration
- Detection mode no longer skips frames — uses shot anchor as fallback when per-frame NCC is low

#### Added
- `VideoReader` class (`src/video/video_reader.hpp/cpp`) — FFmpeg demux+decode pipeline with seeking and frame counting
- `VideoWriter` class (`src/video/video_writer.hpp/cpp`) — FFmpeg encode pipeline (libx264, CRF 14, High profile) with audio passthrough:
  - Audio streams created before MP4 header write (valid moov atom)
  - Fresh input context for audio packet copy with timestamp rescaling
  - BGR24→YUV420P colorspace conversion via swscale
- `VideoProcessor` class (`src/video/video_processor.hpp/cpp`) — frame-by-frame watermark removal:
  - Shot-level NCC detection: samples frames across first 90% of video, takes median position/confidence
  - Per-frame occlusion gate: skips frames where watermark not reliably detected
  - Position refinement: falls back to shot anchor if detection drifts beyond tolerance
  - Frame dimension guard against corrupt seek artifacts
  - Progress output with fps and ETA
- `video` CLI subcommand: `wmr video input.mp4 -o output.mp4` with `--legacy`, `--variant`, `--crf`, `--preset`, `--codec`, `--force` options
- Codebook-free SynthID removal via `NoiseResidualSubtractor` (`src/synthid/noise_residual_subtractor.hpp/cpp`):
  - Estimates carrier from bilateral filter noise residual
  - `--codebook-free` flag as alternative to `--codebook`
- `cmake/FindFFMPEG.cmake` — cross-platform FFmpeg discovery via pkg-config / FFMPEG_ROOT (covers Homebrew and system package managers)
- CLI header with version, description, GitHub URL, and copyright
- Contextual subcommand help: shows subcommand-specific help when required args are missing (e.g. `wmr video`)
- Version output includes GitHub URL

#### Changed
- CMake links FFmpeg via imported targets (`FFMPEG::avformat` etc.) instead of variable-based linking
- `vcpkg.json` version bumped to 0.2.0
- Project version bumped to 0.2.0
- Single-image subcommands (`visible`, `synthid`, `remove`) now require `-o` explicitly — no default overwrite
- Batch processing defaults to `cleaned/` subdirectory instead of modifying originals
- Purged large codebook files (>100MB) from git history, excluded via `.gitignore`
- `.gitignore` updated to exclude HF datasets, generated codebooks, temp analysis outputs

### Phase 5: Unified CLI + Test Suite — COMPLETE

#### Added
- CLI restructured with CLI11 subcommands: `remove`, `detect`, `visible`, `synthid`, `build-codebook`
- Batch processing module (`src/cli/batch_processor.hpp/cpp`) — directory scanning with optional `--recursive`, parallel-safe
- `BatchResult` struct tracking total/succeeded/failed/skipped counts
- Catch2 v3 test suite with 17 tests, 54 assertions (all passing):
  - Unit tests: blend_modes (3), fft_context (4), spectral_codebook (4), inpaint (3)
  - Integration tests: visible_pipeline (3, 2 SKIP in build dir, pass from project root)
- Tests linked via `tests/CMakeLists.txt` with `catch_discover_tests()` and CTest integration
- Build option `WMR_BUILD_TESTS` (default ON) to toggle test building

### Phase 4: SynthID Detection + Codebook Builder — COMPLETE

#### Added
- `SynthidDetector` class (`src/detection/synthid_detector.hpp/cpp`) — 4-method Bayesian detection:
  - Noise correlation via bilateral filter denoise + NCC of noise spectrum vs profile
  - Carrier phase matching via element-wise cosine of phase differences
  - Structure ratio: energy at carrier bins vs total
  - Multi-scale consistency: phase coherence at 1x, 0.5x, 0.25x scales
  - Weighted fusion: noise_corr×0.35 + carrier_phase×0.35 + structure×0.15 + multi_scale×0.15
  - Configurable threshold (default 0.50)
- `CodebookBuilder` class (`src/synthid/codebook_builder.hpp/cpp`) — build spectral codebooks from reference images:
  - Per-channel FFT magnitude/phase accumulation
  - Automatic resolution bucketing
  - Consistency computation (std dev across samples)
  - Quality gate: warns on <3 samples per resolution

### Phase 3: SynthID Spectral Infrastructure — COMPLETE

#### Added
- `FftContext` class (`src/core/fft_context.hpp/cpp`) — FFTW3 wrapper with plan caching:
  - Forward/inverse 2D FFT with CV_32FC1/CV_32FC2 interop
  - Magnitude, phase, and polar reconstruction utilities
  - Plan caching keyed on (rows, cols, direction) with dummy arrays for FFTW_MEASURE safety
- `SpectralProfile` struct in `src/core/types.hpp` — per-resolution FFT data (magnitude, phase, consistency per BGR channel)
- `SpectralCodebook` class (`src/synthid/spectral_codebook.hpp/cpp`) — JSON-based codebook persistence:
  - Save/load with nearest-resolution fallback
  - `--codebook` CLI flag for specifying codebook path
- `CodebookSubtractor` class (`src/synthid/codebook_subtractor.hpp/cpp`) — multi-pass spectral subtraction:
  - Aggressive→moderate→gentle removal schedule
  - `--synthid-strength` CLI flag (0.0–2.0, default 1.0)
- `fftw3f` linked via vcpkg, FFTW3::fftw3f CMake target

#### Fixed
- SpectralProfile aggregate init bug: explicit field assignment prevents misaligned brace initialization
- FFTW_MEASURE input corruption: dummy arrays prevent plan creation from overwriting real data
- OpenCV `cv::cos` (nonexistent) replaced with element-wise `std::cos` loop in carrier phase matching

### Phase 2: Visible Watermark Detection + Inpainting — COMPLETE

#### Added
- `DetectionResult` struct in `src/core/types.hpp` — detection result with confidence scores, region, and per-stage scores
- `NccDetector` class (`src/detection/ncc_detector.hpp/cpp`) — three-stage NCC detection pipeline:
  - Stage 1: Spatial NCC via template matching with circuit breaker at 0.25
  - Stage 2: Gradient NCC via Sobel-filtered magnitude matching
  - Stage 3: Variance analysis comparing watermark region to reference region
  - Heuristic fusion: spatial×0.50 + gradient×0.30 + variance×0.20
  - Detection threshold: confidence ≥ 0.35
- `InpaintMethod` enum and `InpaintConfig` struct in `src/core/inpaint.hpp`
- `inpaint_residual()` function (`src/core/inpaint.hpp/cpp`) — three traditional inpainting methods:
  - Gaussian soft inpaint with gradient-weighted mask
  - TELEA inpaint (cv::inpaint INPAINT_TELEA)
  - Navier-Stokes inpaint (cv::inpaint INPAINT_NS)
  - Configurable strength, radius, and padding
- `opencv_photo` linked for cv::inpaint support
- WatermarkEngine integration: `detect_watermark()`, `remove_watermark_detected()`, `inpaint_residual()` methods
- CLI updated: default flow is now detect→remove→inpaint; `--force` skips detection; `--detect-only` reports results
- Verified: 80% detection confidence on test image, successful removal with Gaussian inpainting (9671 active pixels)

## [0.1.0] - 2026-05-28

### Phase 1: Visible Watermark Removal

#### Added
- C++20 project structure with CMake build system (CMakePresets.json, vcpkg.json)
- `WatermarkEngine` class — reverse alpha blending for visible watermark removal
- `blend_modes` module — alpha map calculation and forward/reverse alpha blending
- Embedded PNG assets (48x48 and 96x96 background captures) in `assets/embedded_assets.hpp`
- CLI via CLI11: `wmr input.png -o output.png` with `--force`, `--force-small`, `--force-large`, `-v` flags
- Auto-detection of watermark size based on image dimensions
- Support for PNG, JPEG, WebP output formats with quality preservation
