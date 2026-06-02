#pragma once

#include <opencv2/core.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace wmr {

struct SceneInfo {
    int64_t start_frame = 0;   // inclusive
    int64_t end_frame = 0;     // exclusive (half-open interval)
};

struct SceneDetectorConfig {
    double threshold = 0.30;       // combined distance for hard cut
    int min_scene_length = 15;     // minimum frames between cuts
};

class SceneDetector {
public:
    explicit SceneDetector(SceneDetectorConfig config = {});

    // Opens its own VideoReader internally, scans for scene boundaries.
    // Returns vector of SceneInfo with start_frame/end_frame populated.
    std::vector<SceneInfo> detect_boundaries(const std::string& video_path);

private:
    SceneDetectorConfig config_;

    // Downsample frame (preserves BGR color)
    cv::Mat prepare_frame(const cv::Mat& frame) const;

    // Combined metric: max of per-channel Bhattacharyya and mean absolute difference
    double compute_distance(const cv::Mat& prev, const cv::Mat& curr) const;

    // Merge scenes shorter than min_scene_length into predecessor
    std::vector<SceneInfo> merge_short_scenes(
        std::vector<int64_t>&& boundaries, int64_t total_frames) const;
};

} // namespace wmr
