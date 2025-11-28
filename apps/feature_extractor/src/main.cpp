#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/utils.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic_bool g_keep_running{true};
constexpr std::size_t kMaxPayloadBytes = 50 * 1024 * 1024;  // 50 MB safety cap
constexpr std::size_t kDefaultQueueDepth = 100;             // Pending frames when logger is absent
constexpr auto kNoSubscriberBackoff = std::chrono::milliseconds(500);
constexpr int kZmqRetryAttempts = 3;
constexpr auto kZmqRetryBackoff = std::chrono::seconds(1);

bool connect_with_retry(zmq::socket_t& socket,
                        const std::string& endpoint,
                        std::string_view socket_name,
                        bool fatal_on_failure) {
    int attempt = 1;
    while (g_keep_running.load()) {
        try {
            socket.connect(endpoint);
            return true;
        } catch (const zmq::error_t& ex) {
            spdlog::warn(
                "Failed to connect {} to {} (attempt {}): {}. Waiting for upstream component...",
                socket_name,
                endpoint,
                attempt,
                ex.what());
            if (fatal_on_failure && attempt >= kZmqRetryAttempts) {
                return false;
            }
            ++attempt;
            std::this_thread::sleep_for(kZmqRetryBackoff);
        }
    }
    return false;
}

bool bind_with_retry(zmq::socket_t& socket, const std::string& endpoint, std::string_view role) {
    for (int attempt = 1; attempt <= kZmqRetryAttempts; ++attempt) {
        try {
            socket.bind(endpoint);
            return true;
        } catch (const zmq::error_t& ex) {
            spdlog::error("Failed to bind {} on {} (attempt {}/{}): {}. Another process might be "
                          "using this endpoint.",
                          role,
                          endpoint,
                          attempt,
                          kZmqRetryAttempts,
                          ex.what());
            if (attempt == kZmqRetryAttempts) {
                break;
            }
            std::this_thread::sleep_for(kZmqRetryBackoff);
        }
    }
    return false;
}

class SubscriberMonitor : public zmq::monitor_t {
  public:
    void start(zmq::socket_t& socket) {
        stop_flag_.store(false);
        sub_count_.store(0);
        monitor_thread_ = std::thread([this, &socket]() {
            try {
                init(socket,
                     "inproc://pub2_monitor",
                     ZMQ_EVENT_CONNECTED | ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_DISCONNECTED |
                         ZMQ_EVENT_CLOSED);
            } catch (const zmq::error_t& ex) {
                spdlog::warn("Failed to start subscriber monitor: {}", ex.what());
                return;
            }

            while (!stop_flag_.load()) {
                check_event(250);
            }
        });
    }

    void stop() {
        stop_flag_.store(true);
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    bool has_subscriber() const { return sub_count_.load() > 0; }

  protected:
    void on_event_connected(const zmq_event_t&, const char*) override {
        sub_count_.fetch_add(1);
    }
    void on_event_accepted(const zmq_event_t&, const char*) override {
        sub_count_.fetch_add(1);
    }
    void on_event_disconnected(const zmq_event_t&, const char*) override {
        sub_count_.fetch_sub(1);
    }
    void on_event_closed(const zmq_event_t&, const char*) override {
        sub_count_.fetch_sub(1);
    }

  private:
    std::atomic_int sub_count_{0};
    std::atomic_bool stop_flag_{false};
    std::thread monitor_thread_;
};

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Feature Extractor - consumes frames, runs SIFT, republishes"};
    std::string cli_env_path;
    std::string cli_log_level;
    bool send_annotated = false;
    app.add_option("--env", cli_env_path, "Path to the .env file (overrides DIST_ENV_PATH)");
    app.add_option("--log-level", cli_log_level, "Override log level (trace|debug|info|warn|error|critical)");
    app.add_flag("--annotated", send_annotated, "Enable sending annotated keypoint overlays");

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

    std::size_t max_queue_depth = kDefaultQueueDepth;
    if (config.extractor.queue_depth > 0) {
        max_queue_depth = static_cast<std::size_t>(config.extractor.queue_depth);
    } else {
        spdlog::warn("FEATURE_EXTRACTOR_QUEUE_DEPTH={} is invalid; using default {}",
                     config.extractor.queue_depth,
                     kDefaultQueueDepth);
    }

    dist::common::install_signal_handlers(g_keep_running);

    zmq::context_t context{1};
    zmq::socket_t subscriber{context, zmq::socket_type::sub};
    subscriber.set(zmq::sockopt::rcvtimeo, 500);
    subscriber.set(zmq::sockopt::linger, 0);
    subscriber.set(zmq::sockopt::subscribe, "");
    if (!connect_with_retry(subscriber, config.extractor.sub_endpoint, "SUB socket", false)) {
        return 1;
    }

