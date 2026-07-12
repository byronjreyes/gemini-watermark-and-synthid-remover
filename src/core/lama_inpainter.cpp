/**
 * @file    lama_inpainter.cpp
 * @brief   LaMa ONNX inpainter implementation (ONNX Runtime, CPU)
 */

#ifdef WMR_AI_LAMA

#include "core/lama_inpainter.hpp"

#include <onnxruntime_cxx_api.h>

#include <opencv2/imgproc.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>   // readlink
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace wmr {
namespace {

constexpr int kTile = 512;  // LaMa fixed input dimension (H = W = 512)

// Directory containing the running executable.
std::filesystem::path exe_dir() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return std::filesystem::weakly_canonical(buf).parent_path();
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return std::filesystem::path(buf).parent_path(); }
#elif defined(_WIN32)
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
    if (n > 0) return std::filesystem::path(buf).parent_path();
#endif
    return std::filesystem::current_path();
}

// Resolve the model path: $WMR_LAMA_MODEL, then co-located with the exe, then
// <exe>/../share/wmr. Returns "" if none exist.
std::string resolve_model_path() {
    if (const char* env = std::getenv("WMR_LAMA_MODEL"); env && std::filesystem::exists(env))
        return env;
    const auto dir = exe_dir();
    for (const auto& p : {dir / "lama_fp32.onnx",
                          dir / ".." / "share" / "wmr" / "lama_fp32.onnx"})
        if (std::filesystem::exists(p)) return p.string();
    return {};
}

} // namespace

struct LamaInpainter::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "wmr-lama"};
    std::unique_ptr<Ort::Session> session;
    std::string in_image, in_mask, out_name;
    bool ready = false;
};

LamaInpainter::LamaInpainter() : m_impl(std::make_unique<Impl>()) {}
LamaInpainter::~LamaInpainter() = default;

bool LamaInpainter::initialize() {
    if (m_impl->ready) return true;

    const std::string model = resolve_model_path();
    if (model.empty()) {
        spdlog::warn("LaMa model not found (set $WMR_LAMA_MODEL or co-locate "
                     "lama_fp32.onnx next to the binary); LaMa unavailable");
        return false;
    }

    const int nthreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(nthreads);
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        opts.SetLogSeverityLevel(3);  // warnings + errors only

        m_impl->session = std::make_unique<Ort::Session>(m_impl->env, model.c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        m_impl->in_image = m_impl->session->GetInputNameAllocated(0, alloc).get();
        m_impl->in_mask  = m_impl->session->GetInputNameAllocated(1, alloc).get();
        m_impl->out_name = m_impl->session->GetOutputNameAllocated(0, alloc).get();
        m_impl->ready = true;
        spdlog::info("LaMa inpainter ready (model={}, {} CPU threads, io={}/{}/{})",
                     model, nthreads, m_impl->in_image, m_impl->in_mask, m_impl->out_name);
    } catch (const Ort::Exception& e) {
        spdlog::warn("LaMa ORT init failed: {}", e.what());
        m_impl->session.reset();
        m_impl->ready = false;
        return false;
    }
    return true;
}

bool LamaInpainter::is_ready() const { return m_impl->ready; }

