#include "cli/batch_processor.hpp"
#include "core/watermark_engine.hpp"
#include "core/types.hpp"
#include "core/fft_context.hpp"
#include "synthid/spectral_codebook.hpp"
#include "synthid/codebook_subtractor.hpp"

#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace wmr {

static bool is_image_file(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
}

static std::vector<fs::path> collect_files(const std::string& dir, bool recursive) {
    std::vector<fs::path> files;

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && is_image_file(entry.path())) {
                files.push_back(entry.path());
            }
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() && is_image_file(entry.path())) {
                files.push_back(entry.path());
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

static int process_single(const fs::path& input, const CliOptions& opts) {
    cv::Mat image = cv::imread(input.string(), cv::IMREAD_COLOR);
    if (image.empty()) {
        spdlog::error("Failed to load: {}", input.filename().string());
        return 1;
    }

    WatermarkEngine engine;

    // Visible watermark processing
    if (opts.mode == CliMode::AutoRemove || opts.mode == CliMode::VisibleOnly) {
        if (!opts.force) {
            auto detection = engine.detect_watermark(image);
            if (detection.detected) {
                engine.remove_watermark_detected(image, detection);
            }
        } else {
            std::optional<WatermarkSize> force_size;
            if (opts.force_small) force_size = WatermarkSize::Small;
            else if (opts.force_large) force_size = WatermarkSize::Large;
            engine.remove_watermark(image, force_size);
        }
    }

    // SynthID processing
    if ((opts.mode == CliMode::AutoRemove && opts.synthid) || opts.mode == CliMode::SynthidOnly) {
        if (!opts.codebook_path.empty()) {
            FftContext fft;
            SpectralCodebook codebook;
            codebook.load(opts.codebook_path);
            CodebookSubtractor subtractor(fft);
            RemovalConfig config;
            config.custom_strength = opts.synthid_strength;
            subtractor.remove_synthid(image, codebook, config);
        }
    }

    // Determine output path
    fs::path out_path;
    if (!opts.output_path.empty()) {
        fs::path out_dir(opts.output_path);
        auto rel = fs::relative(input, opts.input_path);
        out_path = out_dir / rel;
        if (!out_path.has_extension()) {
            out_path /= input.filename();
        }
    } else {
        out_path = input;
    }

    if (!out_path.parent_path().empty() && !fs::exists(out_path.parent_path())) {
        fs::create_directories(out_path.parent_path());
    }

    std::vector<int> params;
    std::string ext = out_path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    if (ext == ".jpg" || ext == ".jpeg") {
        params = {cv::IMWRITE_JPEG_QUALITY, 100};
    } else if (ext == ".png") {
        params = {cv::IMWRITE_PNG_COMPRESSION, 6};
    } else if (ext == ".webp") {
        params = {cv::IMWRITE_WEBP_QUALITY, 101};
    }

    if (!cv::imwrite(out_path.string(), image, params)) {
        spdlog::error("Failed to save: {}", out_path.string());
        return 1;
    }

    spdlog::info("  → {}", out_path.string());
    return 0;
}

BatchResult batch_process(const CliOptions& opts, const ProgressCallback& progress) {
    BatchResult result;

    auto files = collect_files(opts.input_path, opts.recursive);
    result.total = static_cast<int>(files.size());

    if (files.empty()) {
        spdlog::warn("No image files found in: {}", opts.input_path);
        return result;
    }

    spdlog::info("Processing {} images...", result.total);

    for (int i = 0; i < result.total; ++i) {
        const auto& file = files[i];
        spdlog::info("[{}/{}] {}", i + 1, result.total, file.filename().string());

        if (progress) {
            progress(i + 1, result.total, file.filename().string());
        }

        try {
            int rc = process_single(file, opts);
            if (rc == 0) {
                ++result.succeeded;
            } else {
                ++result.failed;
            }
        } catch (const std::exception& e) {
            spdlog::error("  Error: {}", e.what());
            ++result.failed;
        }
    }

    spdlog::info("Batch complete: {} ok, {} failed, {} skipped of {} total",
                 result.succeeded, result.failed, result.skipped, result.total);

    return result;
}

} // namespace wmr
