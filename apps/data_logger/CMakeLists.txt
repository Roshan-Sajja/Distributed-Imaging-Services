add_executable(
    data_logger
    src/main.cpp)

target_link_libraries(
    data_logger
    PRIVATE
        dist::common
        CLI11::CLI11
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
        ${DIST_LIBZMQ_TARGET}
        dist::cppzmq)

set_common_warnings(data_logger)

