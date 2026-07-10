#include "video/notebooklm_detector.hpp"
#include "video/video_reader.hpp"
#include "embedded_assets.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <spdlog/spdlog.h>

namespace wmr {

// ---------------------------------------------------------------------------
// Sample evenly-spaced frames across the first 90% of the video (grayscale).
// ---------------------------------------------------------------------------
static std::vector<cv::Mat> sample_grayscale_frames(VideoReader& reader, int sample_count) {
    const int64_t total = reader.frame_count();
    const int64_t coverage_end = static_cast<int64_t>(static_cast<double>(total) * 0.9);

    std::vector<cv::Mat> frames;
    frames.reserve(sample_count);
    for (int i = 0; i < sample_count; ++i) {
        int64_t idx = (sample_count <= 1) ? 0 : static_cast<int64_t>(
            static_cast<double>(i) / static_cast<double>(sample_count - 1) *
            static_cast<double>(coverage_end - 1));
        if (!reader.seek(idx)) continue;
        cv::Mat frame;
        if (!reader.next_frame(frame) || frame.empty()) continue;
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        frames.push_back(gray);
    }
    return frames;
}

// ---------------------------------------------------------------------------
// Embedded mark template (98x14 grayscale) + user-measured exact coordinates.
// ---------------------------------------------------------------------------

// Exact mark bounding boxes per NotebookLM export mode, measured by the user
// in a graphics editor (tightened: top-left +1px; margins 15/15, 13/13, 45/45).
// Template matching locates the mark — and thereby the mode — robustly; these
// coordinates are then used as the precise removal target.
struct NotebookLMMode {
    int width, height;
    cv::Rect rect;
    const char* name;
};

static const std::vector<NotebookLMMode> kKnownModes = {
    {1280, 720, cv::Rect(1145, 689, 121, 17), "cinematic"},  // margins 15/15
    {1280, 720, cv::Rect(1105, 660, 131, 16), "explainer"},  // margins 45/45
    { 406, 720, cv::Rect( 296, 694,  98, 14), "short"},      // margins 13/13
};

static cv::Mat load_mark_template() {
    std::vector<uchar> buf(embedded::notebooklm_mark_png,
                           embedded::notebooklm_mark_png + embedded::notebooklm_mark_png_size);
    return cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
}

// ---------------------------------------------------------------------------
// Template-match the mark against each sampled frame and keep the best
// (highest |correlation|) frame. This is markedly more robust than matching a
// single temporal median: a static mark is clearest on whichever sampled frame
// has the best local contrast, while muddy multi-scene medians produce
// borderline peaks that false structures can edge out.
//
// Multi-scale + polarity-invariant (max of |maxcorr| and |mincorr|) to handle
// the adaptive light-on-dark / dark-on-light mark. Search is restricted to the
// bottom-right corner, where the mark always sits.
// ---------------------------------------------------------------------------
static constexpr float kMinConfidence = 0.45f;

struct TemplateHit { cv::Rect rect; float score; };

static std::optional<TemplateHit> match_mark(const std::vector<cv::Mat>& frames, const cv::Mat& tmpl) {
    const int tw0 = tmpl.cols, th0 = tmpl.rows;

    // 720p-class marks are ~0.9-1.25x the 98x14 template; capped at 1.5x because
    // larger scales match unrelated structure on busy backgrounds.
    static const float scales[] = {
        0.85f, 0.92f, 1.00f, 1.08f, 1.15f, 1.22f, 1.30f, 1.38f, 1.45f, 1.50f
    };

    float best = -1.0f;
    cv::Rect best_rect{};

    for (const cv::Mat& gray : frames) {
        const int W = gray.cols, H = gray.rows;
        const int rx0 = static_cast<int>(W * 0.55);
        const int ry0 = static_cast<int>(H * 0.86);
        if (rx0 >= W || ry0 >= H) continue;
        cv::Mat region(gray, cv::Range(ry0, H), cv::Range(rx0, W));

        for (float s : scales) {
            int tw = std::max(8, static_cast<int>(tw0 * s));
            int th = std::max(6, static_cast<int>(th0 * s));
            if (tw >= region.cols || th >= region.rows) continue;
            cv::Mat t;
            cv::resize(tmpl, t, cv::Size(tw, th), 0, 0,
                       s < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);
            cv::Mat r;
            cv::matchTemplate(region, t, r, cv::TM_CCOEFF_NORMED);
            double mn, mx; cv::Point loc_mn, loc_mx;
            cv::minMaxLoc(r, &mn, &mx, &loc_mn, &loc_mx);
            float score; cv::Point loc;
            if (std::fabs(mx) >= std::fabs(mn)) { score = static_cast<float>(std::fabs(mx)); loc = loc_mx; }
            else                                { score = static_cast<float>(std::fabs(mn)); loc = loc_mn; }
            if (score > best) {
                best = score;
                best_rect = cv::Rect(loc.x + rx0, loc.y + ry0, tw, th);
            }
        }
    }

    if (best < kMinConfidence) return std::nullopt;
    return TemplateHit{best_rect, best};
}

// ---------------------------------------------------------------------------
// Snap a raw detected bbox to the user's exact measured coordinates when the
// video resolution matches a known mode and detection landed near it. This
// makes the removal target the precise measured rect (not the ~2px-off
// template bbox) for known modes; unknown resolutions use the raw detection.
// ---------------------------------------------------------------------------
static cv::Rect snap_to_known(int W, int H, const cv::Rect& detected, const char*& mode_out) {
    constexpr int kCenterTol = 40;  // px (L1 distance between centers)
    const cv::Point dc(detected.x + detected.width / 2,
                       detected.y + detected.height / 2);
    const NotebookLMMode* best = nullptr;
    int best_dist = kCenterTol + 1;
    for (const auto& m : kKnownModes) {
        if (m.width != W || m.height != H) continue;
        const cv::Point mc(m.rect.x + m.rect.width / 2,
                           m.rect.y + m.rect.height / 2);
        const int d = std::abs(dc.x - mc.x) + std::abs(dc.y - mc.y);
        if (d < best_dist) { best_dist = d; best = &m; }
    }
    if (best) { mode_out = best->name; return best->rect; }
    mode_out = "auto";
    return detected;
}

// ---------------------------------------------------------------------------
// Public detection entry point
// ---------------------------------------------------------------------------
NotebookLMDetection NotebookLMDetector::detect(
    const std::string& input_path,
    std::optional<cv::Rect> manual_rect)
{
    NotebookLMDetection result;

    // Manual rect override (--rect x,y,w,h)
    if (manual_rect) {
        result.found = true;
        result.bbox = *manual_rect;
        result.confidence = 1.0f;
        spdlog::info("NotebookLM: using manual rect ({},{},{},{})",
                     manual_rect->x, manual_rect->y,
                     manual_rect->width, manual_rect->height);
        return result;
    }

    VideoReader reader;
    if (!reader.open(input_path)) {
        spdlog::error("NotebookLM detection: failed to open {}", input_path);
        return result;
    }
    const int W = reader.width(), H = reader.height();

    spdlog::info("NotebookLM: auto-detecting watermark ({}x{}, {} frames)",
                 W, H, reader.frame_count());

    static constexpr int kSampleCount = 12;
    std::vector<cv::Mat> frames = sample_grayscale_frames(reader, kSampleCount);
    reader.close();

    if (frames.empty()) {
        spdlog::warn("NotebookLM: could not sample any frames");
        return result;
    }

    cv::Mat tmpl = load_mark_template();
    if (tmpl.empty()) {
        spdlog::error("NotebookLM: embedded mark template failed to decode");
        return result;
    }

    auto hit = match_mark(frames, tmpl);
    if (!hit) {
        spdlog::warn("NotebookLM: auto-detection failed (template match below {:.2f}). "
                     "Use --rect x,y,w,h to specify the watermark region manually.",
                     kMinConfidence);
        return result;
    }

    const char* mode = nullptr;
    cv::Rect bbox = snap_to_known(W, H, hit->rect, mode);
    result.found = true;
    result.bbox = bbox;
    result.confidence = hit->score;
    spdlog::info("NotebookLM: detected at ({},{},{},{}) mode={} conf={:.2f}",
                 bbox.x, bbox.y, bbox.width, bbox.height, mode, hit->score);
    return result;
}

} // namespace wmr
