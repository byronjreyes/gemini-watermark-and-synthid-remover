#pragma once

#include <opencv2/core.hpp>
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

inline WatermarkPosition get_watermark_config(int width, int height) {
    auto size = get_watermark_size(width, height);
    if (size == WatermarkSize::Large) {
        return {64, 64, 96};
    }
    return {32, 32, 48};
}

} // namespace wmr
