/**
 * @file    migan_inpainter.hpp
 * @brief   MI-GAN inpainter (ONNX Runtime) for NotebookLM watermark removal
 * @license MIT (project). MI-GAN model + code are MIT (Picsart AI Research).
 *
 * @details
 * MI-GAN ("A Simple Baseline for Image Inpainting on Mobile Devices", ICCV 2023,
 * Picsart AI Research) replaces both FSR and LaMa for NotebookLM intricate
 * backgrounds. It is ~225 ms/frame on CPU (vs LaMa ~1.8 s), 27 MB (vs LaMa
 * 208 MB), MIT-licensed (vs LaMa's Places2 gray-area weights), and sharper on
 * cartoon/line-art (GAN vs LaMa's L1/perceptual softness). It is the DEFAULT
 * intricate-scene method — no user flag needed.
 *
 * IO convention (verified empirically, not assumed): image = RGB **uint8** NCHW,
 * mask = uint8 NHW with **0 = hole / 255 = keep**, output "result" = uint8 NCHW
 * composited image (input with the hole filled — identity outside the hole).
 * Dynamic resolution (no fixed tile, no float scaling) — feed the whole frame.
 *
 * The Ort::Session is a process singleton, intentionally leaked (ORT teardown
 * races global state during static destruction, same as the NCNN denoiser).
 */

#pragma once
#ifdef WMR_AI_MIGAN

#include <opencv2/core.hpp>
#include <memory>

namespace wmr {

/**
 * MI-GAN inpainter (ONNX Runtime, CPU).
 *
 * Thread-safety: NOT thread-safe (single Ort::Session). NotebookLM processes
 * frames sequentially, so this is fine.
 */
class MiganInpainter {
public:
    MiganInpainter();
    ~MiganInpainter();

    MiganInpainter(const MiganInpainter&) = delete;
    MiganInpainter& operator=(const MiganInpainter&) = delete;

    /**
     * Create the ORT session + load the model. Resolves the model from, in order:
     * $WMR_MIGAN_MODEL, <exedir>/migan_pipeline_v2.onnx, <exedir>/../share/wmr/...
     * Returns false (and logs) if ORT init or model load fails.
     */
    bool initialize();

    [[nodiscard]] bool is_ready() const;

    /**
     * Inpaint the mark (mark_rect) on `frame` in place. Builds a mask (0 at the
     * mark, 255 elsewhere, dilated 5x5), runs MI-GAN on the whole frame, and
     * replaces `frame` with MI-GAN's composited result (identity outside the
     * hole). MI-GAN is mobile-optimized (~225 ms at 720p) so the full-frame path
     * is fine for NotebookLM resolutions.
     */
    void inpaint_hole(cv::Mat& frame, const cv::Rect& mark_rect);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wmr

#endif // WMR_AI_MIGAN
