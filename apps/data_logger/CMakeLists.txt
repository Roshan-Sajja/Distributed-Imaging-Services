add_executable(data_logger
    src/main.cpp)

target_link_libraries(data_logger PRIVATE dist::common)
set_common_warnings(data_logger)

