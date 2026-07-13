/**
 * @file    migan_coreml_inpainter.mm
 * @brief   MI-GAN inpainter (native CoreML, Apple Silicon) for NotebookLM
 * @license MIT (project). MI-GAN model + code are MIT (Picsart AI Research).
 *
 * @details
 * macOS impl of `wmr::MiganInpainter`. Replaces the ORT impl on Apple — a native
 * CoreML mlprogram runs the same MI-GAN Generator on the Neural Engine at ~28 ms
 * /frame (vs ORT-CPU ~225 ms; ~11×), A/B-verified to match the ORT baseline within
 * Δ1.9/255 on 12 NotebookLM scenes. Linux/Windows keep the ORT `.cpp` impl.
 *
 * Same interface as `migan_inpainter.cpp` (ORT): `initialize()` / `is_ready()` /
 * `inpaint_hole(frame, mark_rect)`. The dispatch + leaked singleton in
 * `video_processor.cpp` are byte-identical for both impls. The `MLModel` is a
 * process singleton, intentionally leaked (CoreML teardown races static
 * destruction, same rationale as NCNN/ORT).
 *
 * Verified IO (probed empirically): the bare MI-GAN `Generator(resolution=512)`
 * takes a (1,4,512,512) float32 tensor = `cat([mask-0.5, image*mask])` with image
 * normalized to [-1,1] and mask in [0,1] (0=hole, 1=keep); output (1,3,512,512)
 * float in [-1,1]. CoreML may deliver the output as Float16 OR Float32 — both are
 * handled. Fixed 512x512 input (the generator is resolution-locked by its
 * `register_buffer` filters), so the host crops a square around the mark and
 * resizes to 512, then pastes back with a dilated+blurred soft mask.
 *
 * Build: Objective-C++ (.mm), linked against the system CoreML + Foundation
 * frameworks (no vendored lib). `WMR_BUILD_AI_MIGAN_COREML` gates this TU.
 */

#ifdef WMR_AI_MIGAN_COREML

#include "core/migan_inpainter.hpp"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include <opencv2/imgproc.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#include <string>
#include <vector>

