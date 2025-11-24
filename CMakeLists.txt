cmake_minimum_required(VERSION 3.26)

project(
    DistImagingServices
    VERSION 0.1.0
    DESCRIPTION "Distributed imaging pipeline (generator, extractor, logger)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(DIST_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

add_subdirectory(libs/common)
add_subdirectory(apps/image_generator)
add_subdirectory(apps/feature_extractor)
add_subdirectory(apps/data_logger)

