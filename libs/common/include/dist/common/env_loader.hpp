#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace dist::common {

// Thin parser for dotenv-style configuration files used by all apps.
class EnvLoader {
  public:
    using EnvMap = std::unordered_map<std::string, std::string>;

    EnvLoader() = default;

    // Read key/value pairs from disk (no exception is thrown on failure).
    [[nodiscard]] bool load_from_file(const std::filesystem::path& path);
    // Parse the given buffer directly (used by tests or in-memory configs).
    [[nodiscard]] bool load_from_string(std::string_view buffer);

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    [[nodiscard]] std::string get_or(std::string_view key,
                                     std::string_view fallback) const;

  private:
    static std::string trim(std::string_view in);
    EnvMap values_;
};

}  // namespace dist::common

