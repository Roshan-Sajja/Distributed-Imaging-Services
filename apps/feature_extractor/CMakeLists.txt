add_executable(
    feature_extractor
    src/main.cpp)

target_link_libraries(
    feature_extractor
    PRIVATE
        dist::common
        CLI11::CLI11
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        ${OpenCV_LIBS}
        ${DIST_LIBZMQ_TARGET}
        dist::cppzmq)

set_common_warnings(feature_extractor)

