/**
 * @file    lama_inpainter.hpp
 * @brief   LaMa (big-lama) ONNX inpainter for NotebookLM intricate backgrounds
 * @license MIT (project); the LaMa model weights are license-gray — see
 *          LICENSE-THIRD-PARTY.md.
 *
 * @details
 * Phase C. FSR/NS blur or smear the hardest NotebookLM backgrounds (complexity
 * >= ~60); LaMa is the only method that reconstructs them sharply. It is too
 * slow to be a default (~2.4 s/frame CPU; CoreML does not help — the fp32 model
 * does not compile to Metal well), so it ships complexity-gated: only the
 * hardest scenes pay it, and only when the user opts in via
 * `--notebooklm-method lama`. Model: Carve/LaMa-ONNX `lama_fp32.onnx`
 * (fixed 512x512 input).
 *
 * The model file is resolved at runtime relative to the executable (or
 * $WMR_LAMA_MODEL). Like the NCNN denoiser, the Ort::Session is held by a
 * process singleton that is intentionally leaked — destroying it during C++
 * static teardown races ONNX Runtime's global state.
 */

#pragma once
#ifdef WMR_AI_LAMA

#include <opencv2/core.hpp>
#include <memory>

namespace wmr {

/**
 * LaMa inpainter (ONNX Runtime, CPU).
 *
 * Thread-safety: NOT thread-safe (single Ort::Session). NotebookLM processes
 * frames sequentially, so this is fine.
 */
class LamaInpainter {
public:
    LamaInpainter();
    ~LamaInpainter();

    LamaInpainter(const LamaInpainter&) = delete;
    LamaInpainter& operator=(const LamaInpainter&) = delete;

    /**
     * Create the ORT session and load the model. Resolves the model from, in
     * order: $WMR_LAMA_MODEL, <exedir>/lama_fp32.onnx,
     * <exedir>/../share/wmr/lama_fp32.onnx. Returns false (and logs) if ORT init
     * or model load fails.
     */
    bool initialize();

    [[nodiscard]] bool is_ready() const;

    /**
     * Inpaint the mark (mark_rect) on `frame` in place. Builds a 512x512
     * bottom-right-aligned crop (LaMa's fixed input), runs the model on CPU, and
     * pastes the reconstructed region back. The mark is assumed to sit in the
     * bottom-right corner of the frame (true for NotebookLM exports).
     */
    void inpaint_hole(cv::Mat& frame, const cv::Rect& mark_rect);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wmr

#endif // WMR_AI_LAMA
