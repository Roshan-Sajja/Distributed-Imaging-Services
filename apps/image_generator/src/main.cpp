#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/utils.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic_bool g_keep_running{true};
constexpr std::size_t kMaxPayloadBytes = 50 * 1024 * 1024;  // 50 MB safety cap
constexpr auto kNoSubscriberBackoff = 500ms;               // Slow down when nobody is listening
constexpr std::size_t kDefaultQueueDepth = 100;
constexpr int kZmqRetryAttempts = 3;
constexpr auto kZmqRetryBackoff = 1s;

bool bind_with_retry(zmq::socket_t& socket, const std::string& endpoint) {
    for (int attempt = 1; attempt <= kZmqRetryAttempts; ++attempt) {
        try {
            socket.bind(endpoint);
            return true;
        } catch (const zmq::error_t& ex) {
            spdlog::error(
                "Unable to bind PUB socket to {} (attempt {}/{}): {}. Is another instance running "
                "on this endpoint?",
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
                     "inproc://pub_monitor",
                     ZMQ_EVENT_ACCEPTED | ZMQ_EVENT_CONNECTED | ZMQ_EVENT_DISCONNECTED |
                         ZMQ_EVENT_CLOSED);
            } catch (const zmq::error_t& ex) {
                spdlog::warn("Failed to start socket monitor: {}", ex.what());
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

std::vector<fs::path> collect_images(const fs::path& dir) {
    std::vector<fs::path> images;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return images;
    }

    const std::vector<std::string> extensions{".png", ".jpg", ".jpeg", ".bmp", ".tif",
                                              ".tiff"};

    const auto iequals = [](const std::string& lhs, const std::string& rhs) {
        return lhs.size() == rhs.size() &&
               std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        const auto match =
            std::find_if(extensions.begin(), extensions.end(), [&](const std::string& e) {
                return iequals(ext, e);
            });
        if (match != extensions.end()) {
            images.push_back(entry.path());
        }
    }

    std::sort(images.begin(), images.end());
    return images;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Image Generator - publishes frames via ZeroMQ"};
    std::string cli_env_path;
    std::string cli_log_level;
    bool run_once = false;
    app.add_option("--env", cli_env_path, "Path to the .env file (overrides DIST_ENV_PATH)");
    app.add_option("--log-level", cli_log_level, "Override log level (trace|debug|info|warn|error|critical)");
    app.add_flag("--once", run_once, "Publish the dataset a single time instead of looping");

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

    spdlog::info("[image_generator] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Input directory: {}", config.generator.input_dir.string());
    spdlog::info("Publish endpoint: {}", config.generator.pub_endpoint);
    spdlog::info("Loop delay: {} ms", config.generator.loop_delay_ms);

    std::size_t max_queue_depth = kDefaultQueueDepth;
    if (config.generator.queue_depth > 0) {
        max_queue_depth = static_cast<std::size_t>(config.generator.queue_depth);
    } else {
        spdlog::warn("IMAGE_GENERATOR_QUEUE_DEPTH={} is invalid; using default {}",
                     config.generator.queue_depth,
                     kDefaultQueueDepth);
    }
    spdlog::info("Queue depth: {}", max_queue_depth);

    struct MonitorGuard {
        SubscriberMonitor* monitor = nullptr;
        ~MonitorGuard() {
            if (monitor != nullptr) {
                monitor->stop();
            }
        }
    };

    auto monitor = std::make_unique<SubscriberMonitor>();
    MonitorGuard monitor_guard{monitor.get()};
    auto images = collect_images(config.generator.input_dir);
    if (images.empty()) {
        spdlog::error("No readable images found under {}", config.generator.input_dir.string());
        return 1;
    }

    dist::common::install_signal_handlers(g_keep_running);

    zmq::context_t context{1};
    zmq::socket_t publisher{context, zmq::socket_type::pub};
    publisher.set(zmq::sockopt::sndhwm, 10);
    publisher.set(zmq::sockopt::sndtimeo, 1000);
    if (!bind_with_retry(publisher, config.generator.pub_endpoint)) {
        return 1;
    }
    monitor->start(publisher);
    monitor_guard.monitor = monitor.get();

    if (config.generator.start_delay_ms > 0) {
        spdlog::info("Waiting {} ms for subscribers to connect...",
                     config.generator.start_delay_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(config.generator.start_delay_ms));
    }
    if (config.generator.subscriber_wait_ms > 0) {
        spdlog::info("Waiting up to {} ms for at least one subscriber...",
                     config.generator.subscriber_wait_ms);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config.generator.subscriber_wait_ms);
        while (!monitor->has_subscriber() && std::chrono::steady_clock::now() < deadline &&
               g_keep_running.load()) {
            std::this_thread::sleep_for(50ms);
        }
        if (!monitor->has_subscriber()) {
            spdlog::warn("No subscribers detected before timeout; initial frames may be dropped");
        } else {
            spdlog::info("Subscriber detected, starting publish loop");
        }
    }

    std::size_t frame_id = 0;
    std::size_t loop_iteration = 0;
    const auto delay = std::chrono::milliseconds(config.generator.loop_delay_ms);
    const auto heartbeat_interval = std::chrono::milliseconds(config.generator.heartbeat_ms);
    auto last_heartbeat = std::chrono::steady_clock::now();
    std::deque<std::pair<std::string, std::vector<uchar>>> pending_frames;

    while (g_keep_running.load()) {
        if (monitor->has_subscriber() && !pending_frames.empty()) {
            spdlog::info("Flushing {} queued frames to new subscriber", pending_frames.size());
            while (!pending_frames.empty() && monitor->has_subscriber()) {
                auto [header_str, encoded] = std::move(pending_frames.front());
                pending_frames.pop_front();
                zmq::message_t header_msg(header_str);
                zmq::message_t payload_msg(encoded.size());
                std::memcpy(payload_msg.data(), encoded.data(), encoded.size());
                try {
                    publisher.send(header_msg, zmq::send_flags::sndmore);
                    publisher.send(payload_msg, zmq::send_flags::none);
                } catch (const zmq::error_t& ex) {
                    spdlog::warn("Failed to flush queued frame: {}", ex.what());
                    break;
                }
            }
        }

        for (const auto& image_path : images) {
            if (!g_keep_running.load()) {
                break;
            }

            const auto frame = cv::imread(image_path.string(), cv::IMREAD_COLOR);
            if (frame.empty()) {
                spdlog::warn("Failed to decode image {}", image_path.string());
                continue;
            }

            std::vector<uchar> encoded;
            if (!cv::imencode(".png", frame, encoded)) {
                spdlog::warn("Failed to encode image {}", image_path.string());
                continue;
            }
            if (encoded.size() > kMaxPayloadBytes) {
                spdlog::warn("Encoded image {} is too large ({} bytes > {}), skipping",
                             image_path.string(),
                             encoded.size(),
                             kMaxPayloadBytes);
                continue;
            }

            nlohmann::json header{
                {"frame_id", frame_id},
                {"loop_iteration", loop_iteration},
                {"timestamp", dist::common::now_iso8601()},
                {"filename", image_path.filename().string()},
                {"width", frame.cols},
                {"height", frame.rows},
                {"channels", frame.channels()},
                {"encoding", "png"},
                {"bytes", encoded.size()},
            };

            spdlog::debug("Header: {}", header.dump());

            const std::string header_str = header.dump();

            if (!monitor->has_subscriber()) {
                if (pending_frames.size() >= max_queue_depth) {
                    spdlog::warn("Queue full ({} frames); dropping oldest queued frame", max_queue_depth);
                    pending_frames.pop_front();
                }
                pending_frames.emplace_back(header_str, std::move(encoded));
                spdlog::warn("No subscriber present; queueing frame {}", frame_id);
                if (kNoSubscriberBackoff.count() > 0) {
                    std::this_thread::sleep_for(kNoSubscriberBackoff);
                }
            } else {
                zmq::message_t header_msg(header_str);
                zmq::message_t payload_msg(encoded.size());
                std::memcpy(payload_msg.data(), encoded.data(), encoded.size());

                try {
                    publisher.send(header_msg, zmq::send_flags::sndmore);
                    publisher.send(payload_msg, zmq::send_flags::none);
                } catch (const zmq::error_t& ex) {
                    spdlog::error("ZeroMQ send failed: {}", ex.what());
                    return 1;
                }

                spdlog::info("Published frame {} ({} bytes)", frame_id, encoded.size());
            }

            ++frame_id;

            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }

            const auto now = std::chrono::steady_clock::now();
            if (heartbeat_interval.count() > 0 &&
                now - last_heartbeat >= heartbeat_interval) {
                spdlog::info("Heartbeat: frames sent={}, loop_iteration={}",
                             frame_id,
                             loop_iteration);
                last_heartbeat = now;
            }
        }

        if (run_once) {
            break;
        }
        ++loop_iteration;
    }

    spdlog::info("Generator shutting down (frames sent: {})", frame_id);
    monitor->stop();
    monitor_guard.monitor = nullptr;
    monitor.reset();  // Destroy monitor before tearing down ZeroMQ to avoid shutdown hang.
    publisher.close();
    context.close();
    spdlog::info("Generator cleanup complete.");
    return 0;
}
