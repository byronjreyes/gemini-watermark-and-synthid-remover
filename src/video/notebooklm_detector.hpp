#pragma once

#include <opencv2/core.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace wmr {

class VideoReader;  // forward declaration (defined in video/video_reader.hpp)

// Result of the NotebookLM watermark auto-detection.
struct NotebookLMDetection {
    bool found = false;
    cv::Rect bbox;           // detected watermark bounding box
    float confidence = 0.0f;
};

// Detect the NotebookLM watermark (rainbow logo + "NotebookLM" wordmark) in a
// video by template matching: multi-scale |TM_CCOEFF_NORMED| against each
// sampled frame, keep the best (polarity-invariant; robust across scene cuts).
// The detected bbox snaps to user-measured exact coordinates per known export
// mode. See notebooklm_detector.cpp for the algorithm and `kKnownModes`.
//
// manual_rect: optional user-provided rect (from --rect x,y,w,h). When set,
//               skips auto-detection entirely and returns it as-is.
class NotebookLMDetector {
public:
    // Detect the NotebookLM mark bbox across the whole video.
    // input_path: video file to sample
    // manual_rect: if set, use this directly (skip detection)
    NotebookLMDetection detect(const std::string& input_path,
                               std::optional<cv::Rect> manual_rect = std::nullopt);

    // Per-scene presence gate: sample frames in [start, end) and test whether
    // the mark is present (template match confidence >= thr). Returns false +
    // best_conf_out < thr when the mark is absent (so the caller can skip
    // inpainting that scene). Uses the embedded mark template.
    bool mark_present_in_scene(VideoReader& reader, int64_t start, int64_t end,
                               float thr, float& best_conf_out);

    // Per-scene background complexity: sample a frame near the middle of
    // [start, end) and return the gradient-energy complexity score around the
    // mark (delegates to background_complexity_score in notebooklm_gates).
    float background_complexity(VideoReader& reader, int64_t start, int64_t end,
                                const cv::Rect& mark_rect);
};

} // namespace wmr
