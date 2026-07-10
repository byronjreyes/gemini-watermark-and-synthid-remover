#include "video/video_processor.hpp"
#include "video/video_reader.hpp"
#include "video/scene_detector.hpp"
#include "core/watermark_engine.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <filesystem>
#include <vector>

#include <spdlog/spdlog.h>
#include <opencv2/photo.hpp>
#include <fmt/format.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace wmr {

// ---------------------------------------------------------------------------
// Resolve geometry from config + resolution
// ---------------------------------------------------------------------------
WatermarkPosition VideoProcessor::resolve_geometry(
    const VideoWatermarkConfig& config, int width, int height) const
{
    return get_video_watermark_geometry(config.variant, width, height, config.profile);
}

// ---------------------------------------------------------------------------
// Determine WatermarkSize from geometry
// ---------------------------------------------------------------------------
WatermarkSize VideoProcessor::geometry_to_size(const WatermarkPosition& geo) const {
    return (geo.logo_size > 48) ? WatermarkSize::Large : WatermarkSize::Small;
}

// ---------------------------------------------------------------------------
// Shot-level detection: sample frames and establish watermark baseline
// ---------------------------------------------------------------------------
VideoProcessor::ShotDetection VideoProcessor::detect_in_shot(
    VideoReader& reader,
    WatermarkEngine& engine,
    const VideoWatermarkConfig& config,
    int64_t range_start,
    int64_t range_end,
    int max_samples)
{
    ShotDetection result;

    const int64_t total = reader.frame_count();
    if (total <= 0) {
        spdlog::warn("Video has no frames, shot detection skipped");
        return result;
    }

    // Get video-specific geometry
    auto geo = resolve_geometry(config, reader.width(), reader.height());
    auto wsize = geometry_to_size(geo);
    auto default_pos = geo.get_position(reader.width(), reader.height());

    // For Veo legacy, use reference alpha map (non-square: 68x30 or 99x43)
    // For Gemini diamond, use V2 diamond alpha map (36x36 or 96x96)
    const cv::Mat* video_alpha = nullptr;
    if (config.profile == VideoProfile::VeoLegacy) {
        video_alpha = (geo.logo_size > 68)
                      ? &engine.get_veo_text_alpha_large()
                      : &engine.get_veo_text_alpha_small();
    } else {
        video_alpha = (geo.logo_size > 48)
                      ? &engine.get_v2_diamond_alpha_large()
                      : &engine.get_v2_diamond_alpha_small();
    }

    if (video_alpha && !video_alpha->empty()) {
        default_pos = {reader.width() - geo.margin_right - video_alpha->cols,
                       reader.height() - geo.margin_bottom - video_alpha->rows};
        result.region = cv::Rect(default_pos.x, default_pos.y, video_alpha->cols, video_alpha->rows);
    } else {
        result.region = cv::Rect(default_pos.x, default_pos.y, geo.logo_size, geo.logo_size);
    }

    result.position = default_pos;
    result.size = wsize;

    // Determine sample range: use range params if specified, otherwise default coverage
    const int64_t sample_start = (range_start >= 0) ? range_start : 0;
    const int64_t sample_end = (range_end >= 0) ? range_end
        : static_cast<int64_t>(static_cast<double>(total) * kShotCoverage);
    const int64_t coverage_end = std::min(sample_end, total);
    const int samples_available = static_cast<int>(coverage_end - sample_start);

    // Adaptive sample count for scene ranges
    const int sample_count = (range_start >= 0)
        ? std::min(max_samples, std::max(2, samples_available / 30))
        : std::min(max_samples, samples_available);

    if (sample_count <= 0) {
        spdlog::warn("Video too short for shot sampling");
        return result;
    }

    // For a single sample, just check frame 0
    if (sample_count == 1) {
        cv::Mat frame;
        if (reader.seek(0) && reader.next_frame(frame) && !frame.empty()) {
            auto det = engine.detect_watermark(frame, wsize, geo, video_alpha);
            if (det.detected) {
                result.found = true;
                result.position = cv::Point(det.region.x, det.region.y);
                result.size = det.size;
                result.confidence = det.confidence;
                result.region = det.region;
                spdlog::info("Shot detection (single frame): detected at ({},{}) conf={:.2f}",
                             result.position.x, result.position.y, result.confidence);
            }
        }
        return result;
    }

    struct Sample {
        cv::Point position;
        WatermarkSize size;
        float confidence;
        cv::Rect region;
    };

    std::vector<Sample> detections;
    detections.reserve(sample_count);

    spdlog::info("Shot detection: sampling {} frames over first {} of {} total "
                 "(geo: margin={},{} size={})",
                 sample_count, coverage_end, total,
                 geo.margin_right, geo.margin_bottom, geo.logo_size);

    if (range_start >= 0) {
        // --- Scene-range path: sequential decode with PTS verification ---
        // Random seek after backward-flag lands on a keyframe that may be
        // before the scene boundary (e.g. in a concatenated video).  Decode
        // forward and skip frames until PTS confirms we're past range_start.

        auto* stream = reader.format_context()->streams[reader.video_stream_index()];

        auto frame_to_pts = [&](int64_t idx) -> int64_t {
            if (stream->avg_frame_rate.num <= 0 || stream->avg_frame_rate.den <= 0)
                return INT64_MIN;
            return idx *
                   static_cast<int64_t>(stream->time_base.den) *
                   static_cast<int64_t>(stream->avg_frame_rate.den) /
                   (static_cast<int64_t>(stream->time_base.num) *
                    static_cast<int64_t>(stream->avg_frame_rate.num));
        };

        // Pre-compute sample frame indices and their target PTS
        struct Target { int64_t frame_idx; int64_t pts; };
        std::vector<Target> targets;
        targets.reserve(sample_count);
        for (int i = 0; i < sample_count; ++i) {
            int64_t idx = sample_start + static_cast<int64_t>(
                static_cast<double>(i) / static_cast<double>(sample_count - 1) *
                static_cast<double>(coverage_end - sample_start - 1));
            targets.push_back({idx, frame_to_pts(idx)});
        }

        // Seek to first sample point (backward to keyframe)
        reader.seek(targets.front().frame_idx);

        cv::Mat frame;
        size_t next_target = 0;
        // Safety: limit total decoded frames to avoid infinite loop
        int max_decode = static_cast<int>(coverage_end - sample_start + 60);

        while (reader.next_frame(frame) && next_target < targets.size()
               && max_decode-- > 0) {
            if (frame.empty()) continue;

            int64_t cur_pts = reader.last_pts();
            int64_t tgt_pts = targets[next_target].pts;

            // Skip frames until PTS reaches the target
            if (tgt_pts != INT64_MIN && cur_pts != INT64_MIN
                && cur_pts < tgt_pts) {
                continue;
            }

            // Reached (or passed) the target — this is our sample
            auto det = engine.detect_watermark(frame, wsize, geo, video_alpha);
            if (det.detected) {
                detections.push_back({cv::Point(det.region.x, det.region.y),
                                      det.size, det.confidence, det.region});
                spdlog::debug("Scene sample {}: detected at ({},{}) conf={:.2f}",
                              next_target, det.region.x, det.region.y,
                              det.confidence);
            } else {
                spdlog::debug("Scene sample {}: not detected ({:.2f})",
                              next_target, det.confidence);
            }
            ++next_target;
        }

        spdlog::info("Scene-range detection: {}/{} samples collected via sequential decode",
                     detections.size(), sample_count);
    } else {
        // --- Full-video path: random seek per sample (original behavior) ---
        for (int i = 0; i < sample_count; ++i) {
            int64_t frame_idx = sample_start + static_cast<int64_t>(
                static_cast<double>(i) / static_cast<double>(sample_count - 1) *
                static_cast<double>(coverage_end - sample_start - 1));

            if (!reader.seek(frame_idx)) {
                spdlog::debug("Shot sample {}: seek to {} failed", i, frame_idx);
                continue;
            }

            cv::Mat frame;
            if (!reader.next_frame(frame) || frame.empty()) {
                spdlog::debug("Shot sample {}: read at {} failed", i, frame_idx);
                continue;
            }

            auto det = engine.detect_watermark(frame, wsize, geo, video_alpha);
            if (det.detected) {
                detections.push_back({cv::Point(det.region.x, det.region.y),
                                      det.size, det.confidence, det.region});
                spdlog::debug("Shot sample {}: detected at ({},{}) conf={:.2f}",
                              i, det.region.x, det.region.y, det.confidence);
            } else {
                spdlog::debug("Shot sample {}: not detected ({:.2f})",
                              i, det.confidence);
            }
        }
    }

    // Need >50% detection rate to trust the result
    const int threshold = (sample_count + 1) / 2;
    if (static_cast<int>(detections.size()) < threshold) {
        spdlog::info("Shot detection: {}/{} samples detected (< {} majority threshold)",
                     detections.size(), sample_count, threshold);
        result.found = false;
        result.confidence = 0.0f;
        return result;
    }

    // Compute median of detected positions, sizes, and confidences
    std::vector<int> xs, ys;
    std::vector<float> confs;
    int small_count = 0;
    cv::Rect median_region;

    for (const auto& d : detections) {
        xs.push_back(d.position.x);
        ys.push_back(d.position.y);
        confs.push_back(d.confidence);
        if (d.size == WatermarkSize::Small) ++small_count;
    }

    const auto mid = xs.size() / 2;

    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    std::sort(confs.begin(), confs.end());

    result.position = cv::Point(xs[mid], ys[mid]);
    result.confidence = confs[mid];

    // Use the region from the detection closest to median position
    int best_dist = std::numeric_limits<int>::max();
    for (const auto& d : detections) {
        int dist = std::abs(d.position.x - result.position.x) +
                   std::abs(d.position.y - result.position.y);
        if (dist < best_dist) {
            best_dist = dist;
            median_region = d.region;
        }
    }
    result.region = median_region;

    // Majority size
    result.size = (small_count > static_cast<int>(detections.size()) / 2)
                      ? WatermarkSize::Small : WatermarkSize::Large;
    result.found = true;

    spdlog::info("Shot detection: anchor at ({},{}) size={} conf={:.2f} "
                 "({}/{} detections)",
                 result.position.x, result.position.y,
                 result.size == WatermarkSize::Small ? "small" : "large",
                 result.confidence, detections.size(), sample_count);

    return result;
}

