#pragma once

// Content-based watermark geometry detection for the Gemini/Veo video path.
//
// Pure logic only: depends on OpenCV + core/types.hpp. It must NOT include
// video_reader.hpp (that pulls FFmpeg, which the test target does not link).
// Callers (VideoProcessor) sample frames themselves and hand them in here.

#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace wmr {

struct GeometryHit {
    cv::Rect rect;        // detected bbox in full-frame coords (size == winning template's size)
    float score = 0.0f;   // polarity-invariant |TM_CCOEFF_NORMED| in [0,1]
    int template_index = -1;  // index into the caller-supplied `templates` vector
};

// Multi-template, polarity-invariant template search restricted to a corner
// window. Each template is matched at its NATIVE size (no scale ladder, since
// the real alpha assets are captured per size). The score is max(|maxcorr|,
// |mincorr|) and the LOCATION follows the polarity (loc_mn when |mn| wins,
// loc_mx when |mx| wins) so a dark-on-light or light-on-dark mark both match.
// Returns nullopt when the global best is below `min_confidence`.
//
// `corner_window` is in full-frame coordinates; it is clamped to each frame.
std::optional<GeometryHit> detect_geometry_in_frames(
    const std::vector<cv::Mat>& gray_frames,   // CV_8UC1
    const std::vector<cv::Mat>& templates,     // CV_8UC1 (caller converts the alphas)
    const cv::Rect& corner_window,
    float min_confidence);

struct SnappedGeometry {
    WatermarkPosition geometry;        // margins + logo_size
    bool snapped = false;              // true iff landed within tol of a known table position
    std::string label;                 // "720p-1" / "720p-2" / "1080p" / "auto"
};

// Snap a raw detected rect to the nearest known table entry for the profile,
// by center L1 distance, only among variants whose effective alpha size matches
// the detected size (so a 96px hit cannot snap to a 48px variant). On no match,
// returns rect_to_watermark_position(detected) with snapped=false, label="auto".
SnappedGeometry snap_geometry_to_known(const cv::Rect& detected, int W, int H,
                                       VideoProfile profile, int tol_px = 40);

// Build a WatermarkPosition from a manual/user rect (--rect override or an
// un-snapped detection). logo_size snaps to the nearest known asset width
// {48,96} (Gemini) / {68,99} (Veo) so a slightly-off width still routes the
// alpha gate correctly. Margins come from the rect's far (bottom-right) corner.
WatermarkPosition rect_to_watermark_position(const cv::Rect& rect, int W, int H,
                                             VideoProfile profile);

// Single source of truth for the logo_size -> alpha-dims gate, mirroring the
// selection in video_processor.cpp (logo_size > 48/>68). Pure; also used by
// snap_geometry_to_known so the snap and the removal path cannot drift.
//   Gemini: >48 -> {96,96}, else {48,48}
//   Veo:    >68 -> {99,43}, else {68,30}
cv::Size effective_alpha_size(VideoProfile profile, int logo_size);

// The regression gate, extracted as pure logic so it is unit-testable without
// FFmpeg. Given an auto-detected result, decide whether it should override the
// resolution-based guess:
//   - a snapped (on-table) detection is trusted at the detection min confidence;
//   - a raw off-table detection must clear raw_override_score;
//   - otherwise fall back to the resolution guess.
// This stops a busy-corner false positive from regressing a video that already
// works today. `score` is already >= the detection threshold (the caller only
// invokes this on a hit), so it is only consulted for the raw case.
enum class AutoGeometryVerdict { UseSnapped, UseRaw, FallBack };
AutoGeometryVerdict decide_auto_geometry(bool snapped, float score, float raw_override_score);

} // namespace wmr
