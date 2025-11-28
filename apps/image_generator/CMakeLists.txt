add_executable(
    image_generator
    src/main.cpp)

# Link against shared utility libs plus runtime dependencies.
target_link_libraries(
    image_generator
    PRIVATE
        dist::common
        CLI11::CLI11
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        ${OpenCV_LIBS}
        ${DIST_LIBZMQ_TARGET}
        dist::cppzmq)

target_compile_definitions(
    image_generator
    PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

set_common_warnings(image_generator)

