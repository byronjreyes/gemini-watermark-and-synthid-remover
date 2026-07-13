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
- **Release (1.7.0+):** single full-package build per (OS, arch) — no lean/full split, no separate AI tarball. The `build` matrix has 4 legs (mac arm64 native + bundled MoltenVK; mac x86_64 **cross-compiled** via `x64-osx` triplet + Rosetta on the arm64 runner — the only Intel runner `macos-13` was retired; linux; windows), all `WMR_BUILD_AI_DENOISE=ON`. A separate `tests` job builds AI+TESTS ON to cover the AI/routing unit tests. The mac arm64 binary's only non-system dynamic deps are the Vulkan loader (`libvulkan.1.dylib`, a hard dyld load command forced by passing `-DVulkan_LIBRARY`) + MoltenVK (`libMoltenVK.dylib`, dlopened at runtime) — everything else is statically linked. `scripts/bundle_macos_vulkan.sh` bundles both (load cmd → `@rpath`, `@loader_path/lib` rpath, co-located `MoltenVK_icd.json`, ad-hoc re-sign) + a `wmr` launcher that sets `VK_ICD_FILENAMES`, so the tarball runs on a clean macOS (no SDK/Homebrew/MoltenVK). The mac **x86_64** leg is built **`-DWMR_NCNN_VULKAN=OFF`** (CPU-only AI): on APPLE NCNN's simplevk does *static* Vulkan linkage and needs a build-time `libvulkan`, but there's no x86_64 Vulkan dylib on the arm64 cross-build runner, so Vulkan is compiled out (`ai_denoise.cpp` guards the GPU calls behind `#if NCNN_VULKAN`; NCNN propagates it via `platform.h`'s `#cmakedefine01`). The **linux / windows** legs pass **no** `Vulkan_LIBRARY` → NCNN `simplevk` (runtime `dlopen`, no Vulkan at build time, graceful CPU fallback). CI installs `vulkan-loader` + `molten-vk` on the arm64 leg only. **Windows CI gotchas (1.7.1):** (1) the project `cmake` step must run inside the MSVC dev env (`vswhere` + `vcvarsall.bat x64`, `shell: cmd` in `release.yml`) or CMake picks MinGW gcc from PATH → MSVC-vs-MinGW link failure; (2) `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` is set as a *variable* (not just the wmr target property) so the `ncnn` subproject inherits `/MT` to match the `x64-windows-static` deps — else LNK2005 (`msvcprt` vs `libcpmt`) + LNK2019 (`__imp_*` ucrt stubs). Licenses ship in `LICENSE-THIRD-PARTY.md`. **(1.10.0) MI-GAN** is platform-split: **macOS (arm64 + x86_64) = native CoreML** (`WMR_BUILD_AI_MIGAN=ON` → the CMake `if(APPLE)` branch compiles `migan_coreml_inpainter.mm`, links the system CoreML framework — no ORT, no vendored lib — and ships the 14 MB `migan_512_places2_fp16.mlpackage` dir, Git LFS via `assets/.../weight.bin`; `lfs: enable_migan` on checkout). mac x86_64 is now a **tarball** (was a bare binary) so the `.mlpackage` sits next to it; Intel Macs gain MI-GAN (were OFF in 1.9.0). **linux/windows = ORT** (unchanged from 1.9.0): the ONNX Runtime shared lib + 27 MB `migan_pipeline_v2.onnx` — **archives** (`wmr` + `libonnxruntime.so.1`/`onnxruntime.dll` + model; linux `patchelf --set-rpath '$ORIGIN'`). **ORT = official v1.27.1 prebuilt fetched at CMake configure (NOT the vcpkg port — heavy source build would threaten the Windows 6 h cap)**, `IMPORTED` target `wmr_ort`, SHA256-pinned; windows post-build-copies `onnxruntime.dll` next to `wmr.exe` (exe-dir search). Licenses ship in `LICENSE-THIRD-PARTY.md`.

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
- **Per-scene dispatch** (`process_notebooklm`, 1.6.0+; FSR routing 1.7.0; always-inpaint 1.7.1; **MI-GAN default 1.9.0** — FSR + LaMa removed): `SceneDetector::detect_boundaries` splits the video; **every scene is inpainted** via `inpaint_mark_roi`, with the method chosen per scene by `resolve_inpaint_method` (notebooklm_gates — pure, unit-tested) from a per-scene **complexity gate** (`background_complexity` — Sobel gradient-energy in a gapped band around the mark): intricate (score ≥ `notebooklm_complexity_threshold`) → **MI-GAN** (ONNX Runtime, `WMR_AI_MIGAN`), uniform → **NS**. **No method flag** — the pipeline always uses NS + MI-GAN. Single-file output; audio copied once. (A per-scene **presence gate** existed in 1.6.0–1.7.0 and was **removed in 1.7.1** — template matching couldn't reliably separate a faint-but-present mark from a genuinely-absent scene for this semi-transparent mark, so skipping risked leaving watermarks; inpainting an already-clean patch is imperceptible.)
- **Detection** (whole-video, for the bbox): template matching — multi-scale `|TM_CCOEFF_NORMED|` against each of ~12 sampled frames, keep the highest-scoring (polarity-invariant; robust across scene cuts). Template = embedded `notebooklm_mark_png` (98×14 grayscale, `assets/embedded_assets.hpp`). The detected bbox snaps to user-measured exact coordinates per known export mode (`kKnownModes`); unknown resolutions use the raw detection. Min confidence 0.45. **(1.9.0)** the explainer-mode rect was corrected from `(1105,660,131,16)` → `(1085,658,153,20)` — the old one started at the spiral logo's right edge, leaving ~18px of the logo unmasked (NS/FSR/LaMa hid it by blurring; MI-GAN's precise fill exposed it).
- **CLI**: `wmr video in.mp4 -o out.mp4 --notebooklm` (auto-detect); `--rect x,y,w,h` manual override; `--complexity-threshold` (NS↔MI-GAN gate, default 15). **No `--notebooklm-method`** (removed in 1.9.0 — MI-GAN is the default). Config: `VideoWatermarkConfig::{notebooklm_rect, notebooklm_complexity_threshold}`.
- **Methods** (`inpaint_mark_roi`, static in video_processor.cpp): **MI-GAN** (`migan` branch) = `MiganInpainter::inpaint_hole` (leaked singleton, `#ifdef WMR_AI_MIGAN`). Two platform impls share one interface (`migan_inpainter.hpp`):
  - **macOS (1.10.0)** — `src/core/migan_coreml_inpainter.mm` (ObjC++, `#ifdef WMR_AI_MIGAN_COREML`): native CoreML fp16 mlprogram on the ANE, **~28 ms/frame** (~11× over ORT-CPU; A/B-verified to match the ORT baseline within Δ1.9/255). The bare `Generator(resolution=512)` is resolution-locked, so it crops a square around the mark → resizes to 512² → builds the (1,4,512,512) f32 input `cat([mask−0.5, img×mask])` (img in [−1,1], mask 0=hole) → predicts → denormalizes → soft-pastes (dilated+blurred mask). Two-step load (`+compileModelAtURL:` cached, then `+modelWithContentsOfURL:configuration:error:` — a CLASS factory, not `[[MLModel alloc] initWith…]` which doesn't exist), `MLComputeUnitsAll` (ANE preferred). **CoreML may deliver the output as Float16 OR Float32 — handle both** (`outMA.dataType` check). Replaces ORT entirely on mac: CoreML is a system framework (no vendored lib), the `.mlpackage` is arch-neutral → **both arm64 AND x86_64** ship it (Intel Macs gain MI-GAN; was OFF). Resolved via `$WMR_COREML_MODEL` / `<exedir>/migan_512_places2_fp16.mlpackage` / `<exedir>/../share/wmr/...`. First load compiles the .mlpackage (one-time, cached by CoreML).
  - **Linux/Windows (1.9.0)** — `src/core/migan_inpainter.cpp` (ORT): feeds the **whole frame** (RGB uint8 NCHW, dynamic-res) + mask (0=hole, dilated 5×5) → uint8 result. ~225 ms/frame CPU. ORT = vendored official v1.27.1 prebuilt (NOT the vcpkg port — heavy source build threatens the Windows 6 h cap): `file(DOWNLOAD)`+SHA256, `IMPORTED wmr_ort`; linux `patchelf $ORIGIN` + libonnxruntime.so.1, windows onnxruntime.dll in exe dir. Model `assets/migan_pipeline_v2.onnx` (~27 MB, LFS) via `$WMR_MIGAN_MODEL` / `<exedir>/…`.
  - **(1.10.0) mac no longer fetches ORT** — the `if(WMR_BUILD_AI_MIGAN)` block splits `if(APPLE)` (CoreML: `enable_language(OBJCXX)`, `find_library CoreML+Foundation`, `.mm`, `copy_directory` the `.mlpackage`) / `else()` (ORT). `video_processor.cpp` dispatch + leaked singleton are byte-identical for both.
  - **NS** = `cv::inpaint` Navier-Stokes on a lean `radius+4` padded crop (uniform-scene + MI-GAN-unavailable fallback). MI-GAN is MIT (Picsart, ICCV 2023). **Dead weight:** vcpkg `opencv4[contrib]`/xphoto is still built though FSR is gone — drop in a follow-up.
- **History note — "CoreML is slower" was WRONG:** the 1.9.0 finding "GPU (CoreML) tested — SLOWER (602 ms)" was ORT's *CoreML execution provider* (only 375/559 nodes on CoreML → partition overhead). A **native** `coremltools` mlprogram (whole graph in one MIL program, no partitioning) is the opposite — 28 ms, 11× faster. Don't re-conclude CoreML is slow from the ORT-EP number.
- **Known limitation** (1.10.0): MI-GAN is sharp + robust on cartoons/textures. macOS CoreML ~28 ms/frame on the ANE; Linux/Windows ORT ~225 ms/frame. macOS requires 14+ (the .mlpackage targets `minimum_deployment_target=macOS14`; the Conv/LeakyRelu/Resize graph needs only macOS12+ mlprogram — re-verified 28 ms / Δ1.9 hold at macOS14). CoreML on the GitHub `macos-14` runner falls back to CPU (paravirtualized ANE) — CI smoke proves load+predict+teardown, not the 28 ms (arm64-ANE-native, verified locally). x86_64 CoreML *link* is verified (CoreML.tbd has an x86_64 slice); x86_64 *runtime under Rosetta* is the CI Step-0 open item (fallback: flip `enable_migan`→`false` for x86_64 only — the CMake split makes it a one-line matrix revert).

### CLI

CLI11 subcommands in src/cli/: `remove` (default), `visible`, `synthid`, `detect`, `video`, `build-codebook`. Directory inputs to remove/visible/synthid trigger batch mode (sequential, outputs to `cleaned/` subdirectory).

## Key Conventions

- Alpha maps are constexpr PNG byte arrays decoded at runtime via `cv::imdecode`
- Still-image watermark geometry is profile-aware (`WatermarkVariant::V1`/`V2`, default V2 with auto V2→V1 fallback; `--legacy` pins V1): V1 (legacy, pre-3.5) → 48×48 if either dim ≤ 1024 else 96×96, margins {32,32}/{64,64}; V2 (Gemini 3.5+) → large 96×96 @192px, small 36×36 with aspect-aware margin (`v2_small_config_from_dims`) + ±3px NCC snap (trusted iff spatial NCC ≥ 0.60). `WatermarkSize` (Small/Large) is a size class, not a pixel count (V2 Small = 36px alpha). Still `WatermarkVariant` is distinct from video `VideoVariant`.
- Video encoding defaults: libx264, CRF 14, High profile, slow preset
- Test executable re-compiles library sources (doesn't link main binary) — add new sources to both CMakeLists.txt and tests/CMakeLists.txt
- `wmr --version` is the `APP_VERSION` define (`=project(wmr VERSION …)`) baked at CMake **configure** time (cached as `CMAKE_PROJECT_VERSION`). Editing the version doesn't change `build/wmr` until a reconfigure — `cmake --build build` reconfigures automatically when `CMakeLists.txt` changed.
- Integrating an ONNX inpainter: **probe its IO empirically** (input/output dtype + range, mask polarity — e.g. MI-GAN is uint8 RGB + mask 0=hole; Carve/LaMa-ONNX *outputs* [0,255], not [0,1]); don't assume. Then **verify the actual output is a valid image** (per-channel mean ≈ the original scene, not ~0/~255 saturated) — a white/black output = a scale/polarity bug. Don't trust VLM quality judgments on small/upscaled crops; verify objectively (pixel mean, tensor diff vs a known-good reference).
- Before commit/merge: `git add -A` + `git status`. The test exe and the dev `build/` compile from the **working tree**, which masks un-staged changes — the MI-GAN swap once shipped a commit with only the *new* files staged, missing modifications/deletions to existing source (caught only by `git status` pre-merge).
- License/redistribution suitability is the project owner's call, not a hard pre-filter. When evaluating dependencies, rank on technical merit and report license facts separately; don't silently exclude GPL/CC-NC options before the owner decides.

## Platform Quirks

- CMakePresets.json is macOS-only (arm64, despite "x64" naming). Linux/Windows use manual cmake invocation.
- FFmpeg found via custom `cmake/FindFFMPEG.cmake` (pkg-config primary, `FFMPEG_ROOT` fallback). Creates imported targets `FFMPEG::avformat` etc.
- FFTW3 linked via variables in main build (`${FFTW3f_LIBRARIES}`) but via imported target in tests (`FFTW3::fftw3f`) — inconsistency inherited from vcpkg vs system lib resolution.
- Linux links static libgcc/libstdc++; MSVC uses static CRT.
- **Local build is DYNAMIC; CI is STATIC.** The Homebrew `build/wmr` links OpenCV/FFmpeg/fmt/spdlog dynamically (~10 MB); CI's vcpkg build is fully static (lean release binaries are ~29 MB single self-contained files — `otool -L` shows only system frameworks). Don't judge CI portability from the local binary — inspect the downloaded release binary (`gh release download`).
- GitHub macOS runners use a paravirtualized Metal GPU (`AppleParavirtDevice`) that throws during MoltenVK `vkCreateInstance` (`newArgumentEncoderWithLayout:`). The GPU path can't run in CI — the `ai-denoise` job verifies CPU only (`VK_ICD_FILENAMES=/nonexistent`); verify GPU out-of-band on real Apple Silicon.
- CMake post-build model copies (NCNN `ai_denoise_model`, MI-GAN `migan_pipeline_v2.onnx`) are guarded by `if(EXISTS assets/…)` evaluated at **configure time**. Adding/renaming a model under `assets/` after configuring won't copy it next to the binary → silent "model not found" → runtime fallback (NS). Reconfigure (or touch `CMakeLists.txt`) after changing a model asset. The CoreML `.mlpackage` is a **directory** — use `copy_directory` (not `copy_if_different`), `cp -R` in the bundle step, and Git LFS can't directory-glob so the weight blob needs a full-path rule (`assets/migan_512_places2_fp16.mlpackage/Data/com.apple.CoreML/weights/weight.bin filter=lfs`).

## CI & Releases

- `.github/workflows/release.yml`: the `build` matrix (4 legs: mac arm64 native + bundled Vulkan/MoltenVK + CoreML `.mlpackage`; mac x86_64 cross-compiled + CoreML `.mlpackage` (tarball); linux; windows — all `WMR_BUILD_AI_MIGAN=ON` + `WMR_BUILD_AI_DENOISE=ON`; on mac `WMR_BUILD_AI_MIGAN` routes to CoreML, elsewhere ORT) + a `tests` job (AI+TESTS ON, ubuntu/ORT); `release` (`needs: [build, tests]`, `if: v*` tag) attaches the 4 packages (mac arm64 tarball, mac x86_64 tarball, linux tarball, windows zip) + `LICENSE-THIRD-PARTY.md`. **Validate a changed job off-cycle via `workflow_dispatch` before tagging** — avoids tag-force-move churn on failure.
- `gh run view --log` returns empty until the *whole run* completes (per-job logs too). To read a finished job's log fast, `gh run cancel` the run (preserves completed jobs' logs), then read — or wait for completion. **Don't monitor a run with `gh run watch --exit-status` in a piped background command** — the pipeline masks gh's exit code (always 0) and gh can drop early on an API hiccup → a false "completed". Poll `gh run view <id> --json status,conclusion` until `status=="completed"`; `conclusion` is the source of truth. (Windows is the ~2 h long pole — every release, even mac-only, waits on it.)
