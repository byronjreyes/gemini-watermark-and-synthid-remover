/**
 * @file    migan_inpainter.hpp
 * @brief   MI-GAN inpainter for NotebookLM watermark removal
 * @license MIT (project). MI-GAN model + code are MIT (Picsart AI Research).
 *
 * @details
 * MI-GAN ("A Simple Baseline for Image Inpainting on Mobile Devices", ICCV 2023,
 * Picsart AI Research) replaces both FSR and LaMa for NotebookLM intricate
 * backgrounds. It is the DEFAULT intricate-scene method — no user flag needed.
 *
 * Platform impls (same interface; selected by CMake):
 *   - **macOS** — `migan_coreml_inpainter.mm`: native CoreML fp16 mlprogram on the
 *     Neural Engine, ~28 ms/frame (A/B-verified to match the ORT baseline within
 *     Δ1.9/255). 512x512 fixed-input crop around the mark + soft-paste. Replaces
 *     ORT entirely on mac (no vendored lib — CoreML is a system framework).
 *   - **Linux/Windows** — `migan_inpainter.cpp`: ONNX Runtime CPU, ~225 ms/frame,
 *     27 MB ONNX, whole-frame uint8 RGB + mask (0=hole) → uint8 result.
 *
 * The inpainter (ORT session / CoreML MLModel) is a process singleton,
 * intentionally leaked — teardown races global state during static destruction
 * (same rationale as the NCNN denoiser). The dispatch + leaked singleton in
 * `video_processor.cpp` are identical for both impls.
 */

#pragma once
#ifdef WMR_AI_MIGAN

#include <opencv2/core.hpp>
#include <memory>

namespace wmr {

/**
 * MI-GAN inpainter (CoreML on macOS; ONNX Runtime on Linux/Windows).
 *
 * Thread-safety: NOT thread-safe (single MLModel/Ort::Session). NotebookLM
 * processes frames sequentially, so this is fine.
 */
class MiganInpainter {
public:
    MiganInpainter();
    ~MiganInpainter();

    MiganInpainter(const MiganInpainter&) = delete;
    MiganInpainter& operator=(const MiganInpainter&) = delete;

    /**
     * Load the model. On macOS: compiles + loads the CoreML `.mlpackage` (resolved
     * from $WMR_COREML_MODEL, <exedir>/migan_512_places2_fp16.mlpackage,
     * <exedir>/../share/wmr/...). On Linux/Windows: creates the ORT session
     * (resolved from $WMR_MIGAN_MODEL, <exedir>/migan_pipeline_v2.onnx, ...).
     * Returns false (and logs) if init or model load fails.
     */
    bool initialize();

    [[nodiscard]] bool is_ready() const;

    /**
     * Inpaint the mark (mark_rect) on `frame` in place. macOS: crops a square
     * around the mark to 512x512, runs CoreML, soft-pastes back. Linux/Windows:
     * builds a mask (0 at the mark, dilated 5x5), runs MI-GAN on the whole frame,
     * replaces `frame` with the composited result (identity outside the hole).
     */
    void inpaint_hole(cv::Mat& frame, const cv::Rect& mark_rect);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wmr

#endif // WMR_AI_MIGAN