// ---------------------------------------------------------------------------
// Main processing loop
// ---------------------------------------------------------------------------
VideoResult VideoProcessor::process(const std::string& input_path,
                                    const std::string& output_path,
                                    const VideoWatermarkConfig& config,
                                    const EncodeOptions& encode_opts)
{
    VideoResult result;
    const auto t_start = std::chrono::steady_clock::now();

    // NotebookLM uses a separate path (temporal detection + NS inpaint)
    if (config.profile == VideoProfile::NotebookLM) {
        return process_notebooklm(input_path, output_path, config, encode_opts);
    }

    // Open input
    VideoReader reader;
    if (!reader.open(input_path)) {
        result.success = false;
        result.message = fmt::format("Failed to open input: {}", input_path);
        spdlog::error("{}", result.message);
        return result;
    }

    spdlog::info("Input: {} ({}x{}, {:.2f} fps, {} frames)",
                 input_path, reader.width(), reader.height(),
                 reader.fps(), reader.frame_count());

    WatermarkEngine engine;

    // Resolve video-specific geometry
    auto geo = resolve_geometry(config, reader.width(), reader.height());
    auto wsize = geometry_to_size(geo);

    // Select alpha map based on profile and resolution
    const cv::Mat* video_alpha = nullptr;
    if (config.profile == VideoProfile::VeoLegacy) {
        video_alpha = (geo.logo_size > 68)
                      ? &engine.get_veo_text_alpha_large()
                      : &engine.get_veo_text_alpha_small();
    } else {
        video_alpha = (geo.logo_size > 48)
                      ? &engine.get_v2_diamond_alpha_large()
                      : &engine.get_v2_diamond_alpha_small();
    }

    // Shot-level detection
    ShotDetection shot;

    if (config.scenes) {
        // ---- Scene splitting: detect boundaries, split into separate files ----

        // Detect scene boundaries using SceneDetector's own reader
        spdlog::info("Scanning for scene boundaries...");
        SceneDetector detector({config.scene_threshold});
        auto scenes = detector.detect_boundaries(input_path);

        if (scenes.empty()) {
            spdlog::warn("Scene detection returned no results, treating as single scene");
            scenes.push_back({0, reader.frame_count()});
        }

        spdlog::info("Detected {} scene(s)", scenes.size());

        // Single full-video watermark detection
        if (config.force) {
            cv::Point default_pos;
            if (video_alpha && !video_alpha->empty()) {
                default_pos = {static_cast<int>(reader.width()) - geo.margin_right - video_alpha->cols,
                               static_cast<int>(reader.height()) - geo.margin_bottom - video_alpha->rows};
                shot.region = cv::Rect(default_pos.x, default_pos.y, video_alpha->cols, video_alpha->rows);
            } else {
                default_pos = geo.get_position(reader.width(), reader.height());
                shot.region = cv::Rect(default_pos.x, default_pos.y, geo.logo_size, geo.logo_size);
            }
            shot.found = false;
            shot.position = default_pos;
            shot.size = wsize;
            shot.confidence = 1.0f;
            spdlog::info("Force mode: using position ({},{}) size={}",
                         shot.position.x, shot.position.y, geo.logo_size);
        } else {
            shot = detect_in_shot(reader, engine, config);
        }

        reader.seek(0);

        // Output naming: <stem>_<NNN>.mp4
        namespace fs = std::filesystem;
        const std::string stem = fs::path(input_path).stem().string();
        const fs::path out_dir(output_path);
        const int num_scenes = static_cast<int>(scenes.size());
        const int pad_width = std::max(3, static_cast<int>(
            std::ceil(std::log10(static_cast<double>(num_scenes) + 1))));

        // Process each scene sequentially (reader stays in position between scenes)
        for (int si = 0; si < num_scenes; ++si) {
            const auto& scene = scenes[si];
            const int64_t scene_frames = scene.end_frame - scene.start_frame;
            const double start_sec = static_cast<double>(scene.start_frame) / reader.fps();
            const double end_sec = static_cast<double>(scene.end_frame) / reader.fps();

            const std::string out_name = fmt::format("{}_{:0{}}.mp4", stem, si + 1, pad_width);
            const fs::path out_path = out_dir / out_name;

            spdlog::info("Scene {}/{}: {} (frames [{},{}), {:.2f}s)",
                         si + 1, num_scenes, out_name,
                         scene.start_frame, scene.end_frame,
                         static_cast<double>(scene_frames) / reader.fps());

            // Create writer for this scene
            VideoWriter writer;
            if (!writer.open(out_path.string(), reader.width(), reader.height(),
                             reader.fps(), encode_opts, input_path)) {
                spdlog::error("Failed to open output for scene {}: {}",
                              si + 1, out_path.string());
                result.success = false;
                result.message = fmt::format("Failed to open output for scene {}", si + 1);
                reader.close();
                return result;
            }

            // Process frames for this scene (sequential decode, no seeking)
            cv::Mat frame;
            int64_t scene_processed = 0;
            for (int64_t fi = scene.start_frame; fi < scene.end_frame; ++fi) {
                if (!reader.next_frame(frame)) {
                    spdlog::warn("Scene {}: unexpected EOF at frame {}", si + 1, fi);
                    break;
                }

                if (frame.empty()) {
                    spdlog::warn("Scene {}: frame {} empty, skipping", si + 1, fi);
                    ++result.frames_skipped;
                    continue;
                }

                if (frame.cols != reader.width() || frame.rows != reader.height()) {
                    spdlog::warn("Scene {}: frame {} unexpected dimensions {}x{}",
                                 si + 1, fi, frame.cols, frame.rows);
                    ++result.frames_skipped;
                    continue;
                }

                // Apply watermark removal (same logic as non-scene branch)
                if (config.force) {
                    DetectionResult det;
                    det.detected = true;
                    det.confidence = 1.0f;
                    det.region = shot.region;
                    det.size = shot.size;
                    engine.remove_watermark_alpha_only(frame, det, video_alpha);
                } else {
                    DetectionResult det;
                    det.detected = true;
                    det.confidence = shot.confidence;
                    det.region = shot.region;
                    det.size = shot.size;

                    auto detection = engine.detect_watermark(frame, wsize, geo, video_alpha);
                    if (detection.detected &&
                        detection.confidence >= kOcclusionGateNcc) {
                        cv::Point detected_pos(detection.region.x, detection.region.y);
                        int dx = std::abs(detected_pos.x - shot.position.x);
                        int dy = std::abs(detected_pos.y - shot.position.y);
                        if (dx <= kPositionTolerancePx && dy <= kPositionTolerancePx) {
                            det.region = detection.region;
                            det.size = detection.size;
                        }
                    }

                    engine.remove_watermark_alpha_only(frame, det, video_alpha);
                }

                writer.write_frame(frame);
                ++scene_processed;
                ++result.frames_processed;
            }

            // Copy audio for this scene's time range
            if (!writer.copy_audio_range(start_sec, end_sec)) {
                spdlog::warn("Scene {}: audio range copy failed", si + 1);
            }

            writer.close();

            double scene_dur = static_cast<double>(scene_processed) / reader.fps();
            spdlog::info("Scene {}/{}: {} done ({} frames, {:.2f}s)",
                         si + 1, num_scenes, out_name, scene_processed, scene_dur);
        }

        reader.close();

    } else {
        // ---- Standard single-shot processing (original behavior) ----

        if (config.force) {
            cv::Point default_pos;
            if (video_alpha && !video_alpha->empty()) {
                default_pos = {static_cast<int>(reader.width()) - geo.margin_right - video_alpha->cols,
                               static_cast<int>(reader.height()) - geo.margin_bottom - video_alpha->rows};
                shot.region = cv::Rect(default_pos.x, default_pos.y, video_alpha->cols, video_alpha->rows);
            } else {
                default_pos = geo.get_position(reader.width(), reader.height());
                shot.region = cv::Rect(default_pos.x, default_pos.y, geo.logo_size, geo.logo_size);
            }
            shot.found = false;
            shot.position = default_pos;
            shot.size = wsize;
            shot.confidence = 1.0f;
            spdlog::info("Force mode: using position ({},{}) size={}",
                         shot.position.x, shot.position.y, geo.logo_size);
        } else {
            shot = detect_in_shot(reader, engine, config);
            reader.seek(0);
        }

        // Open output writer (audio streams set up before MP4 header)
        VideoWriter writer;
        if (!writer.open(output_path, reader.width(), reader.height(),
                         reader.fps(), encode_opts, input_path)) {
            result.success = false;
            result.message = fmt::format("Failed to open output: {}", output_path);
            spdlog::error("{}", result.message);
            reader.close();
            return result;
        }

        // Process frames
        cv::Mat frame;
        int64_t frame_idx = 0;
        int64_t last_progress_frame = 0;
        auto t_last_progress = t_start;

        while (reader.next_frame(frame)) {
            if (frame.empty()) {
                spdlog::warn("Frame {}: empty, skipping", frame_idx);
                writer.write_frame(frame);
                ++result.frames_skipped;
                ++frame_idx;
                continue;
            }

            // Guard against corrupt frames from seek artifacts
            if (frame.cols != reader.width() || frame.rows != reader.height()) {
                spdlog::warn("Frame {}: unexpected dimensions {}x{} (expected {}x{}), skipping",
                             frame_idx, frame.cols, frame.rows,
                             reader.width(), reader.height());
                ++result.frames_skipped;
                ++frame_idx;
                continue;
            }

            if (config.force) {
                // Pure reverse alpha blend — no inpaint to avoid blur
                DetectionResult det;
                det.detected = true;
                det.confidence = 1.0f;
                det.region = shot.region;
                det.size = shot.size;
                engine.remove_watermark_alpha_only(frame, det, video_alpha);
                writer.write_frame(frame);
                ++result.frames_processed;
            } else {
                // Shot detection confirmed the watermark — process every frame.
                // Use per-frame detection for position refinement when available,
                // fall back to shot anchor when detection fails (occluded frames).
                DetectionResult det;
                det.detected = true;
                det.confidence = shot.confidence;
                det.region = shot.region;
                det.size = shot.size;

                auto detection = engine.detect_watermark(frame, wsize, geo, video_alpha);
                if (detection.detected &&
                    detection.confidence >= kOcclusionGateNcc) {
                    // Per-frame detection succeeded — refine position
                    cv::Point detected_pos(detection.region.x, detection.region.y);
                    int dx = std::abs(detected_pos.x - shot.position.x);
                    int dy = std::abs(detected_pos.y - shot.position.y);

                    if (dx <= kPositionTolerancePx && dy <= kPositionTolerancePx) {
                        det.region = detection.region;
                        det.size = detection.size;
                    }
                }

                engine.remove_watermark_alpha_only(frame, det, video_alpha);
                writer.write_frame(frame);
                ++result.frames_processed;
            }

            // Progress output
            ++frame_idx;
            auto t_now = std::chrono::steady_clock::now();
            double since_last = std::chrono::duration<double>(t_now - t_last_progress).count();

            if (frame_idx - last_progress_frame >= kProgressIntervalFrames ||
                since_last >= 2.0) {
                double elapsed = std::chrono::duration<double>(t_now - t_start).count();
                double proc_fps = static_cast<double>(frame_idx) / std::max(elapsed, 1e-9);
                int64_t remaining = reader.frame_count() - frame_idx;
                double eta = (remaining > 0) ? (static_cast<double>(remaining) / proc_fps) : 0.0;

                spdlog::info("Frame {}/{} ({:.1f} fps, ETA {:.0f}s)",
                             frame_idx, reader.frame_count(), proc_fps, eta);

                last_progress_frame = frame_idx;
                t_last_progress = t_now;
            }
        }

        // Copy audio
        if (!writer.copy_audio()) {
            spdlog::warn("Audio copy failed or no audio stream present");
        }

        writer.close();
        reader.close();
    }

    // Build result
    const auto t_end = std::chrono::steady_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t_end - t_start).count();
    result.detection_confidence = shot.confidence;
    result.success = true;
    result.message = fmt::format("Processed {} frames ({} skipped) in {:.1f}s",
                                 result.frames_processed, result.frames_skipped,
                                 result.elapsed_seconds);

    spdlog::info("Done: {} processed, {} skipped, detection conf={:.2f}, "
                 "elapsed={:.1f}s",
                 result.frames_processed, result.frames_skipped,
                 result.detection_confidence, result.elapsed_seconds);

    return result;
}


