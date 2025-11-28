#pragma once

#include <string_view>

namespace dist::common {

// Semantic version string baked into the binaries.
[[nodiscard]] std::string_view version();

}  // namespace dist::common

