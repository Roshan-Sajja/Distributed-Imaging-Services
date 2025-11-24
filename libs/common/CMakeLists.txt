add_library(
    dist_common STATIC
    src/version.cpp
    src/env_loader.cpp
    src/config.cpp)

add_library(dist::common ALIAS dist_common)

target_include_directories(dist_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(dist_common PUBLIC cxx_std_20)
set_common_warnings(dist_common)