namespace wmr {
namespace {

// Directory containing the running executable (macOS).
std::filesystem::path exe_dir() {
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return std::filesystem::weakly_canonical(buf).parent_path();
    return std::filesystem::current_path();
}

// Resolve the .mlpackage directory: $WMR_COREML_MODEL, co-located with the exe,
// then <exe>/../share/wmr. Returns "" if none exist (a .mlpackage is a directory).
std::string resolve_model_path() {
    if (const char* env = std::getenv("WMR_COREML_MODEL");
        env && std::filesystem::exists(env))
        return env;
    const auto dir = exe_dir();
    for (const auto& p : {dir / "migan_512_places2_fp16.mlpackage",
                          dir / ".." / "share" / "wmr" / "migan_512_places2_fp16.mlpackage"})
        if (std::filesystem::exists(p)) return p.string();
    return {};
}

// Fixed generator input resolution (MI-GAN is resolution-locked).
constexpr int kRes = 512;

} // namespace

struct MiganInpainter::Impl {
    MLModel* model = nil;   // retained; leaked with the singleton
    bool ready = false;
};

MiganInpainter::MiganInpainter() : m_impl(std::make_unique<Impl>()) {}

MiganInpainter::~MiganInpainter() {
    // Leaked singleton in practice (CoreML teardown races static destruction). The
    // release is defensive only — covers a hypothetical non-singleton instance.
#if !__has_feature(objc_arc)
    if (m_impl && m_impl->model) { [m_impl->model release]; m_impl->model = nil; }
#endif
}

bool MiganInpainter::initialize() {
    if (m_impl->ready) return true;

    const std::string model = resolve_model_path();
    if (model.empty()) {
        spdlog::warn("MI-GAN CoreML model not found (set $WMR_COREML_MODEL or co-locate "
                     "migan_512_places2_fp16.mlpackage next to the binary); MI-GAN unavailable");
        return false;
    }

    @try {
        NSError* err = nil;
        MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
        cfg.computeUnits = MLComputeUnitsAll;   // ANE preferred, GPU/CPU fallback

        NSURL* pkgURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:model.c_str()]];
        // Two-step load: compile (slow first time, then cached by CoreML keyed on
        // the source model hash) -> load the compiled .mlmodelc. CoreML exposes
        // this as the +modelWithContentsOfURL:configuration:error: CLASS factory
        // (returns autoreleased), not an instance initializer.
        NSURL* compiledURL = [MLModel compileModelAtURL:pkgURL error:&err];
        if (!compiledURL) {
            spdlog::warn("MI-GAN CoreML compileModel failed: {}",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            return false;
        }
        MLModel* ml = [MLModel modelWithContentsOfURL:compiledURL
                                        configuration:cfg error:&err];
        if (!ml) {
            spdlog::warn("MI-GAN CoreML load failed: {}",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            return false;
        }
#if !__has_feature(objc_arc)
        m_impl->model = [ml retain];   // factory returns autoreleased; retain for the singleton
#else
        m_impl->model = ml;
#endif
        m_impl->ready = true;
        spdlog::info("MI-GAN inpainter ready (CoreML, model={}, computeUnits=ALL, res={})",
                     model, kRes);
    } @catch (NSException* ex) {
        spdlog::warn("MI-GAN CoreML init exception: {}", [[ex reason] UTF8String]);
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

    @autoreleasepool {
        // --- crop a square around the mark, clamped to the frame (always fits) ---
        int side = std::max(256, std::max(m.width, m.height) * 2 + 160);
        side = std::min({side, H, W});
        const int cx = m.x + m.width / 2, cy = m.y + m.height / 2;
        const int x0 = std::clamp(cx - side / 2, 0, W - side);
        const int y0 = std::clamp(cy - side / 2, 0, H - side);
        cv::Mat crop = frame(cv::Rect(x0, y0, side, side)).clone();

        // --- mask: 0 at the hole, 255 elsewhere, dilated 5x5 (matches ORT impl) ---
        cv::Rect markInCrop(m.x - x0, m.y - y0, m.width, m.height);
        markInCrop &= cv::Rect(0, 0, side, side);
        cv::Mat hole = cv::Mat::zeros(side, side, CV_8U);
        hole(markInCrop).setTo(255);
        cv::dilate(hole, hole, cv::Mat::ones(5, 5, CV_8U), cv::Point(-1, -1), 1);
        cv::Mat mask;                       // 0 at hole, 255 elsewhere
        cv::bitwise_not(hole, mask);

        // --- preprocess: resize to 512, build (1,4,512,512) f32 = [mt-0.5, it*mt] ---
        cv::Mat img_r, mask_r;
        cv::resize(crop, img_r, cv::Size(kRes, kRes), 0, 0, cv::INTER_LINEAR);
        cv::resize(mask, mask_r, cv::Size(kRes, kRes), 0, 0, cv::INTER_NEAREST);
        cv::Mat rgb; cv::cvtColor(img_r, rgb, cv::COLOR_BGR2RGB);  // interleaved RGB uint8

        std::vector<float> buf(static_cast<size_t>(4) * kRes * kRes);
        float* c0 = buf.data();                       // mt - 0.5
        float* c1 = buf.data() + 1 * kRes * kRes;     // it * mt  (R)
        float* c2 = buf.data() + 2 * kRes * kRes;     // it * mt  (G)
        float* c3 = buf.data() + 3 * kRes * kRes;     // it * mt  (B)
        for (int i = 0; i < kRes * kRes; ++i) {
            const float mt = mask_r.at<uint8_t>(i) / 255.0f;
            const cv::Vec3b& px = rgb.at<cv::Vec3b>(i);
            c0[i] = mt - 0.5f;
            c1[i] = (px[0] * (2.0f / 255.0f) - 1.0f) * mt;   // R
            c2[i] = (px[1] * (2.0f / 255.0f) - 1.0f) * mt;   // G
            c3[i] = (px[2] * (2.0f / 255.0f) - 1.0f) * mt;   // B
        }

        // --- build the MLMultiArray input (zero-copy over buf) + predict ---
        NSError* err = nil;
        NSArray<NSNumber*>* shape = @[@1, @4, @(kRes), @(kRes)];
        NSArray<NSNumber*>* strides = @[@(4 * kRes * kRes), @(kRes * kRes), @(kRes), @1];
        MLMultiArray* inputMA = [[MLMultiArray alloc]
            initWithDataPointer:reinterpret_cast<void*>(buf.data())
                          shape:shape
                       dataType:MLMultiArrayDataTypeFloat32
                        strides:strides
                     deallocator:nil
                          error:&err];
        if (!inputMA) {
            spdlog::warn("MI-GAN CoreML input array build failed: {}",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            return;
        }
        MLDictionaryFeatureProvider* fp = [[MLDictionaryFeatureProvider alloc]
            initWithDictionary:@{@"input_image": inputMA} error:&err];
        if (!fp) {
            spdlog::warn("MI-GAN CoreML feature provider build failed: {}",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            return;
        }
        MLPredictionOptions* opts = [[MLPredictionOptions alloc] init];
        id<MLFeatureProvider> outFP = [m_impl->model predictionFromFeatures:fp
                                                                    options:opts
                                                                      error:&err];
        if (!outFP) {
            spdlog::warn("MI-GAN CoreML prediction failed: {}",
                         err ? [[err localizedDescription] UTF8String] : "unknown");
            return;
        }
        MLMultiArray* outMA = [[outFP featureValueForName:@"output_image"] multiArrayValue];
        if (!outMA) {
            spdlog::warn("MI-GAN CoreML: output_image feature missing");
            return;
        }

        // --- read output (1,3,512,512); handle Float32 OR Float16 delivery ---
        std::vector<float> outbuf(static_cast<size_t>(3) * kRes * kRes);
        if (outMA.dataType == MLMultiArrayDataTypeFloat32) {
            std::memcpy(outbuf.data(), outMA.dataPointer, outbuf.size() * sizeof(float));
        } else if (outMA.dataType == MLMultiArrayDataTypeFloat16) {
            const __fp16* p = static_cast<const __fp16*>(outMA.dataPointer);
            for (size_t i = 0; i < outbuf.size(); ++i) outbuf[i] = static_cast<float>(p[i]);
        } else {
            spdlog::warn("MI-GAN CoreML: unsupported output dataType {}", (int)outMA.dataType);
            return;
        }

        // --- postprocess: denormalize [-1,1]->[0,255], resize back, soft-paste ---
        cv::Mat planes[3];
        for (int c = 0; c < 3; ++c) {
            cv::Mat p(kRes, kRes, CV_32F,
                      outbuf.data() + static_cast<size_t>(c) * kRes * kRes);
            cv::Mat p8;
            p.convertTo(p8, CV_32F, 0.5, 0.5);     // (p*0.5 + 0.5)
            p8.convertTo(p8, CV_8U, 255.0);        // *255 + saturate_cast clamp
            planes[c] = p8.clone();                // clone (p shares outbuf)
        }
        cv::Mat rgb_out; cv::merge(planes, 3, rgb_out);   // RGB uint8 (kRes,kRes,3)
        cv::Mat bgr_out; cv::cvtColor(rgb_out, bgr_out, cv::COLOR_RGB2BGR);
        cv::resize(bgr_out, bgr_out, cv::Size(side, side), 0, 0, cv::INTER_LINEAR);

        // soft-composite mask: dilate(0-hole mask,3) -> gaussianBlur(5,1) -> /255
        cv::Mat md, mf;
        cv::dilate(mask, md, cv::Mat::ones(3, 3, CV_8U));   // shrink hole (matches A/B)
        cv::GaussianBlur(md, md, cv::Size(5, 5), 1.0);
        md.convertTo(mf, CV_32F, 1.0 / 255.0);              // 0..1
        cv::Mat mf3; cv::merge(std::vector<cv::Mat>{mf, mf, mf}, mf3);

        cv::Mat crop_f, out_f;
        crop.convertTo(crop_f, CV_32F);
        bgr_out.convertTo(out_f, CV_32F);
        cv::Mat one(crop_f.size(), crop_f.type(), cv::Scalar::all(1.0));
        cv::Mat invmf3; cv::subtract(one, mf3, invmf3);
        cv::Mat composed; cv::add(crop_f.mul(mf3), out_f.mul(invmf3), composed);
        cv::Mat composed8; composed.convertTo(composed8, CV_8U);   // saturate clamps

        composed8.copyTo(frame(cv::Rect(x0, y0, side, side)));
    }
}

} // namespace wmr

#endif // WMR_AI_MIGAN_COREML
