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
# sendspin-cli
# ==============================================================================

add_executable(sendspin-cli
    src/main.cpp
    src/cli.cpp
    src/audio_sink.cpp
    src/null_sink.cpp
    src/player_listener.cpp
)

target_include_directories(sendspin-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sendspin-cli PRIVATE sendspin)
target_compile_options(sendspin-cli PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sendspin-cli PRIVATE
    SENDSPIN_CLI_VERSION="${PROJECT_VERSION}"
    SENDSPIN_CLI_LIB_TAG="${SENDSPIN_GIT_TAG}"
)

message(STATUS "sendspin-cli ${PROJECT_VERSION} against sendspin-cpp ${SENDSPIN_GIT_TAG}")
