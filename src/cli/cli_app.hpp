#pragma once

#include <string>

namespace wmr {

enum class CliMode {
    AutoRemove,
    Detect,
    VisibleOnly,
    SynthidOnly,
    BuildCodebook,
    Video,
};

struct CliOptions {
    CliMode mode = CliMode::AutoRemove;
    std::string input_path;
    std::string output_path;
    bool force = false;
    bool force_small = false;
    bool force_large = false;
    bool verbose = false;
    bool detect_only = false;
    float inpaint_strength = 0.85f;
    bool synthid = false;
    std::string codebook_path;
    float synthid_strength = 0.50f;
    bool recursive = false;
    bool codebook_free = false;
    bool phase_adaptive = false;
    bool legacy_profile = false;
    std::string video_variant_str;
    bool scenes = false;
    double scene_threshold = 0.30;
    int video_crf = 14;
    std::string video_preset = "slow";
    std::string video_codec = "libx264";
};

int run_cli(int argc, char* argv[]);

} // namespace wmr
