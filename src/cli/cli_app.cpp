#include "cli/cli_app.hpp"
#include "cli/batch_processor.hpp"
#include "core/watermark_engine.hpp"
#include "core/types.hpp"
#include "core/fft_context.hpp"
#include "synthid/spectral_codebook.hpp"
#include "synthid/codebook_subtractor.hpp"
#include "synthid/codebook_builder.hpp"
#include "detection/synthid_detector.hpp"

#include <opencv2/imgcodecs.hpp>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <filesystem>

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

#ifndef APP_NAME
#define APP_NAME "wmr"
#endif

namespace wmr {

static int process_detect(const CliOptions& opts) {
    cv::Mat image = cv::imread(opts.input_path, cv::IMREAD_COLOR);
    if (image.empty()) {
        spdlog::error("Failed to load image: {}", opts.input_path);
        return 1;
    }

    spdlog::info("Image: {}x{}", image.cols, image.rows);

    // Visible watermark detection
    {
        WatermarkEngine engine;
        auto result = engine.detect_watermark(image);
        if (result.detected) {
            spdlog::info("[VISIBLE] DETECTED (confidence: {:.1f}%)",
                         result.confidence * 100.0f);
            spdlog::info("  Region: ({}, {}) {}x{}",
                         result.region.x, result.region.y,
                         result.region.width, result.region.height);
        } else {
            spdlog::info("[VISIBLE] not detected ({:.1f}%)",
                         result.confidence * 100.0f);
        }
    }

    // SynthID detection
    if (!opts.codebook_path.empty()) {
        FftContext fft;
        SpectralCodebook codebook;
        try {
            codebook.load(opts.codebook_path);
        } catch (const std::exception& e) {
            spdlog::error("Cannot load codebook: {}", e.what());
            return 1;
        }

        SynthidDetector detector(fft);
        auto result = detector.detect(image, codebook);
        if (result.detected) {
            spdlog::info("[SYNTHID] DETECTED (confidence: {:.1f}%)",
                         result.confidence * 100.0f);
            spdlog::info("  noise_corr={:.3f} carrier_phase={:.3f} "
                         "struct_ratio={:.3f} ms_consistency={:.3f}",
                         result.noise_correlation, result.carrier_phase_score,
                         result.structure_ratio, result.multi_scale_consistency);
        } else {
            spdlog::info("[SYNTHID] not detected ({:.1f}%)",
                         result.confidence * 100.0f);
        }
    }

    return 0;
}

static int process_single_image(const CliOptions& opts) {
    cv::Mat image = cv::imread(opts.input_path, cv::IMREAD_COLOR);
    if (image.empty()) {
        spdlog::error("Failed to load image: {}", opts.input_path);
        return 1;
    }

    spdlog::info("Loading: {}", opts.input_path);
    spdlog::info("Image: {}x{}", image.cols, image.rows);

    bool did_work = false;

    // Visible watermark removal
    if (opts.mode == CliMode::AutoRemove || opts.mode == CliMode::VisibleOnly) {
        WatermarkEngine engine;
        std::optional<WatermarkSize> force_size;
        if (opts.force_small) force_size = WatermarkSize::Small;
        else if (opts.force_large) force_size = WatermarkSize::Large;

        if (!opts.force) {
            auto detection = engine.detect_watermark(image, force_size);
            if (detection.detected) {
                spdlog::info("Visible watermark detected ({:.1f}%), removing...",
                             detection.confidence * 100.0f);
                engine.remove_watermark_detected(image, detection);
                did_work = true;
            } else if (opts.mode == CliMode::AutoRemove) {
                spdlog::debug("No visible watermark detected");
            } else {
                spdlog::warn("No visible watermark detected. Use --force to remove anyway.");
                return 2;
            }
        } else {
            spdlog::info("Force mode: removing visible watermark");
            engine.remove_watermark(image, force_size);
            did_work = true;
        }
    }

    // SynthID watermark removal
    if (opts.mode == CliMode::SynthidOnly ||
        (opts.mode == CliMode::AutoRemove && opts.synthid)) {
        if (opts.codebook_path.empty()) {
            spdlog::error("SynthID removal requires --codebook <path>");
            return 1;
        }

        FftContext fft;
        SpectralCodebook codebook;
        codebook.load(opts.codebook_path);

        // Detect first (unless forced)
        if (!opts.force) {
            SynthidDetector detector(fft);
            auto det = detector.detect(image, codebook);
            if (!det.detected) {
                spdlog::debug("No SynthID detected ({:.1f}%)", det.confidence * 100.0f);
                if (opts.mode == CliMode::SynthidOnly) {
                    spdlog::warn("No SynthID detected. Use --force to remove anyway.");
                    return 2;
                }
            } else {
                spdlog::info("SynthID detected ({:.1f}%), removing...",
                             det.confidence * 100.0f);
            }
        }

        CodebookSubtractor subtractor(fft);
        RemovalConfig config;
        config.custom_strength = opts.synthid_strength;
        subtractor.remove_synthid(image, codebook, config);
        spdlog::info("SynthID removal complete");
        did_work = true;
    }

    if (!did_work) {
        spdlog::info("No watermarks found or removed");
        return 0;
    }

    // Save output
    std::string output = opts.output_path.empty() ? opts.input_path : opts.output_path;
    std::filesystem::path out_path(output);
    if (!out_path.parent_path().empty() && !std::filesystem::exists(out_path.parent_path())) {
        std::filesystem::create_directories(out_path.parent_path());
    }

    std::vector<int> params;
    std::string ext = out_path.extension().string();
    if (ext == ".jpg" || ext == ".jpeg") {
        params = {cv::IMWRITE_JPEG_QUALITY, 100};
    } else if (ext == ".png") {
        params = {cv::IMWRITE_PNG_COMPRESSION, 6};
    } else if (ext == ".webp") {
        params = {cv::IMWRITE_WEBP_QUALITY, 101};
    }

    if (!cv::imwrite(output, image, params)) {
        spdlog::error("Failed to save: {}", output);
        return 1;
    }

    spdlog::info("Saved: {}", output);
    return 0;
}

static int process_build_codebook(const CliOptions& opts) {
    FftContext fft;
    CodebookBuilder builder(fft);

    auto stats = builder.build_from_directory(opts.input_path, opts.output_path);

    spdlog::info("Build complete: {} images → {} profiles ({} low-sample)",
                 stats.total_images, stats.profiles_created,
                 stats.skipped_low_samples);

    return stats.profiles_created > 0 ? 0 : 1;
}

int run_cli(int argc, char* argv[]) {
    CLI::App app{"Watermark Remover — remove Gemini visible and SynthID invisible watermarks", APP_NAME};
    app.set_version_flag("-V,--version", APP_VERSION);
    app.fallthrough();

    CliOptions opts;

    // Common options shared across subcommands
    auto add_common = [&](CLI::App* cmd) {
        cmd->add_flag("-v,--verbose", opts.verbose, "Verbose output");
    };

    // --- remove (default) ---
    auto* remove_cmd = app.add_subcommand("remove", "Auto-detect and remove watermarks");
    remove_cmd->add_option("input", opts.input_path, "Input image or directory")
        ->required()
        ->check(CLI::ExistingPath);
    remove_cmd->add_flag("-f,--force", opts.force, "Skip detection");
    remove_cmd->add_flag("--force-small", opts.force_small, "Force 48x48 watermark");
    remove_cmd->add_flag("--force-large", opts.force_large, "Force 96x96 watermark");
    remove_cmd->add_flag("--synthid", opts.synthid, "Also remove SynthID");
    remove_cmd->add_option("--codebook", opts.codebook_path, "Spectral codebook path (.wcb)");
    remove_cmd->add_option("--synthid-strength", opts.synthid_strength,
                           "SynthID removal strength 0.0-2.0")
        ->check(CLI::Range(0.0f, 2.0f));
    remove_cmd->add_option("--inpaint-strength", opts.inpaint_strength,
                           "Inpaint strength 0.0-1.0")
        ->check(CLI::Range(0.0f, 1.0f));
    remove_cmd->add_flag("-r,--recursive", opts.recursive, "Process directories recursively");
    remove_cmd->add_option("-o,--output", opts.output_path, "Output path");
    add_common(remove_cmd);

    // --- detect ---
    auto* detect_cmd = app.add_subcommand("detect", "Detect watermarks without modifying");
    detect_cmd->add_option("input", opts.input_path, "Input image")
        ->required()
        ->check(CLI::ExistingFile);
    detect_cmd->add_option("--codebook", opts.codebook_path, "Spectral codebook for SynthID detection");
    add_common(detect_cmd);

    // --- visible ---
    auto* visible_cmd = app.add_subcommand("visible", "Remove visible watermark only");
    visible_cmd->add_option("input", opts.input_path, "Input image")
        ->required()
        ->check(CLI::ExistingFile);
    visible_cmd->add_flag("-f,--force", opts.force, "Skip detection");
    visible_cmd->add_flag("--force-small", opts.force_small, "Force 48x48");
    visible_cmd->add_flag("--force-large", opts.force_large, "Force 96x96");
    visible_cmd->add_option("--inpaint-strength", opts.inpaint_strength,
                            "Inpaint strength 0.0-1.0")
        ->check(CLI::Range(0.0f, 1.0f));
    visible_cmd->add_option("-o,--output", opts.output_path, "Output path");
    add_common(visible_cmd);

    // --- synthid ---
    auto* synthid_cmd = app.add_subcommand("synthid", "Remove SynthID watermark only");
    synthid_cmd->add_option("input", opts.input_path, "Input image")
        ->required()
        ->check(CLI::ExistingFile);
    synthid_cmd->add_flag("-f,--force", opts.force, "Skip detection");
    synthid_cmd->add_option("--codebook", opts.codebook_path, "Spectral codebook path (.wcb)")
        ->required();
    synthid_cmd->add_option("--synthid-strength", opts.synthid_strength,
                            "SynthID removal strength 0.0-2.0")
        ->check(CLI::Range(0.0f, 2.0f));
    synthid_cmd->add_option("-o,--output", opts.output_path, "Output path");
    add_common(synthid_cmd);

    // --- build-codebook ---
    auto* build_cmd = app.add_subcommand("build-codebook", "Build SynthID codebook from reference images");
    build_cmd->add_option("input", opts.input_path, "Directory of reference images")
        ->required()
        ->check(CLI::ExistingDirectory);
    build_cmd->add_option("-o,--output", opts.output_path, "Output codebook path")
        ->required();
    add_common(build_cmd);

    // Default subcommand: if no subcommand given, treat as positional for backward compat
    app.require_subcommand(0, 1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (opts.verbose) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    if (opts.force_small && opts.force_large) {
        spdlog::error("Cannot use both --force-small and --force-large");
        return 1;
    }

    // Determine mode from subcommand
    if (detect_cmd->parsed()) {
        opts.mode = CliMode::Detect;
    } else if (visible_cmd->parsed()) {
        opts.mode = CliMode::VisibleOnly;
    } else if (synthid_cmd->parsed()) {
        opts.mode = CliMode::SynthidOnly;
    } else if (build_cmd->parsed()) {
        opts.mode = CliMode::BuildCodebook;
    } else {
        opts.mode = CliMode::AutoRemove;
    }

    try {
        switch (opts.mode) {
            case CliMode::Detect:
                return process_detect(opts);

            case CliMode::BuildCodebook:
                return process_build_codebook(opts);

            case CliMode::AutoRemove:
            case CliMode::VisibleOnly:
            case CliMode::SynthidOnly: {
                // Check if input is a directory → batch mode
                if (std::filesystem::is_directory(opts.input_path)) {
                    auto result = batch_process(opts);
                    return result.failed > 0 ? 1 : 0;
                }
                return process_single_image(opts);
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }

    return 0;
}

} // namespace wmr
