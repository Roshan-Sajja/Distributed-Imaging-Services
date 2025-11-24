#include <iostream>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

int main() {
    dist::common::EnvLoader loader;
    const auto root = fs::current_path();
    const auto env_path = root / ".env";

    if (!loader.load_from_file(env_path)) {
        std::cerr << "Failed to read environment file at " << env_path << '\n';
        return 1;
    }

    const auto config = dist::common::load_app_config(loader, root);

    std::cout << "[image_generator] Dist Imaging Services v" << dist::common::version()
              << '\n';
    std::cout << "Input directory: " << config.generator.input_dir << '\n';
    std::cout << "Publish endpoint: " << config.generator.pub_endpoint << '\n';
    std::cout << "Loop delay (ms): " << config.generator.loop_delay_ms << '\n';
    return 0;
}

