add_executable(image_generator
    src/main.cpp)

target_link_libraries(image_generator PRIVATE dist::common)
set_common_warnings(image_generator)

