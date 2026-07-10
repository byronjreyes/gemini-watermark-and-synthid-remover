#pragma once

#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace wmr {

enum class [[nodiscard]] ResultCode {
    Success,
    FileNotFound,
    InvalidFormat,
    ProcessingFailed,
    SaveFailed
};

enum class WatermarkSize { Small, Large };

// Still-image watermark profile generation.
// V1 = legacy Gemini (pre-3.5); V2 = Gemini 3.5+ (current). Kept separate from
// VideoVariant (which encodes resolution tiers) — see CLAUDE.md.
enum class WatermarkVariant {
    V1,   // legacy profile
    V2,   // current profile (Gemini 3.5+)
};

struct ProcessResult {
    bool success = false;
    bool skipped = false;
    float confidence = 0.0f;
    std::string message;
};

struct DetectionResult {
    bool detected = false;
    float confidence = 0.0f;
    cv::Rect region;
    WatermarkSize size = WatermarkSize::Small;
    float spatial_score = 0.0f;
    float gradient_score = 0.0f;
    float variance_score = 0.0f;
};

inline WatermarkSize get_watermark_size(int width, int height) {
    return (width > 1024 && height > 1024) ? WatermarkSize::Large : WatermarkSize::Small;
}

struct WatermarkPosition {
    int margin_right;
    int margin_bottom;
    int logo_size;

    cv::Point get_position(int image_width, int image_height) const {
        return {image_width - margin_right - logo_size,
                image_height - margin_bottom - logo_size};
    }
};

// V2 small position is aspect-aware: small Gemini outputs are 1024-class on the
// long side and inherit per-axis rounding from the source aspect ratio, so a
// single fixed margin does not fit every aspect. Ported verbatim from upstream
// allenk/GeminiWatermarkTool v0.3.1.
inline WatermarkPosition v2_small_config_from_dims(int width, int height) {
    const int long_side = std::max(width, height);
    const int short_side = std::min(width, height);
    // Map short side back to the canonical large width. Thresholds bisect the
    // observed canonical heights (540, 559, 572) for a 1024-class small output.
    double source_long_dim;
    if (short_side >= 566)      source_long_dim = 2752.0;
    else if (short_side >= 550) source_long_dim = 2816.0;
    else                        source_long_dim = 2848.0;
    const double scale = static_cast<double>(long_side) / source_long_dim;
    const int margin = static_cast<int>(std::round(192.0 * scale));
    return {margin, margin, 36};  // {margin_right, margin_bottom, logo_size}
}

inline WatermarkPosition get_watermark_config(int width, int height,
                                              WatermarkVariant variant) {
    const bool is_large = (width > 1024 && height > 1024);
    if (variant == WatermarkVariant::V1) {
        return is_large ? WatermarkPosition{64, 64, 96}
                        : WatermarkPosition{32, 32, 48};
    }
    // V2 (current profile)
    if (is_large) {
        return {192, 192, 96};
    }
    return v2_small_config_from_dims(width, height);
}

// Backward-compatible overload: defaults to V1 so existing callers
// (NccDetector's internal default-position path, the video path, and existing
// tests) are unchanged. The V2 default is applied one level up, in WatermarkEngine.
inline WatermarkPosition get_watermark_config(int width, int height) {
    return get_watermark_config(width, height, WatermarkVariant::V1);
}

// Video-specific watermark geometry
// Gemini/Veo videos use different positions than still images
enum class VideoVariant {
    Auto,       // auto-detect from resolution
    P720_1,     // 720p standard (48x48, margin 72,72)
    P720_2,     // 720p compact (44x44, margin 29,40)
    P1080p,     // 1080p (96x96, margin 192,192)
};

enum class VideoProfile {
    GeminiDiamond,
    VeoLegacy,
    NotebookLM,
};

inline WatermarkPosition get_video_watermark_geometry(
    VideoVariant variant, int width, int height, VideoProfile profile = VideoProfile::GeminiDiamond)
{
    // Veo legacy text watermark — different shape and position
    if (profile == VideoProfile::VeoLegacy) {
        // Reference alpha maps: 68x30 (small), 99x43 (large)
        switch (variant) {
            case VideoVariant::P1080p:
                return {17, 18, 99};  // 99x43 large Veo text
            default:
                return {17, 18, 68};  // 68x30 small Veo text
        }
    }

    // Gemini V2 diamond watermark
    switch (variant) {
        case VideoVariant::P720_1:
            return {72, 72, 48};
        case VideoVariant::P720_2:
            return {29, 40, 44};
        case VideoVariant::P1080p:
            return {192, 192, 96};
        case VideoVariant::Auto:
            break;
    }

    // Auto-detect from resolution
    int max_dim = std::max(width, height);
    if (max_dim >= 1920) {
        return {192, 192, 96};
    }
    // Default to 720p-1 for 720p content
    return {72, 72, 48};
}

} // namespace wmr