    zmq::socket_t publisher{context, zmq::socket_type::pub};
    publisher.set(zmq::sockopt::sndhwm, 100);
    publisher.set(zmq::sockopt::sndtimeo, 1000);
    publisher.set(zmq::sockopt::linger, 0);
    struct MonitorGuard {
        SubscriberMonitor* monitor = nullptr;
        ~MonitorGuard() {
            if (monitor != nullptr) {
                monitor->stop();
            }
        }
    };
    auto subscriber_monitor = std::make_unique<SubscriberMonitor>();
    MonitorGuard monitor_guard{subscriber_monitor.get()};

    if (!bind_with_retry(publisher, config.extractor.pub_endpoint, "PUB socket")) {
        return 1;
    }
    subscriber_monitor->start(publisher);
    monitor_guard.monitor = subscriber_monitor.get();

    auto sift = cv::SIFT::create(
        config.extractor.sift_n_features > 0 ? config.extractor.sift_n_features : 0,
        3,
        config.extractor.sift_contrast_threshold,
        config.extractor.sift_edge_threshold,
        1.6);

    spdlog::info("[feature_extractor] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Listening on {}", config.extractor.sub_endpoint);
    spdlog::info("Publishing to {}", config.extractor.pub_endpoint);
    spdlog::info("Queue depth: {}", max_queue_depth);

    auto last_wait_log = std::chrono::steady_clock::now();
    struct ProcessedFrame {
        std::string header_json;
        std::vector<std::uint8_t> descriptors;
        std::vector<std::uint8_t> image;
        std::vector<std::uint8_t> annotated;
    };
    std::deque<ProcessedFrame> pending;

    const auto send_frame = [&](const ProcessedFrame& frame) -> bool {
        std::vector<zmq::message_t> multipart;
        multipart.emplace_back(frame.header_json);
        multipart.emplace_back(frame.descriptors.size());
        if (!frame.descriptors.empty()) {
            std::memcpy(multipart.back().data(),
                        frame.descriptors.data(),
                        frame.descriptors.size());
        }
        multipart.emplace_back(frame.image.size());
        std::memcpy(multipart.back().data(), frame.image.data(), frame.image.size());
        if (!frame.annotated.empty()) {
            multipart.emplace_back(frame.annotated.size());
            std::memcpy(multipart.back().data(),
                        frame.annotated.data(),
                        frame.annotated.size());
        }

        try {
            zmq::send_multipart(publisher, multipart, zmq::send_flags::none);
            return true;
        } catch (const zmq::error_t& ex) {
            if (ex.num() == EAGAIN) {
                spdlog::warn(
                    "Downstream consumer not keeping up on {} (queueing processed frame)",
                    config.extractor.pub_endpoint);
                return false;
            }
            spdlog::error("Failed to publish processed frame: {}", ex.what());
            return false;
        }
    };

    while (g_keep_running.load()) {
        while (!pending.empty()) {
            if (subscriber_monitor->has_subscriber() && send_frame(pending.front())) {
                pending.pop_front();
            } else {
                break;  // stop flushing if downstream still blocked
            }
        }

        zmq::message_t header_msg;
        zmq::message_t image_msg;

        try {
            if (!subscriber.recv(header_msg, zmq::recv_flags::none)) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_wait_log > std::chrono::seconds(5)) {
                    spdlog::info("Waiting for frames on {}", config.extractor.sub_endpoint);
                    last_wait_log = now;
                }
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
        cv::Mat image = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (image.empty()) {
            spdlog::warn("Failed to decode incoming frame {}", source_header.value("frame_id", -1));
            continue;
        }

        spdlog::info("Received frame {} ({} bytes)",
                     source_header.value("frame_id", -1),
                     encoded.size());

        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        sift->detectAndCompute(image, cv::noArray(), keypoints, descriptors);

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
        cv::drawKeypoints(image,
                          keypoints,
                          annotated,
                          cv::Scalar(0, 255, 0),
                          cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
        std::vector<std::uint8_t> annotated_bytes;
        if (send_annotated && !annotated.empty()) {
            cv::imencode(".png", annotated, annotated_bytes);
        }

        const auto frame_id = source_header.value("frame_id", -1);
        spdlog::info("Processed frame {} ({} keypoints)", frame_id, keypoints.size());

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
                         frame_id);
            continue;
        }

        ProcessedFrame processed{
            header.dump(), std::move(descriptor_bytes), std::move(encoded), std::move(annotated_bytes)};

        if (!subscriber_monitor->has_subscriber() || !send_frame(processed)) {
            if (pending.size() >= max_queue_depth) {
                spdlog::warn("Extractor queue full ({} frames); dropping oldest", max_queue_depth);
                pending.pop_front();
            }
            spdlog::warn("Queueing processed frame {} until logger is available", frame_id);
            pending.push_back(std::move(processed));
            if (kNoSubscriberBackoff.count() > 0) {
                std::this_thread::sleep_for(kNoSubscriberBackoff);
            }
        }
    }

    spdlog::info("Feature extractor shutting down");
    subscriber_monitor->stop();
    monitor_guard.monitor = nullptr;
    subscriber_monitor.reset();  // Destroy monitor before tearing down ZeroMQ to avoid shutdown hang.
    publisher.close();
    subscriber.close();
    context.close();
    spdlog::info("Feature extractor cleanup complete.");
    return 0;
}
