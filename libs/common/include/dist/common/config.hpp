#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "dist/common/env_loader.hpp"

namespace dist::common {

struct GlobalConfig {
    std::string log_level = "info";
    int metrics_port = 9100;
};

struct ImageGeneratorConfig {
    std::filesystem::path input_dir;
    int loop_delay_ms = 100;
    int start_delay_ms = 500;
    std::string pub_endpoint;
    int heartbeat_ms = 2000;
};

struct FeatureExtractorConfig {
    std::string sub_endpoint;
    std::string pub_endpoint;
    int sift_n_features = 0;
    double sift_contrast_threshold = 0.04;
    double sift_edge_threshold = 10.0;
};

struct DataLoggerConfig {
    std::string sub_endpoint;
    std::filesystem::path db_path;
    std::filesystem::path raw_image_dir;
    int prune_days = 30;
};

struct AppConfig {
    GlobalConfig global;
    ImageGeneratorConfig generator;
    FeatureExtractorConfig extractor;
    DataLoggerConfig logger;
};

[[nodiscard]] AppConfig load_app_config(const EnvLoader& env,
                                        const std::filesystem::path& root_dir);

}  // namespace dist::common

