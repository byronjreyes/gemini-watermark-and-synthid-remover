#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <string>

namespace wmr {

// Result of the NotebookLM watermark auto-detection.
struct NotebookLMDetection {
    bool found = false;
    cv::Rect bbox;           // detected watermark bounding box
    float confidence = 0.0f;
};

// Detect the NotebookLM watermark in a video by sampling frames and looking
// for a static semi-transparent mark in the bottom-right corner.
// Uses temporal median + local contrast + Otsu threshold + connected component
// filtering (ported from the validated Python prototype).
//
// manual_rect: optional user-provided rect (from --rect x,y,w,h). When set,
//               skips auto-detection entirely and returns it as-is.
class NotebookLMDetector {
public:
    // Detect the NotebookLM mark bbox.
    // input_path: video file to sample
    // manual_rect: if set, use this directly (skip detection)
    NotebookLMDetection detect(const std::string& input_path,
                               std::optional<cv::Rect> manual_rect = std::nullopt);
};

} // namespace wmr
