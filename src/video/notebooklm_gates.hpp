#pragma once

#include <opencv2/core.hpp>
#include <string>

namespace wmr {

// Per-scene gate logic for NotebookLM adaptive inpaint dispatch. Pure logic
// (operates on cv::Mat, no VideoReader/FFmpeg) so it can be unit-tested without
// linking FFmpeg (which the test target excludes).

// Background complexity score in a band AROUND the mark (the mark bbox itself
// is excluded, since the watermark strokes would inflate the gradient energy).
// Higher = more intricate/textured background. Based on mean Sobel gradient
// magnitude in the surrounding ring.
float background_complexity_score(const cv::Mat& gray_frame, const cv::Rect& mark_rect);

// Convenience: true when the background around the mark is intricate enough to
// warrant a stronger inpainter than plain NS.
bool background_is_intricate(const cv::Mat& gray_frame, const cv::Rect& mark_rect,
                             float threshold);

// Resolve the per-scene inpaint method from the requested option + the scene's
// background complexity. Pure (no VideoReader) so it can be unit-tested without
// linking FFmpeg.
//   complexity:      scene background-complexity score
//   threshold:       fsr/complexity threshold — intricate (-> FSR) when
//                    complexity >= threshold (--complexity-threshold, default 15)
//   lama_threshold:  LaMa threshold — only the HARDEST scenes (complexity >=
//                    lama_threshold, --lama-threshold default 60) use LaMa
//   requested:       "auto" (FSR/NS, never LaMa) | "ns" | "fsr" | "lama"
//   has_xphoto:      whether the build compiled opencv_contrib xphoto
//                    (WMR_HAS_XPHOTO); passed as a bool so the function is
//                    testable in either build configuration.
//   has_lama:        whether the build compiled the LaMa inpainter
//                    (WMR_AI_LAMA); same testability rationale.
// "auto" never routes to LaMa (~2.4 s/frame CPU — infeasible as a default); LaMa
// runs only on explicit "--notebooklm-method lama" AND the hardest scenes AND
// when compiled in. Otherwise it falls through to FSR/NS. "ns" is the universal
// fallback.
std::string resolve_inpaint_method(float complexity, double threshold,
                                   double lama_threshold,
                                   const std::string& requested,
                                   bool has_xphoto, bool has_lama);

} // namespace wmr
