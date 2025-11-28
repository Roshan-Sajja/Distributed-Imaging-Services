#include "dist/common/version.hpp"

namespace dist::common {

// The build embeds this string so binaries can print their origin.
std::string_view version() {
    return "0.1.0";
}

}  // namespace dist::common