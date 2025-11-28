#include "dist/common/env_loader.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace dist::common {

namespace {
[[nodiscard]] bool is_comment_or_empty(std::string_view line) {
    for (char c : line) {
        if (c == '#') {
            return true;  // treat inline comments as whole-line comments
        }
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}
}  // namespace

bool EnvLoader::load_from_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::ostringstream contents;
    contents << in.rdbuf();
    return load_from_string(contents.str());
}

bool EnvLoader::load_from_string(std::string_view buffer) {
    std::istringstream stream{std::string(buffer)};
    std::string line;

    while (std::getline(stream, line)) {
        if (is_comment_or_empty(line)) {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (!key.empty()) {
            values_[key] = value;
        }
    }

    return true;
}

bool EnvLoader::load_from_env() {
#if defined(_WIN32)
    char** env = _environ;
#else
    char** env = ::environ;
#endif
    if (env == nullptr) {
        return false;
    }

    bool loaded = false;
    for (char** current = env; *current != nullptr; ++current) {
        std::string_view entry(*current);
        const auto pos = entry.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string key = trim(entry.substr(0, pos));
        std::string value = std::string(entry.substr(pos + 1));
        if (!key.empty()) {
            values_[std::move(key)] = std::move(value);
            loaded = true;
        }
    }
    return loaded;
}

std::optional<std::string> EnvLoader::get(std::string_view key) const {
    if (auto it = values_.find(std::string(key)); it != values_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string EnvLoader::get_or(std::string_view key,
                              std::string_view fallback) const {
    if (auto value = get(key)) {
        return *value;
    }
    return std::string(fallback);
}

std::string EnvLoader::trim(std::string_view in) {
    const auto is_ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)); };
    size_t start = 0;
    while (start < in.size() && is_ws(in[start])) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && is_ws(in[end - 1])) {
        --end;
    }
    return std::string(in.substr(start, end - start));
}

}  // namespace dist::common
