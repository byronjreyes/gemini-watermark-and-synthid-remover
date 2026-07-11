#pragma once

#include <opencv2/core.hpp>

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

} // namespace wmr
