#pragma once

#include <string>
#include <functional>
#include "cli/cli_app.hpp"

namespace wmr {

struct BatchResult {
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
};

using ProgressCallback = std::function<void(int current, int total, const std::string& filename)>;

BatchResult batch_process(const CliOptions& opts,
                          const ProgressCallback& progress = nullptr);

} // namespace wmr
