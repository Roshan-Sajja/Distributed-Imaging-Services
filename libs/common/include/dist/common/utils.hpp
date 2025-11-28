#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace dist::common {

// Convert string log level to spdlog level (defaults to info).
[[nodiscard]] spdlog::level::level_enum level_from_string(std::string_view value);

// Current UTC timestamp in ISO-8601 "YYYY-MM-DDTHH:MM:SSZ" format.
[[nodiscard]] std::string now_iso8601();

// Install SIGINT/SIGTERM handlers that flip the provided atomic flag to false.
// This lets every binary reuse the same shutdown plumbing.
void install_signal_handlers(std::atomic_bool& keep_running_flag);

}  // namespace dist::common
