#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

int main() {
    dist::common::EnvLoader loader;
    const auto root = fs::current_path();
    const char* env_override = std::getenv("DIST_ENV_PATH");
    const fs::path env_path = env_override != nullptr ? fs::path(env_override) : root / ".env";
    if (!loader.load_from_file(env_path)) {
        std::cerr << "Failed to read environment file at " << env_path << '\n';
        return 1;
    }

    const auto config = dist::common::load_app_config(loader, root);

    std::cout << "[data_logger] Dist Imaging Services v" << dist::common::version() << '\n';
    std::cout << "DB path: " << config.logger.db_path << '\n';
    std::cout << "Raw image dir: " << config.logger.raw_image_dir << '\n';
    return 0;
}

