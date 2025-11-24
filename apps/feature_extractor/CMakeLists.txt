add_executable(feature_extractor
    src/main.cpp)

target_link_libraries(feature_extractor PRIVATE dist::common)
set_common_warnings(feature_extractor)

