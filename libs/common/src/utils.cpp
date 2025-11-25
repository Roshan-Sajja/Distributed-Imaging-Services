#include "dist/common/utils.hpp"

#include <chrono>
#include <csignal>
#include <ctime>
#include <unordered_map>

namespace dist::common {

namespace {
std::atomic_bool* g_signal_flag = nullptr;

void handle_signal(int) {
    if (g_signal_flag != nullptr) {
        g_signal_flag->store(false);
    }
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

void install_signal_handlers(std::atomic_bool& keep_running_flag) {
    g_signal_flag = &keep_running_flag;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

}  // namespace dist::common
