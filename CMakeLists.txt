cmake_minimum_required(VERSION 3.16)

project(sendspin-cpp-cli
    VERSION 0.1.0
    DESCRIPTION "Headless CLI/daemon Sendspin audio player"
    HOMEPAGE_URL "https://github.com/Sendspin/sendspin-cpp"
    LANGUAGES CXX
)

# sendspin-cpp's host build declares `target_compile_features(sendspin PUBLIC
# cxx_std_20)`, so C++20 is the floor for anything that links it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ==============================================================================
# sendspin-cpp
# ==============================================================================

set(SENDSPIN_GIT_TAG "v0.7.0" CACHE STRING
    "sendspin-cpp git tag to build against")

# Roles are opt-out in the library and each one costs sources and dependencies.
# A player endpoint needs `player` (audio) and `metadata` (what is playing, for
# logging and a future `status` subcommand). `controller` is for driving *other*
# clients, which this daemon does not do; the display-oriented roles have no
# headless use. Declared without FORCE so `-DSENDSPIN_ENABLE_COLOR=ON` still wins.
set(SENDSPIN_ENABLE_PLAYER ON CACHE BOOL "Enable sendspin player role")
set(SENDSPIN_ENABLE_METADATA ON CACHE BOOL "Enable sendspin metadata role")
set(SENDSPIN_ENABLE_CONTROLLER OFF CACHE BOOL "Enable sendspin controller role")
set(SENDSPIN_ENABLE_COLOR OFF CACHE BOOL "Enable sendspin color role")
set(SENDSPIN_ENABLE_ARTWORK OFF CACHE BOOL "Enable sendspin artwork role")
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "Enable sendspin visualizer role")

# The library only builds its own examples when it is the top-level project, which
# it is not under FetchContent. Pinned off anyway so a future CMake change there
# cannot start pulling example-only dependencies (FTXUI, PortAudio) into our build.
set(BUILD_EXAMPLES OFF CACHE BOOL "Build sendspin-cpp's own examples")

include(FetchContent)
FetchContent_Declare(
    sendspin
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG        ${SENDSPIN_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sendspin)

# ==============================================================================
# Audio backends
# ==============================================================================

# ALSA is optional and auto-detected: a host with libasound gets the backend, and
# macOS or a device-less container still builds on null/stdout alone. Turning this
# OFF forces it out even where libasound exists.
option(SENDSPIN_CLI_WITH_ALSA "Build the ALSA output backend if libasound is available" ON)

set(SENDSPIN_CLI_BACKENDS null stdout)
set(SENDSPIN_CLI_ALSA_ENABLED OFF)

if(SENDSPIN_CLI_WITH_ALSA)
    find_package(ALSA)
    if(ALSA_FOUND)
        set(SENDSPIN_CLI_ALSA_ENABLED ON)
        list(APPEND SENDSPIN_CLI_BACKENDS alsa)
    else()
        message(STATUS
            "sendspin-cli: libasound not found -- building without the ALSA backend. "
            "Install alsa-lib-devel (Fedora) or libasound2-dev (Debian) to enable it.")
    endif()
endif()

# ==============================================================================
# sendspin-cli
# ==============================================================================

# Everything except main() lives in a library, so the tests can link the parser and the
# -o spec resolver without dragging in a process entry point. The compile definitions and
# the include directory are PUBLIC because the headers use them: cli.h picks its default
# -o device off SENDSPIN_CLI_HAVE_ALSA, so a test compiled without it would see a
# different Options than the binary does.
add_library(sendspin-cli-core STATIC
    src/cli.cpp
    src/audio_sink.cpp
    src/null_sink.cpp
    src/player_listener.cpp
)

if(SENDSPIN_CLI_ALSA_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/alsa_sink.cpp)
    target_link_libraries(sendspin-cli-core PUBLIC ALSA::ALSA)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_ALSA)
endif()

target_include_directories(sendspin-cli-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sendspin-cli-core PUBLIC sendspin)
target_compile_options(sendspin-cli-core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sendspin-cli-core PUBLIC
    SENDSPIN_CLI_VERSION="${PROJECT_VERSION}"
    SENDSPIN_CLI_LIB_TAG="${SENDSPIN_GIT_TAG}"
)

add_executable(sendspin-cli src/main.cpp)
target_link_libraries(sendspin-cli PRIVATE sendspin-cli-core)
target_compile_options(sendspin-cli PRIVATE -Wall -Wextra -Wpedantic)

list(JOIN SENDSPIN_CLI_BACKENDS ", " SENDSPIN_CLI_BACKEND_LIST)
message(STATUS "sendspin-cli ${PROJECT_VERSION} against sendspin-cpp ${SENDSPIN_GIT_TAG}")
message(STATUS "sendspin-cli audio backends: ${SENDSPIN_CLI_BACKEND_LIST}")

# ==============================================================================
# Tests
# ==============================================================================

# Defaults ON only when this is the top-level project: a parent that vendors sendspin-cli
# via FetchContent should not pay to fetch and build GoogleTest, nor find our test names
# in its own CTest run. (PROJECT_IS_TOP_LEVEL would say this more directly, but it wants
# CMake 3.21 and this project's floor is 3.16.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SENDSPIN_CLI_TESTS_DEFAULT ON)
else()
    set(SENDSPIN_CLI_TESTS_DEFAULT OFF)
endif()
option(SENDSPIN_CLI_BUILD_TESTS "Build the unit tests" ${SENDSPIN_CLI_TESTS_DEFAULT})

# Pinned to a tag for the same reason SENDSPIN_GIT_TAG is: a moving branch turns an
# unrelated upstream commit into a build failure here.
set(GOOGLETEST_GIT_TAG "v1.17.0" CACHE STRING
    "GoogleTest git tag to build the tests against")

if(SENDSPIN_CLI_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        ${GOOGLETEST_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest alongside our own targets")
    set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock")
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    add_executable(sendspin-cli-tests
        tests/cli_test.cpp
        tests/device_spec_test.cpp
    )
    target_link_libraries(sendspin-cli-tests PRIVATE sendspin-cli-core GTest::gtest_main)
    target_compile_options(sendspin-cli-tests PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(sendspin-cli-tests)
endif()
