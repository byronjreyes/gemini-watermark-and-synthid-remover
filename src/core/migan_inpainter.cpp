/**
 * @file    migan_inpainter.cpp
 * @brief   MI-GAN ONNX inpainter implementation (ONNX Runtime, CPU)
 */

#ifdef WMR_AI_MIGAN

#include "core/migan_inpainter.hpp"

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
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace wmr {
namespace {

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

// Resolve the model path: $WMR_MIGAN_MODEL, then co-located with the exe, then
// <exe>/../share/wmr. Returns "" if none exist.
std::string resolve_model_path() {
    if (const char* env = std::getenv("WMR_MIGAN_MODEL"); env && std::filesystem::exists(env))
        return env;
    const auto dir = exe_dir();
    for (const auto& p : {dir / "migan_pipeline_v2.onnx",
                          dir / ".." / "share" / "wmr" / "migan_pipeline_v2.onnx"})
        if (std::filesystem::exists(p)) return p.string();
    return {};
}

} // namespace

struct MiganInpainter::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "wmr-migan"};
    std::unique_ptr<Ort::Session> session;
    std::string in_image, in_mask, out_name;
    bool ready = false;
};

MiganInpainter::MiganInpainter() : m_impl(std::make_unique<Impl>()) {}
MiganInpainter::~MiganInpainter() = default;

bool MiganInpainter::initialize() {
    if (m_impl->ready) return true;

    const std::string model = resolve_model_path();
    if (model.empty()) {
        spdlog::warn("MI-GAN model not found (set $WMR_MIGAN_MODEL or co-locate "
                     "migan_pipeline_v2.onnx next to the binary); MI-GAN unavailable");
        return false;
    }

    const int nthreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(nthreads);
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
        opts.SetLogSeverityLevel(3);  // warnings + errors only

        // ORT's path overload takes ORTCHAR_T* = wchar_t on Windows, char on POSIX.
        // std::filesystem::path::c_str() returns the native value_type for either.
        m_impl->session = std::make_unique<Ort::Session>(m_impl->env,
                                                          std::filesystem::path(model).c_str(),
                                                          opts);

        Ort::AllocatorWithDefaultOptions alloc;
        m_impl->in_image = m_impl->session->GetInputNameAllocated(0, alloc).get();
        m_impl->in_mask  = m_impl->session->GetInputNameAllocated(1, alloc).get();
        m_impl->out_name = m_impl->session->GetOutputNameAllocated(0, alloc).get();
        m_impl->ready = true;
        spdlog::info("MI-GAN inpainter ready (model={}, {} CPU threads, io={}/{}/{})",
                     model, nthreads, m_impl->in_image, m_impl->in_mask, m_impl->out_name);
    } catch (const Ort::Exception& e) {
        spdlog::warn("MI-GAN ORT init failed: {}", e.what());
        m_impl->session.reset();
        m_impl->ready = false;
        return false;
    }
    return true;
}

bool MiganInpainter::is_ready() const { return m_impl->ready; }

void MiganInpainter::inpaint_hole(cv::Mat& frame, const cv::Rect& mark_rect) {
    if (!m_impl->ready) return;

    const int H = frame.rows, W = frame.cols;
    const cv::Rect bounds(0, 0, W, H);
    cv::Rect m = mark_rect & bounds;
    if (m.empty()) return;

    // Mask: 0 at the hole (mark), 255 elsewhere — MI-GAN's convention. Dilate the
    // hole 5x5 (same as the NS/FSR path) to cover the anti-aliased fringe.
    cv::Mat hole = cv::Mat::zeros(H, W, CV_8U);
    hole(m).setTo(255);
    cv::dilate(hole, hole, cv::Mat::ones(5, 5, CV_8U), cv::Point(-1, -1), 1);
    cv::Mat keep;
    cv::bitwise_not(hole, keep);  // 0 at hole, 255 elsewhere

    // Image: RGB uint8 NCHW (MI-GAN takes uint8, not float — no scaling gotcha).
    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    std::vector<cv::Mat> ch;  // R, G, B planes (each H*W contiguous uint8)
    cv::split(rgb, ch);
    const size_t N = static_cast<size_t>(H) * W;
    std::vector<uint8_t> img_buf(3 * N);
    std::memcpy(img_buf.data() + 0 * N, ch[0].data, N);
    std::memcpy(img_buf.data() + 1 * N, ch[1].data, N);
    std::memcpy(img_buf.data() + 2 * N, ch[2].data, N);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t img_shape[4] = {1, 3, H, W};
    int64_t msk_shape[4] = {1, 1, H, W};
    Ort::Value inputs[2] = {
        Ort::Value::CreateTensor<uint8_t>(mem, img_buf.data(), img_buf.size(), img_shape, 4),
        Ort::Value::CreateTensor<uint8_t>(mem, keep.data,  N,            msk_shape, 4),
    };
    const char* in_names[2] = {m_impl->in_image.c_str(), m_impl->in_mask.c_str()};
    const char* out_names[1] = {m_impl->out_name.c_str()};
    auto outputs = m_impl->session->Run(Ort::RunOptions{nullptr}, in_names, inputs, 2, out_names, 1);

    // result: (1,3,H,W) uint8 = the composited image (input + fill).
    uint8_t* od = outputs[0].GetTensorMutableData<uint8_t>();
    cv::Mat planes[3] = {
        cv::Mat(H, W, CV_8U, od + 0 * N),
        cv::Mat(H, W, CV_8U, od + 1 * N),
        cv::Mat(H, W, CV_8U, od + 2 * N),
    };
    cv::Mat rgb_out;
    cv::merge(planes, 3, rgb_out);  // RGB uint8 (deep copy; od no longer needed)
    cv::Mat bgr_out;
    cv::cvtColor(rgb_out, bgr_out, cv::COLOR_RGB2BGR);
    bgr_out.copyTo(frame);  // MI-GAN already composited (identity outside the hole)
}

} // namespace wmr

#endif // WMR_AI_MIGAN
