#pragma once

#include <string>

namespace wmr {

enum class CliMode {
    AutoRemove,
    Detect,
    VisibleOnly,
    SynthidOnly,
    BuildCodebook,
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
    float synthid_strength = 1.0f;
    bool recursive = false;
};

int run_cli(int argc, char* argv[]);

} // namespace wmr
