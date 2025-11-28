#pragma once

#include <filesystem>
#include <string>

#include "dist/common/env_loader.hpp"

namespace dist::common {

// Process-wide tuning knobs (log level, etc.).
struct GlobalConfig {
    std::string log_level = "info";
};

// Parameters consumed by the image generator binary.
struct ImageGeneratorConfig {
    std::filesystem::path input_dir;
    int loop_delay_ms = 100;
    int start_delay_ms = 500;
    int subscriber_wait_ms = 1000;
    std::string pub_endpoint;
    int heartbeat_ms = 2000;
    int queue_depth = 100;
};

// Parameters consumed by the feature extractor binary.
struct FeatureExtractorConfig {
    std::string sub_endpoint;
    std::string pub_endpoint;
    int sift_n_features = 0;
    double sift_contrast_threshold = 0.04;
    double sift_edge_threshold = 10.0;
    int queue_depth = 100;
};

// Parameters consumed by the data logger binary.
struct DataLoggerConfig {
    std::string sub_endpoint;
    std::filesystem::path db_path;
    std::filesystem::path raw_image_dir;
    std::filesystem::path annotated_image_dir;
};

struct AppConfig {
    GlobalConfig global;
    ImageGeneratorConfig generator;
    FeatureExtractorConfig extractor;
    DataLoggerConfig logger;
};

// Populate the strongly typed config structs from the dotenv loader.
[[nodiscard]] AppConfig load_app_config(const EnvLoader& env,
                                        const std::filesystem::path& root_dir);

}  // namespace dist::common
