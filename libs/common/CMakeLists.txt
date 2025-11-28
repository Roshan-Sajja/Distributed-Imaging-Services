add_library(
    dist_common STATIC
    src/version.cpp
    src/env_loader.cpp
    src/config.cpp
    src/utils.cpp)

add_library(dist::common ALIAS dist_common)

target_include_directories(dist_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Shared helpers are header-only aside from the compilation units above.
target_link_libraries(
    dist_common
    PUBLIC
        spdlog::spdlog_header_only)

target_compile_features(dist_common PUBLIC cxx_std_20)
set_common_warnings(dist_common)
