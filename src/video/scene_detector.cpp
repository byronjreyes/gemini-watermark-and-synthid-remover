#include "video/scene_detector.hpp"
#include "video/video_reader.hpp"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace wmr {

SceneDetector::SceneDetector(SceneDetectorConfig config)
    : config_(config) {}

cv::Mat SceneDetector::prepare_frame(const cv::Mat& frame) const {
    int max_dim = std::max(frame.cols, frame.rows);
    if (max_dim <= 320) {
        return frame.clone();
    }

    double scale = 320.0 / static_cast<double>(max_dim);
    cv::Mat small;
    cv::resize(frame, small, {}, scale, scale, cv::INTER_AREA);
    return small;
}

double SceneDetector::compute_distance(const cv::Mat& prev, const cv::Mat& curr) const {
    constexpr int kBins = 64;
    constexpr float kRange[] = {0.0f, 256.0f};
    const float* kRanges = kRange;

    // Per-channel Bhattacharyya, take max across BGR channels
    double max_bhatt = 0.0;
    const int channels = std::min(prev.channels(), 3);

    for (int c = 0; c < channels; ++c) {
        cv::Mat prev_ch, curr_ch;
        cv::extractChannel(prev, prev_ch, c);
        cv::extractChannel(curr, curr_ch, c);

        cv::Mat hist_prev, hist_curr;
        cv::calcHist(&prev_ch, 1, nullptr, cv::Mat(), hist_prev, 1, &kBins, &kRanges);
        cv::calcHist(&curr_ch, 1, nullptr, cv::Mat(), hist_curr, 1, &kBins, &kRanges);

        cv::normalize(hist_prev, hist_prev, 1.0, 0.0, cv::NORM_L1);
        cv::normalize(hist_curr, hist_curr, 1.0, 0.0, cv::NORM_L1);

        double dist = cv::compareHist(hist_prev, hist_curr, cv::HISTCMP_BHATTACHARYYA);
        max_bhatt = std::max(max_bhatt, dist);
    }

    // Mean absolute pixel difference (normalized to [0, 1])
    cv::Mat diff;
    cv::absdiff(prev, curr, diff);
    double mad = cv::mean(diff)[0] / 255.0;

    return std::max(max_bhatt, mad);
}

std::vector<SceneInfo> SceneDetector::merge_short_scenes(
    std::vector<int64_t>&& boundaries, int64_t total_frames) const
{
    if (boundaries.empty()) {
        // No cuts — entire video is one scene
        SceneInfo scene;
        scene.start_frame = 0;
        scene.end_frame = total_frames;
        return {scene};
    }

    // Convert boundary indices to half-open intervals
    std::vector<SceneInfo> scenes;
    scenes.reserve(boundaries.size() + 1);

    int64_t prev = 0;
    for (int64_t b : boundaries) {
        SceneInfo s;
        s.start_frame = prev;
        s.end_frame = b;
        scenes.push_back(s);
        prev = b;
    }
    // Last scene goes to end
    SceneInfo last;
    last.start_frame = prev;
    last.end_frame = total_frames;
    scenes.push_back(last);

    // Merge scenes shorter than min_scene_length into predecessor
    std::vector<SceneInfo> merged;
    merged.reserve(scenes.size());

    for (auto& s : scenes) {
        int64_t length = s.end_frame - s.start_frame;
        if (length < config_.min_scene_length && !merged.empty()) {
            // Merge into previous scene
            merged.back().end_frame = s.end_frame;
        } else {
            merged.push_back(s);
        }
    }

    return merged;
}

std::vector<SceneInfo> SceneDetector::detect_boundaries(const std::string& video_path) {
    VideoReader reader;
    if (!reader.open(video_path)) {
        spdlog::warn("Scene detector: failed to open {}", video_path);
        return {};
    }

    const int64_t total = reader.frame_count();
    if (total <= 0) {
        spdlog::debug("Scene detector: no frames in video");
        reader.close();
        return {};
    }

    // Short videos: no scene detection needed
    if (total < config_.min_scene_length * 2) {
        spdlog::debug("Scene detector: video too short for scene detection ({} frames)", total);
        reader.close();
        SceneInfo scene;
        scene.start_frame = 0;
        scene.end_frame = total;
        return {scene};
    }

    spdlog::info("Scene detection: scanning {} frames (threshold={:.2f})...",
                 total, config_.threshold);

    std::vector<int64_t> boundaries;
    cv::Mat frame, prev_prepared;
    int64_t frame_idx = 0;

    // Read first frame
    if (!reader.next_frame(frame) || frame.empty()) {
        spdlog::warn("Scene detector: failed to read first frame");
        reader.close();
        SceneInfo scene;
        scene.start_frame = 0;
        scene.end_frame = total;
        return {scene};
    }

    prev_prepared = prepare_frame(frame);
    ++frame_idx;

    while (reader.next_frame(frame)) {
        if (frame.empty()) {
            ++frame_idx;
            continue;
        }

        cv::Mat curr_prepared = prepare_frame(frame);

        double dist = compute_distance(prev_prepared, curr_prepared);

        if (dist > config_.threshold) {
            // Enforce minimum distance from previous boundary
            int64_t last_boundary = boundaries.empty() ? 0 : boundaries.back();
            if (frame_idx - last_boundary >= config_.min_scene_length) {
                boundaries.push_back(frame_idx);
                spdlog::debug("Scene boundary at frame {} (distance={:.3f})",
                              frame_idx, dist);
            }
        }

        prev_prepared = std::move(curr_prepared);
        ++frame_idx;
    }

    reader.close();

    auto scenes = merge_short_scenes(std::move(boundaries), total);

    spdlog::info("Scene detection: found {} scene(s) in {} frames",
                 scenes.size(), total);

    return scenes;
}

} // namespace wmr
