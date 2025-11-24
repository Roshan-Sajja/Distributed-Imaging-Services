#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dist/common/config.hpp"
#include "dist/common/env_loader.hpp"
#include "dist/common/version.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic_bool g_keep_running{true};

void handle_signal(int) {
    g_keep_running.store(false);
}

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

std::string now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buffer);
}

}  // namespace

spdlog::level::level_enum level_from_string(std::string_view value) {
    static const std::unordered_map<std::string_view, spdlog::level::level_enum> map{
        {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug},
        {"info", spdlog::level::info},   {"warn", spdlog::level::warn},
        {"error", spdlog::level::err},   {"critical", spdlog::level::critical},
    };

    if (auto it = map.find(value); it != map.end()) {
        return it->second;
    }
    return spdlog::level::info;
}

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

    spdlog::set_level(level_from_string(resolved_level));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::info("[image_generator] Dist Imaging Services v{}", dist::common::version());
    spdlog::info("Input directory: {}", config.generator.input_dir.string());
    spdlog::info("Publish endpoint: {}", config.generator.pub_endpoint);
    spdlog::info("Loop delay: {} ms", config.generator.loop_delay_ms);

    auto images = collect_images(config.generator.input_dir);
    if (images.empty()) {
        spdlog::error("No readable images found under {}", config.generator.input_dir.string());
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    zmq::context_t context{1};
    zmq::socket_t publisher{context, zmq::socket_type::pub};
    publisher.set(zmq::sockopt::sndhwm, 10);
    publisher.bind(config.generator.pub_endpoint);

    std::size_t frame_id = 0;
    std::size_t loop_iteration = 0;
    const auto delay = std::chrono::milliseconds(config.generator.loop_delay_ms);

    while (g_keep_running.load()) {
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

            nlohmann::json header{
                {"frame_id", frame_id},
                {"loop_iteration", loop_iteration},
                {"timestamp", now_iso8601()},
                {"filename", image_path.filename().string()},
                {"width", frame.cols},
                {"height", frame.rows},
                {"channels", frame.channels()},
                {"encoding", "png"},
                {"bytes", encoded.size()},
            };

            spdlog::debug("Header: {}", header.dump());

            zmq::message_t header_msg(header.dump());
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
            ++frame_id;

            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }

        if (run_once) {
            break;
        }
        ++loop_iteration;
    }

    spdlog::info("Generator shutting down (frames sent: {})", frame_id);
    return 0;
}

