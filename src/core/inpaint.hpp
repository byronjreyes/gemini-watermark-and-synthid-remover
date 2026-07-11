#pragma once

#include <opencv2/core.hpp>

namespace wmr {

enum class InpaintMethod {
    Gaussian,
    Telea,
    NavierStokes,
    ShiftMap              // opencv_contrib xphoto INPAINT_SHIFTMAP (Phase B; usable only when WMR_HAS_XPHOTO)
#ifdef WMR_AI_DENOISE
    , AiDenoise  // FDnCNN NCNN/Vulkan AI denoise (dispatched by WatermarkEngine)
#endif
#ifdef WMR_AI_LAMA
    , LaMa      // LaMa ONNX inpaint (Phase C; dispatched by WatermarkEngine)
#endif
};

struct InpaintConfig {
    float strength = 0.85f;
    InpaintMethod method =
#ifdef WMR_AI_DENOISE
        InpaintMethod::AiDenoise;  // AI denoise is the default cleanup when built
#else
        InpaintMethod::Gaussian;
#endif
    int radius = 10;
    int padding = 32;
    bool full_mask = false;  // use full alpha region as inpaint mask (vs gradient edges)
    float sigma = 50.0f;     // FDnCNN sigma (1-150); unused by Gaussian/Telea/NS
};

void inpaint_residual(
    cv::Mat& image,
    const cv::Rect& region,
    const cv::Mat& alpha_map,
    const InpaintConfig& config = InpaintConfig{});

} // namespace wmr
