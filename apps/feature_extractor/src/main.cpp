#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/utils.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic_bool g_keep_running{true};
constexpr std::size_t kMaxPayloadBytes = 50 * 1024 * 1024;  // 50 MB safety cap

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Feature Extractor - consumes frames, runs SIFT, republishes"};
    std::string cli_env_path;
    std::string cli_log_level;
    app.add_option("--env", cli_env_path, "Path to the .env file (overrides DIST_ENV_PATH)");
    app.add_option("--log-level", cli_log_level, "Override log level (trace|debug|info|warn|error|critical)");

    CLI11_PARSE(app, argc, argv);

    dist::common::EnvLoader loader;
    const auto root = fs::current_path();
    const char* env_override = std::getenv("DIST_ENV_PATH");
    fs::path env_path = root / ".env";
    if (!cli_env_path.empty()) {
        env_path = cli_env_path;
    } else if (env_override != nullptr) {
        env_path = env_override;
    }
    if (!loader.load_from_file(env_path)) {
        spdlog::error("Failed to read environment file at {}", env_path.string());
        return 1;
    }

    const auto config = dist::common::load_app_config(loader, root);
    const auto resolved_level =
        cli_log_level.empty() ? config.global.log_level : cli_log_level;
    spdlog::set_level(dist::common::level_from_string(resolved_level));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    dist::common::install_signal_handlers(g_keep_running);

    zmq::context_t context{1};
    zmq::socket_t subscriber{context, zmq::socket_type::sub};
    subscriber.set(zmq::sockopt::rcvtimeo, 500);
    subscriber.set(zmq::sockopt::subscribe, "");
    subscriber.connect(config.extractor.sub_endpoint);

    zmq::socket_t publisher{context, zmq::socket_type::push};
    publisher.set(zmq::sockopt::sndhwm, 100);
    publisher.set(zmq::sockopt::sndtimeo, 1000);
    publisher.set(zmq::sockopt::linger, 0);
    publisher.set(zmq::sockopt::immediate, 1);
    publisher.connect(config.extractor.pub_endpoint);

    auto sift = cv::SIFT::create(
        config.extractor.sift_n_features > 0 ? config.extractor.sift_n_features : 0,
        3,
        config.extractor.sift_contrast_threshold,
        config.extractor.sift_edge_threshold,
        1.6);

    spdlog::info("[feature_extractor] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Listening on {}", config.extractor.sub_endpoint);
    spdlog::info("Publishing to {}", config.extractor.pub_endpoint);

    while (g_keep_running.load()) {
        zmq::message_t header_msg;
        zmq::message_t image_msg;

        try {
            if (!subscriber.recv(header_msg, zmq::recv_flags::none)) {
                continue;  // timeout or interrupted
            }
            if (!header_msg.more()) {
                spdlog::warn("Discarding message without payload part");
                continue;
            }
            if (!subscriber.recv(image_msg, zmq::recv_flags::none)) {
                spdlog::warn("Incomplete multipart message (missing payload)");
                continue;
            }
        } catch (const zmq::error_t& ex) {
            if (g_keep_running.load()) {
                spdlog::error("ZeroMQ receive error: {}", ex.what());
            }
            break;
        }

        nlohmann::json source_header;
        try {
            source_header = nlohmann::json::parse(header_msg.to_string());
        } catch (const std::exception& ex) {
            spdlog::warn("Failed to parse header JSON: {}", ex.what());
            continue;
        }

        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(image_msg.size()));
        std::memcpy(encoded.data(), image_msg.data(), encoded.size());
        cv::Mat frame = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (frame.empty()) {
            spdlog::warn("Failed to decode incoming frame {}", source_header.value("frame_id", -1));
            continue;
        }

        spdlog::info("Received frame {} ({} bytes)",
                     source_header.value("frame_id", -1),
                     encoded.size());

        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        sift->detectAndCompute(frame, cv::noArray(), keypoints, descriptors);

        nlohmann::json keypoints_json = nlohmann::json::array();
        for (const auto& kp : keypoints) {
            keypoints_json.push_back({
                {"x", kp.pt.x},
                {"y", kp.pt.y},
                {"size", kp.size},
                {"angle", kp.angle},
                {"response", kp.response},
                {"octave", kp.octave},
                {"class_id", kp.class_id},
            });
        }

        std::vector<std::uint8_t> descriptor_bytes;
        if (!descriptors.empty()) {
            descriptor_bytes.resize(static_cast<std::size_t>(descriptors.total() * descriptors.elemSize()));
            std::memcpy(descriptor_bytes.data(), descriptors.data, descriptor_bytes.size());
        }

        cv::Mat annotated;
        cv::drawKeypoints(frame,
                          keypoints,
                          annotated,
                          cv::Scalar(0, 255, 0),
                          cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
        std::vector<std::uint8_t> annotated_bytes;
        if (!annotated.empty()) {
            cv::imencode(".png", annotated, annotated_bytes);
        }

        spdlog::info("Processed frame {} ({} keypoints)",
                     source_header.value("frame_id", -1),
                     keypoints.size());

        nlohmann::json header = {
            {"source", source_header},
            {"processed_timestamp", dist::common::now_iso8601()},
            {"keypoint_count", keypoints.size()},
            {"descriptor_rows", descriptors.rows},
            {"descriptor_cols", descriptors.cols},
            {"descriptor_elem_size", descriptors.elemSize()},
            {"descriptor_type", descriptors.type()},
            {"descriptors_bytes", descriptor_bytes.size()},
            {"annotated_bytes", annotated_bytes.size()},
            {"keypoints", std::move(keypoints_json)},
        };

        const std::size_t payload_bytes =
            descriptor_bytes.size() + encoded.size() + annotated_bytes.size();
        if (payload_bytes > kMaxPayloadBytes) {
            spdlog::warn("Processed payload too large ({} bytes > {}), dropping frame {}",
                         payload_bytes,
                         kMaxPayloadBytes,
                         source_header.value("frame_id", -1));
            continue;
        }

        std::vector<zmq::message_t> multipart;
        multipart.emplace_back(header.dump());
        multipart.emplace_back(descriptor_bytes.size());
        if (!descriptor_bytes.empty()) {
            std::memcpy(multipart.back().data(), descriptor_bytes.data(), descriptor_bytes.size());
        }
        multipart.emplace_back(encoded.size());
        std::memcpy(multipart.back().data(), encoded.data(), encoded.size());
        if (!annotated_bytes.empty()) {
            multipart.emplace_back(annotated_bytes.size());
            std::memcpy(multipart.back().data(), annotated_bytes.data(), annotated_bytes.size());
        }

        try {
            zmq::send_multipart(publisher, multipart, zmq::send_flags::none);
        } catch (const zmq::error_t& ex) {
            if (ex.num() == EAGAIN) {
                spdlog::warn(
                    "Downstream consumer not keeping up on {} (dropping processed frame {})",
                    config.extractor.pub_endpoint,
                    source_header.value("frame_id", -1));
                continue;
            }
            spdlog::error("Failed to publish processed frame: {}", ex.what());
            g_keep_running.store(false);
            break;
        }
    }

    spdlog::info("Feature extractor shutting down");
    return 0;
}
