#include "video/notebooklm_gates.hpp"

#include <opencv2/imgproc.hpp>
#include <string>

namespace wmr {

// Band geometry around the mark: a ring with a GAP from the mark bbox, so the
// mark's own (high-gradient) edge and anti-aliased fringe do not inflate the
// background complexity score.
static constexpr int kGap = 4;       // px gap from the mark bbox
static constexpr int kBandWidth = 8; // px thickness of the sampled ring

float background_complexity_score(const cv::Mat& gray_frame, const cv::Rect& mark_rect) {
    if (gray_frame.empty()) return 0.0f;
    cv::Mat gray = gray_frame;
    if (gray.channels() != 1) {
        cv::Mat g;
        cv::cvtColor(gray, g, cv::COLOR_BGR2GRAY);
        gray = g;
    }

    // Sobel gradient magnitude.
    cv::Mat gx, gy;
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
    cv::Mat mag;
    cv::magnitude(gx, gy, mag);

    const cv::Rect bounds(0, 0, gray.cols, gray.rows);
    cv::Rect mark = mark_rect & bounds;
    if (mark.empty()) return 0.0f;

    const int outer_pad = kGap + kBandWidth;
    cv::Rect outer = (mark + cv::Size(2 * outer_pad, 2 * outer_pad)) - cv::Point(outer_pad, outer_pad);
    cv::Rect inner = (mark + cv::Size(2 * kGap, 2 * kGap)) - cv::Point(kGap, kGap);
    outer &= bounds;
    inner &= bounds;
    if (outer.empty()) return 0.0f;

    // Ring mask: outer band minus the inner gap (which already excludes the mark).
    cv::Mat mask = cv::Mat::zeros(gray.size(), CV_8U);
    mask(outer) = 255;
    mask(inner) = 0;

    cv::Scalar mean, sd;
    cv::meanStdDev(mag, mean, sd, mask);
    return static_cast<float>(mean[0]);
}

bool background_is_intricate(const cv::Mat& gray_frame, const cv::Rect& mark_rect,
                             float threshold) {
    return background_complexity_score(gray_frame, mark_rect) >= threshold;
}

std::string resolve_inpaint_method(float complexity, double threshold,
                                   double lama_threshold,
                                   const std::string& requested,
                                   bool has_xphoto, bool has_lama) {
    const bool intricate = complexity >= static_cast<float>(threshold);
    const bool lama_hard = complexity >= static_cast<float>(lama_threshold);
    if (requested == "lama") {
        // Explicit LaMa: only the hardest scenes AND when compiled in; otherwise
        // fall through to FSR/NS. "lama" means "LaMa for hard scenes, FSR/NS for
        // the rest", not "LaMa on every frame".
        if (has_lama && lama_hard) return "lama";
        return (has_xphoto && intricate) ? "fsr" : "ns";
    }
    if (requested == "fsr") {
        // Explicit FSR: honour it only when xphoto is compiled in; else NS.
        return has_xphoto ? "fsr" : "ns";
    }
    if (requested == "ns") {
        return "ns";
    }
    // "auto" (and any unrecognized value): route intricate backgrounds to FSR
    // when available, otherwise NS — the bit-identical v1.6.0 default. LaMa is
    // never auto-selected (~2.4 s/frame is infeasible as a default).
    return (has_xphoto && intricate) ? "fsr" : "ns";
}

} // namespace wmr
