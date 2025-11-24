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

    std::cout << "[feature_extractor] Dist Imaging Services v" << dist::common::version()
              << '\n';
    std::cout << "Sub endpoint: " << config.extractor.sub_endpoint << '\n';
    std::cout << "Pub endpoint: " << config.extractor.pub_endpoint << '\n';
    std::cout << "SIFT contrast threshold: " << config.extractor.sift_contrast_threshold
              << '\n';
    return 0;
}

