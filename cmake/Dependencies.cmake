include(FetchContent)

set(DIST_CLI11_TAG v2.4.2)
set(DIST_SPDLOG_TAG v1.13.0)
set(DIST_CPPZMQ_TAG v4.10.0)
set(DIST_JSON_TAG v3.11.3)

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc imgcodecs features2d)
find_package(ZeroMQ QUIET)
find_package(SQLite3 REQUIRED)

if(ZeroMQ_FOUND)
    if(TARGET libzmq)
        set(DIST_LIBZMQ_TARGET libzmq)
    elseif(TARGET ZeroMQ::libzmq)
        set(DIST_LIBZMQ_TARGET ZeroMQ::libzmq)
    elseif(TARGET ZeroMQ::ZeroMQ)
        set(DIST_LIBZMQ_TARGET ZeroMQ::ZeroMQ)
    else()
        message(FATAL_ERROR "Unable to determine ZeroMQ target name after find_package")
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_ZEROMQ REQUIRED IMPORTED_TARGET libzmq)
    add_library(dist_libzmq INTERFACE)
    target_include_directories(dist_libzmq INTERFACE ${PC_ZEROMQ_INCLUDE_DIRS})
    target_link_libraries(dist_libzmq INTERFACE PkgConfig::PC_ZEROMQ)
    add_library(dist::libzmq ALIAS dist_libzmq)
    set(DIST_LIBZMQ_TARGET dist::libzmq)
endif()

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG ${DIST_CLI11_TAG}
    GIT_SHALLOW TRUE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG ${DIST_SPDLOG_TAG}
    GIT_SHALLOW TRUE)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG ${DIST_JSON_TAG}
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(CLI11 spdlog nlohmann_json)

set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "Disable cppzmq tests" FORCE)

FetchContent_Declare(
    cppzmq
    GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
    GIT_TAG ${DIST_CPPZMQ_TAG}
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(cppzmq)

add_library(dist_cppzmq INTERFACE)
target_link_libraries(dist_cppzmq INTERFACE cppzmq ${DIST_LIBZMQ_TARGET})
add_library(dist::cppzmq ALIAS dist_cppzmq)