// ---------------------------------------------------------------------------
// NotebookLM processing: detect mark bbox, then per-frame NS inpaint
// ---------------------------------------------------------------------------
VideoResult VideoProcessor::process_notebooklm(const std::string& input_path,
                                              const std::string& output_path,
                                              const VideoWatermarkConfig& config,
                                              const EncodeOptions& encode_opts)
{
    VideoResult result;
    const auto t_start = std::chrono::steady_clock::now();

    // Detect the watermark bbox
    NotebookLMDetector detector;
    auto det = detector.detect(input_path, config.notebooklm_rect);

    if (!det.found) {
        result.success = false;
        result.message = "NotebookLM watermark not detected (use --rect x,y,w,h to specify manually)";
        spdlog::error("{}", result.message);
        return result;
    }

    spdlog::info("NotebookLM watermark bbox: ({},{},{},{})",
                 det.bbox.x, det.bbox.y, det.bbox.width, det.bbox.height);

    // Open input
    VideoReader reader;
    if (!reader.open(input_path)) {
        result.success = false;
        result.message = fmt::format("Failed to open input: {}", input_path);
        spdlog::error("{}", result.message);
        return result;
    }

    spdlog::info("Input: {} ({}x{}, {:.2f} fps, {} frames)",
                 input_path, reader.width(), reader.height(),
                 reader.fps(), reader.frame_count());

    // Open output writer
    VideoWriter writer;
    if (!writer.open(output_path, reader.width(), reader.height(),
                     reader.fps(), encode_opts, input_path)) {
        result.success = false;
        result.message = fmt::format("Failed to open output: {}", output_path);
        spdlog::error("{}", result.message);
        reader.close();
        return result;
    }

    // Create binary mask for the watermark bbox (dilated slightly for clean edges)
    cv::Rect mask_rect = det.bbox;
    // Clamp to frame bounds
    mask_rect.x = std::max(0, mask_rect.x);
    mask_rect.y = std::max(0, mask_rect.y);
    mask_rect.width = std::min(mask_rect.width, reader.width() - mask_rect.x);
    mask_rect.height = std::min(mask_rect.height, reader.height() - mask_rect.y);

    // NS inpaint radius: proportional to bbox height, clamped [3, 8].
    // (Validated at radius 4 for 720p marks; scales modestly for larger marks.)
    const int inpaint_radius = std::clamp(mask_rect.height / 4, 3, 8);

    // Precompute the inpaint mask once (bbox region, dilated ~2px to cover the
    // semi-transparent mark's anti-aliased fringe) and reuse it for every frame.
    cv::Mat mask = cv::Mat::zeros(reader.height(), reader.width(), CV_8UC1);
    mask(mask_rect).setTo(255);
    cv::dilate(mask, mask, cv::Mat::ones(5, 5, CV_8U), cv::Point(-1, -1), 1);

    // Process frames
    cv::Mat frame;
    int64_t frame_idx = 0;
    int64_t last_progress_frame = 0;
    auto t_last_progress = t_start;

    while (reader.next_frame(frame)) {
        if (frame.empty()) {
            spdlog::warn("Frame {}: empty, skipping", frame_idx);
            writer.write_frame(frame);
            ++result.frames_skipped;
            ++frame_idx;
            continue;
        }

        if (frame.cols != reader.width() || frame.rows != reader.height()) {
            spdlog::warn("Frame {}: unexpected dimensions {}x{}, skipping",
                         frame_idx, frame.cols, frame.rows);
            ++result.frames_skipped;
            ++frame_idx;
            continue;
        }

        // Apply NS inpaint (mask precomputed above the loop)
        cv::Mat inpainted;
        cv::inpaint(frame, mask, inpainted, inpaint_radius, cv::INPAINT_NS);
        frame = inpainted;

        writer.write_frame(frame);
        ++result.frames_processed;

        // Progress output
        ++frame_idx;
        auto t_now = std::chrono::steady_clock::now();
        double since_last = std::chrono::duration<double>(t_now - t_last_progress).count();

        if (frame_idx - last_progress_frame >= kProgressIntervalFrames ||
            since_last >= 2.0) {
            double elapsed = std::chrono::duration<double>(t_now - t_start).count();
            double proc_fps = static_cast<double>(frame_idx) / std::max(elapsed, 1e-9);
            int64_t remaining = reader.frame_count() - frame_idx;
            double eta = (remaining > 0) ? (static_cast<double>(remaining) / proc_fps) : 0.0;

            spdlog::info("Frame {}/{} ({:.1f} fps, ETA {:.0f}s)",
                         frame_idx, reader.frame_count(), proc_fps, eta);

            last_progress_frame = frame_idx;
            t_last_progress = t_now;
        }
    }

    // Copy audio
    if (!writer.copy_audio()) {
        spdlog::warn("Audio copy failed or no audio stream present");
    }

    writer.close();
    reader.close();

    // Build result
    const auto t_end = std::chrono::steady_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t_end - t_start).count();
    result.detection_confidence = det.confidence;
    result.success = true;
    result.message = fmt::format("NotebookLM: processed {} frames ({} skipped) in {:.1f}s",
                                 result.frames_processed, result.frames_skipped,
                                 result.elapsed_seconds);

    spdlog::info("Done: {} processed, {} skipped, elapsed={:.1f}s",
                 result.frames_processed, result.frames_skipped,
                 result.elapsed_seconds);

    return result;
}

} // namespace wmr
