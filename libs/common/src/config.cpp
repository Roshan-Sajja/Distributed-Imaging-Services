#include "dist/common/config.hpp"

#include <cstdlib>

namespace dist::common {

namespace {
int to_int(const EnvLoader& env, std::string_view key, int fallback) {
    if (auto value = env.get(key)) {
        try {
            return std::stoi(*value);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

double to_double(const EnvLoader& env, std::string_view key, double fallback) {
    if (auto value = env.get(key)) {
        try {
            return std::stod(*value);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

std::filesystem::path to_path(const EnvLoader& env,
                              std::string_view key,
                              const std::filesystem::path& fallback,
                              const std::filesystem::path& root) {
    if (auto value = env.get(key)) {
        std::filesystem::path candidate = *value;
        if (candidate.is_relative()) {
            candidate = root / candidate;
        }
        return candidate;
    }
    return fallback.is_relative() ? (root / fallback) : fallback;
}
}  // namespace

AppConfig load_app_config(const EnvLoader& env, const std::filesystem::path& root_dir) {
    AppConfig cfg;

    cfg.global.log_level = env.get_or("APP_LOG_LEVEL", cfg.global.log_level);
    cfg.generator.input_dir =
        to_path(env, "IMAGE_GENERATOR_INPUT_DIR", "./data/images", root_dir);
    cfg.generator.loop_delay_ms =
        to_int(env, "IMAGE_GENERATOR_LOOP_DELAY_MS", cfg.generator.loop_delay_ms);
    cfg.generator.start_delay_ms =
        to_int(env, "IMAGE_GENERATOR_START_DELAY_MS", cfg.generator.start_delay_ms);
    cfg.generator.subscriber_wait_ms =
        to_int(env, "IMAGE_GENERATOR_SUBSCRIBER_WAIT_MS", cfg.generator.subscriber_wait_ms);
    cfg.generator.pub_endpoint =
        env.get_or("IMAGE_GENERATOR_PUB_ENDPOINT", "tcp://127.0.0.1:5555");
    cfg.generator.heartbeat_ms =
        to_int(env, "IMAGE_GENERATOR_HEARTBEAT_MS", cfg.generator.heartbeat_ms);
    const int extractor_queue_fallback =
        to_int(env, "FEATURE_EXTRACTOR_QUEUE_DEPTH", cfg.generator.queue_depth);
    cfg.generator.queue_depth =
        to_int(env, "IMAGE_GENERATOR_QUEUE_DEPTH", extractor_queue_fallback);

    cfg.extractor.sub_endpoint =
        env.get_or("FEATURE_EXTRACTOR_SUB_ENDPOINT", "tcp://127.0.0.1:5555");
    cfg.extractor.pub_endpoint =
        env.get_or("FEATURE_EXTRACTOR_PUB_ENDPOINT", "tcp://127.0.0.1:5556");
    cfg.extractor.sift_n_features =
        to_int(env, "FEATURE_EXTRACTOR_SIFT_N_FEATURES", cfg.extractor.sift_n_features);
    cfg.extractor.sift_contrast_threshold = to_double(
        env, "FEATURE_EXTRACTOR_SIFT_CONTRAST_THRESHOLD", cfg.extractor.sift_contrast_threshold);
    cfg.extractor.sift_edge_threshold =
        to_double(env, "FEATURE_EXTRACTOR_SIFT_EDGE_THRESHOLD", cfg.extractor.sift_edge_threshold);
    cfg.extractor.queue_depth =
        to_int(env, "FEATURE_EXTRACTOR_QUEUE_DEPTH", cfg.extractor.queue_depth);

    cfg.logger.sub_endpoint =
        env.get_or("DATA_LOGGER_SUB_ENDPOINT", "tcp://127.0.0.1:5556");
    cfg.logger.db_path =
        to_path(env, "DATA_LOGGER_DB_PATH", "./storage/dist_imaging.sqlite", root_dir);
    cfg.logger.raw_image_dir =
        to_path(env, "DATA_LOGGER_RAW_IMAGE_DIR", "./storage/raw_frames", root_dir);
    cfg.logger.annotated_image_dir =
        to_path(env, "DATA_LOGGER_ANNOTATED_DIR", "./storage/annotated_frames", root_dir);

    return cfg;
}

}  // namespace dist::common