// Pure inference: BGR8 512x512 + hole mask (0/255, 255 = hole) -> BGR8 512x512.
// Mirrors the validated eval pipeline (/tmp/lama_ab.py) exactly.
static cv::Mat run_inference(Ort::Session& sess, const cv::Mat& bgr, const cv::Mat& hole_mask,
                             const std::string& in_image, const std::string& in_mask,
                             const std::string& out_name) {
    // image: RGB float32 [0,1], NCHW (1,3,512,512) — NO ImageNet normalization.
    cv::Mat rgb, f;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(f, CV_32F, 1.0 / 255.0);
    std::vector<cv::Mat> ch;
    cv::split(f, ch);  // continuous R, G, B planes
    const size_t N = static_cast<size_t>(kTile) * kTile;
    std::vector<float> img_buf(3 * N);
    for (int c = 0; c < 3; ++c)
        std::memcpy(img_buf.data() + static_cast<size_t>(c) * N, ch[c].data, N * sizeof(float));

    // mask: float32 (1,1,512,512), 1 = hole.
    cv::Mat mf;
    hole_mask.convertTo(mf, CV_32F, 1.0 / 255.0);
    std::vector<float> mask_buf(N);
    std::memcpy(mask_buf.data(), mf.data, N * sizeof(float));

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t img_shape[4] = {1, 3, kTile, kTile};
    int64_t msk_shape[4] = {1, 1, kTile, kTile};
    Ort::Value inputs[2] = {
        Ort::Value::CreateTensor<float>(mem, img_buf.data(), img_buf.size(), img_shape, 4),
        Ort::Value::CreateTensor<float>(mem, mask_buf.data(), mask_buf.size(), msk_shape, 4),
    };
    const char* in_names[2] = {in_image.c_str(), in_mask.c_str()};
    const char* out_names[1] = {out_name.c_str()};
    auto outputs = sess.Run(Ort::RunOptions{nullptr}, in_names, inputs, 2, out_names, 1);

    float* od = outputs[0].GetTensorMutableData<float>();
    cv::Mat planes[3] = {
        cv::Mat(kTile, kTile, CV_32F, od + 0 * N),
        cv::Mat(kTile, kTile, CV_32F, od + 1 * N),
        cv::Mat(kTile, kTile, CV_32F, od + 2 * N),
    };
    cv::Mat rgb_f;
    cv::merge(planes, 3, rgb_f);          // RGB float HWC (deep copy)
    // This Carve/LaMa-ONNX export takes image in [0,1] but OUTPUTS in [0,255]
    // (not [0,1]) — clipping to [0,1] then *255 saturates everything to white.
    // Clip to [0,255] and convert directly.
    cv::max(rgb_f, 0.0, rgb_f);
    cv::min(rgb_f, 255.0, rgb_f);
    cv::Mat bgr_f;
    cv::cvtColor(rgb_f, bgr_f, cv::COLOR_RGB2BGR);
    cv::Mat bgr8;
    bgr_f.convertTo(bgr8, CV_8U);         // already [0,255] -> uint8 (round + clamp)
    return bgr8;
}

void LamaInpainter::inpaint_hole(cv::Mat& frame, const cv::Rect& mark_rect) {
    if (!m_impl->ready) return;

    const int W = frame.cols, H = frame.rows, K = kTile;
    // Bottom-right-aligned 512x512 window (LaMa's fixed input). The NotebookLM
    // mark sits in the bottom-right corner, so it always lands inside.
    const int cx0 = std::max(0, W - K);
    const int cy0 = std::max(0, H - K);
    const int cw  = std::min(K, W - cx0);
    const int ch  = std::min(K, H - cy0);
    cv::Mat crop = frame(cv::Rect(cx0, cy0, cw, ch)).clone();

    // If a frame dim is < 512 (not the NotebookLM case), pad top/left with
    // reflect so the bottom-right mark keeps its coordinates and the canvas is
    // 512x512. Zero padding in the common W,H >= 512 case.
    const int pad_top = K - ch, pad_left = K - cw;
    cv::Mat crop512;
    if (pad_top > 0 || pad_left > 0)
        cv::copyMakeBorder(crop, crop512, pad_top, 0, pad_left, 0, cv::BORDER_REFLECT);
    else
        crop512 = std::move(crop);

    // Hole mask in crop512 coords (255 = hole), dilated 5x5 like the NS/FSR path.
    cv::Mat mask = cv::Mat::zeros(K, K, CV_8U);
    cv::Rect local(mark_rect.x - cx0 + pad_left, mark_rect.y - cy0 + pad_top,
                   mark_rect.width, mark_rect.height);
    local &= cv::Rect(0, 0, K, K);
    if (!local.empty()) {
        mask(local).setTo(255);
        cv::dilate(mask, mask, cv::Mat::ones(5, 5, CV_8U), cv::Point(-1, -1), 1);
    }

    cv::Mat out512 = run_inference(*m_impl->session, crop512, mask,
                                   m_impl->in_image, m_impl->in_mask, m_impl->out_name);

    // Paste the real (un-padded) region back.
    cv::Rect back(pad_left, pad_top, cw, ch);
    out512(back).copyTo(frame(cv::Rect(cx0, cy0, cw, ch)));
}

} // namespace wmr

#endif // WMR_AI_LAMA
