#include "video/geometry_detector.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace wmr {

// ---------------------------------------------------------------------------
// logo_size -> real alpha dimensions (the >48/>68 gate used by the remover).
// ---------------------------------------------------------------------------
cv::Size effective_alpha_size(VideoProfile profile, int logo_size) {
    if (profile == VideoProfile::VeoLegacy) {
        return (logo_size > 68) ? cv::Size(99, 43) : cv::Size(68, 30);
    }
    return (logo_size > 48) ? cv::Size(96, 96) : cv::Size(48, 48);
}

// ---------------------------------------------------------------------------
// Regression gate (pure): when does an auto-detection override the guess?
// ---------------------------------------------------------------------------
AutoGeometryVerdict decide_auto_geometry(bool snapped, float score, float raw_override_score) {
    if (snapped) return AutoGeometryVerdict::UseSnapped;
    if (score >= raw_override_score) return AutoGeometryVerdict::UseRaw;
    return AutoGeometryVerdict::FallBack;
}

// ---------------------------------------------------------------------------
// Multi-template polarity-invariant search. Clone of NotebookLMDetector's
// match_mark, generalized: a vector of native-size templates replaces the
// single-template scale ladder, because 48 and 96 are independent captures.
// ---------------------------------------------------------------------------
std::optional<GeometryHit> detect_geometry_in_frames(
    const std::vector<cv::Mat>& gray_frames,
    const std::vector<cv::Mat>& templates,
    const cv::Rect& corner_window,
    float min_confidence)
{
    if (templates.empty() || gray_frames.empty()) return std::nullopt;

    float best = -1.0f;
    GeometryHit best_hit{};
    bool have_best = false;

    for (const cv::Mat& gray : gray_frames) {
        if (gray.empty()) continue;
        // Clamp the corner window to this frame.
        const cv::Rect frame_rect(0, 0, gray.cols, gray.rows);
        const cv::Rect win = corner_window & frame_rect;
        if (win.width <= 0 || win.height <= 0) continue;
        const cv::Mat region(gray, win);  // shallow view in frame coords

        for (std::size_t ti = 0; ti < templates.size(); ++ti) {
            const cv::Mat& t = templates[ti];
            if (t.empty()) continue;
            // matchTemplate needs the image >= template in both dims.
            if (t.cols > region.cols || t.rows > region.rows) continue;

            cv::Mat r;
            cv::matchTemplate(region, t, r, cv::TM_CCOEFF_NORMED);
            double mn, mx;
            cv::Point loc_mn, loc_mx;
            cv::minMaxLoc(r, &mn, &mx, &loc_mn, &loc_mx);

            float score;
            cv::Point loc;
            if (std::fabs(mx) >= std::fabs(mn)) {
                score = static_cast<float>(std::fabs(mx));
                loc = loc_mx;
            } else {
                score = static_cast<float>(std::fabs(mn));
                loc = loc_mn;
            }

            if (score > best) {
                best = score;
                have_best = true;
                best_hit.score = score;
                best_hit.template_index = static_cast<int>(ti);
                best_hit.rect = cv::Rect(loc.x + win.x, loc.y + win.y, t.cols, t.rows);
            }
        }
    }

    if (!have_best || best < min_confidence) return std::nullopt;
    return best_hit;
}

// ---------------------------------------------------------------------------
// Manual/user rect -> WatermarkPosition. logo_size snaps to the nearest known
// asset width so the >48/>68 alpha gate still routes correctly for a rect whose
// width is a touch off (e.g. measured as 50 instead of 48).
// ---------------------------------------------------------------------------
namespace {
int nearest_asset_width(VideoProfile profile, int width) {
    if (profile == VideoProfile::VeoLegacy) {
        const int a = 68, b = 99;
        return (std::abs(width - a) <= std::abs(width - b)) ? a : b;
    }
    const int a = 48, b = 96;
    return (std::abs(width - a) <= std::abs(width - b)) ? a : b;
}
}  // namespace

WatermarkPosition rect_to_watermark_position(const cv::Rect& rect, int W, int H,
                                             VideoProfile profile) {
    const int logo_size = nearest_asset_width(profile, rect.width);
    return {W - (rect.x + rect.width),
            H - (rect.y + rect.height),
            logo_size};
}

// ---------------------------------------------------------------------------
// Snap a detected rect to the nearest known table geometry for the profile,
// matching by center L1 distance and only among variants whose effective alpha
// size equals the detected size. Clone of NotebookLMDetector's snap_to_known.
// ---------------------------------------------------------------------------
namespace {
const char* variant_label(VideoVariant v) {
    switch (v) {
        case VideoVariant::P720_1: return "720p-1";
        case VideoVariant::P720_2: return "720p-2";
        case VideoVariant::P1080p: return "1080p";
        case VideoVariant::Auto:   break;
    }
    return "auto";
}
}  // namespace

SnappedGeometry snap_geometry_to_known(const cv::Rect& detected, int W, int H,
                                       VideoProfile profile, int tol_px) {
    const cv::Point dc(detected.x + detected.width / 2,
                       detected.y + detected.height / 2);

    // Enumerate the table variants that produce distinct geometries per profile.
    // Veo has no 720p-2; P720_1 stands in for the Veo default ({17,18,68}).
    VideoVariant variants[3];
    int n_variants = 0;
    if (profile == VideoProfile::VeoLegacy) {
        variants[n_variants++] = VideoVariant::P720_1;   // default -> {17,18,68}
        variants[n_variants++] = VideoVariant::P1080p;   // -> {17,18,99}
    } else {
        variants[n_variants++] = VideoVariant::P720_1;   // {72,72,48}
        variants[n_variants++] = VideoVariant::P720_2;   // {29,40,44}
        variants[n_variants++] = VideoVariant::P1080p;   // {192,192,96}
    }

    int best_dist = tol_px + 1;
    VideoVariant best_variant = VideoVariant::Auto;

    for (int i = 0; i < n_variants; ++i) {
        const WatermarkPosition g =
            get_video_watermark_geometry(variants[i], W, H, profile);
        const cv::Size a = effective_alpha_size(profile, g.logo_size);
        // Only consider variants whose alpha size matches what was detected,
        // so a 96px hit cannot snap onto a 48px slot.
        if (a.width != detected.width || a.height != detected.height) continue;

        const cv::Point tl(W - g.margin_right - a.width,
                           H - g.margin_bottom - a.height);
        const cv::Point center(tl.x + a.width / 2, tl.y + a.height / 2);
        const int dist = std::abs(dc.x - center.x) + std::abs(dc.y - center.y);
        if (dist < best_dist) {
            best_dist = dist;
            best_variant = variants[i];
        }
    }

    SnappedGeometry out;
    if (best_variant != VideoVariant::Auto) {
        out.geometry = get_video_watermark_geometry(best_variant, W, H, profile);
        out.snapped = true;
        out.label = variant_label(best_variant);
    } else {
        out.geometry = rect_to_watermark_position(detected, W, H, profile);
        out.snapped = false;
        out.label = "auto";
    }
    return out;
}

} // namespace wmr
