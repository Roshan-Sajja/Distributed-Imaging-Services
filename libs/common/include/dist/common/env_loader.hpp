#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace dist::common {

class EnvLoader {
  public:
    using EnvMap = std::unordered_map<std::string, std::string>;

    EnvLoader() = default;

    [[nodiscard]] bool load_from_file(const std::filesystem::path& path);
    [[nodiscard]] bool load_from_string(std::string_view buffer);

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    [[nodiscard]] std::string get_or(std::string_view key,
                                     std::string_view fallback) const;

  private:
    static std::string trim(std::string_view in);
    EnvMap values_;
};

}  // namespace dist::common

