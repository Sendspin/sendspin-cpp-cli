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
# it is not under FetchContent. Pinned off anyway so a future CMake change there cannot start
# building programs we do not ship, and pulling in what only they need (FTXUI). This says
# nothing about PortAudio: our own backend depends on it deliberately, and finds it itself.
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

# PortAudio is optional and auto-detected the same way, and is what makes macOS audible --
# there is no ALSA there at all. Turning this OFF forces it out even where libportaudio exists.
option(SENDSPIN_CLI_WITH_PORTAUDIO
    "Build the PortAudio output backend if libportaudio is available" ON)

set(SENDSPIN_CLI_BACKENDS null stdout)
set(SENDSPIN_CLI_ALSA_ENABLED OFF)
set(SENDSPIN_CLI_PORTAUDIO_ENABLED OFF)

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

if(SENDSPIN_CLI_WITH_PORTAUDIO)
    # PortAudio ships no CMake config module, so pkg-config is the only portable way to find
    # it. IMPORTED_TARGET is what makes that usable: on macOS the .pc file is the only thing
    # that knows the CoreAudio/AudioToolbox/AudioUnit frameworks have to be linked too, and
    # PkgConfig::PORTAUDIO carries them through where raw ${PORTAUDIO_LIBRARIES} would not.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
    endif()

    if(TARGET PkgConfig::PORTAUDIO)
        set(SENDSPIN_CLI_PORTAUDIO_ENABLED ON)
        list(APPEND SENDSPIN_CLI_BACKENDS portaudio)
    elseif(NOT PkgConfig_FOUND)
        # Named separately from "no portaudio": a host missing pkg-config would otherwise be
        # told to install a library it already has.
        message(STATUS
            "sendspin-cli: pkg-config not found -- building without the PortAudio backend. "
            "Install pkgconf (brew install pkgconf) or pkg-config, alongside portaudio, "
            "to enable it.")
    else()
        message(STATUS
            "sendspin-cli: libportaudio not found -- building without the PortAudio backend. "
            "Install portaudio (brew install portaudio) or portaudio19-dev (Debian) "
            "to enable it.")
    endif()
endif()

# ==============================================================================
# mDNS
# ==============================================================================

# Optional and auto-detected, like the audio backends: a host with dns_sd.h gets discovery
# and advertisement, and one without still builds and runs -- it just has to be told where
# its server is. Turning this OFF forces it out even where dns_sd.h exists.
#
# Bonjour on macOS, libavahi-compat-libdnssd on Linux. An Avahi-native backend would avoid
# the compat shim and recover from a daemon restart more directly, but only on Linux; it is
# tracked as its own roadmap item rather than replacing this one.
option(SENDSPIN_CLI_WITH_MDNS "Build mDNS advertisement/discovery if dns_sd.h is available" ON)

set(SENDSPIN_CLI_MDNS_ENABLED OFF)
set(SENDSPIN_CLI_MDNS_DESCRIPTION "none")

if(SENDSPIN_CLI_WITH_MDNS)
    if(APPLE)
        # dns_sd lives in libSystem here, so there is nothing to find and nothing to link.
        set(SENDSPIN_CLI_MDNS_ENABLED ON)
        set(SENDSPIN_CLI_MDNS_DESCRIPTION "dns_sd (Bonjour, built in)")
    else()
        find_path(SENDSPIN_CLI_DNS_SD_INCLUDE_DIR dns_sd.h)
        find_library(SENDSPIN_CLI_DNS_SD_LIBRARY dns_sd)
        if(SENDSPIN_CLI_DNS_SD_INCLUDE_DIR AND SENDSPIN_CLI_DNS_SD_LIBRARY)
            set(SENDSPIN_CLI_MDNS_ENABLED ON)
            set(SENDSPIN_CLI_MDNS_DESCRIPTION "dns_sd (${SENDSPIN_CLI_DNS_SD_LIBRARY})")
        else()
            message(STATUS
                "sendspin-cli: dns_sd.h not found -- building without mDNS. Install "
                "libavahi-compat-libdnssd-dev (Debian/Ubuntu) or avahi-compat-libdns_sd-devel "
                "(Fedora) to enable it; without it the player cannot be discovered and -s "
                "mdns: is refused at parse time.")
        endif()
    endif()
endif()

# ==============================================================================
# sendspin-cli
# ==============================================================================

# Everything except main() lives in a library, so the tests can link the parser and the
# -o spec resolver without dragging in a process entry point. The compile definitions and
# the include directory are PUBLIC because the headers use them: cli.h picks its default
# -o device off SENDSPIN_CLI_HAVE_ALSA and SENDSPIN_CLI_HAVE_PORTAUDIO, so a test compiled
# without them would see a different Options than the binary does.
#
# The backend libraries are PUBLIC for a separate reason: a static archive does not record
# its own dependencies, so anything linking this one has to be handed them too.
add_library(sendspin-cli-core STATIC
    src/cli.cpp
    src/audio_sink.cpp
    src/last_server.cpp
    src/mdns_common.cpp
    src/null_sink.cpp
    src/outbound.cpp
    src/pcm_volume.cpp
    src/player_listener.cpp
    src/supported_formats.cpp
)

# One of these two, never both: they define the same MdnsService, and which one a build
# gets is a property of the host rather than a runtime choice. The pure parts of discovery
# -- URL construction, candidate selection -- are in mdns_common.cpp above, so they are
# compiled and tested in either configuration.
if(SENDSPIN_CLI_MDNS_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/mdns_dnssd.cpp)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_MDNS)
    if(NOT APPLE)
        target_include_directories(sendspin-cli-core PRIVATE ${SENDSPIN_CLI_DNS_SD_INCLUDE_DIR})
        target_link_libraries(sendspin-cli-core PUBLIC ${SENDSPIN_CLI_DNS_SD_LIBRARY})
    endif()
else()
    target_sources(sendspin-cli-core PRIVATE src/mdns_null.cpp)
endif()

if(SENDSPIN_CLI_ALSA_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/alsa_sink.cpp)
    target_link_libraries(sendspin-cli-core PUBLIC ALSA::ALSA)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_ALSA)
endif()

if(SENDSPIN_CLI_PORTAUDIO_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/portaudio_sink.cpp)
    target_link_libraries(sendspin-cli-core PUBLIC PkgConfig::PORTAUDIO)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_PORTAUDIO)
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
message(STATUS "sendspin-cli mDNS: ${SENDSPIN_CLI_MDNS_DESCRIPTION}")

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
        tests/discovery_test.cpp
        tests/last_server_test.cpp
        tests/pcm_volume_test.cpp
        tests/supported_formats_test.cpp
    )
    target_link_libraries(sendspin-cli-tests PRIVATE sendspin-cli-core GTest::gtest_main)
    target_compile_options(sendspin-cli-tests PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(sendspin-cli-tests)
endif()
