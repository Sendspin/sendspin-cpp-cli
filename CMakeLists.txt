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
# A player endpoint needs `player` (audio), `metadata` (what is playing, for logging
# and the `status` subcommand) and `controller`. That last one is not about driving
# *other* clients: `controller@v1` carries the transport verbs for the group this
# client is part of -- play, pause, seek, group volume -- which is exactly what a
# local CLI on a player needs, and which the `player` role does not have at all (it
# covers only this endpoint's own volume, mute and static delay). The
# display-oriented roles have no headless use. Declared without FORCE so
# `-DSENDSPIN_ENABLE_COLOR=ON` still wins.
set(SENDSPIN_ENABLE_PLAYER ON CACHE BOOL "Enable sendspin player role")
set(SENDSPIN_ENABLE_METADATA ON CACHE BOOL "Enable sendspin metadata role")
set(SENDSPIN_ENABLE_CONTROLLER ON CACHE BOOL "Enable sendspin controller role")
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

# Two of the error codes describe_error() names are Bonjour's alone: libavahi-compat-libdnssd
# declares neither kDNSServiceErr_ServiceNotRunning nor kDNSServiceErr_Timeout, so naming them
# unconditionally is what stops the Linux build compiling at all.
#
# They have to be probed rather than tested with the preprocessor. Both are enumerators of an
# anonymous enum rather than macros, so `#ifdef kDNSServiceErr_Timeout` is false even on macOS
# -- it would silently drop the two cases on every platform, which is worse than the break it
# was meant to fix. check_cxx_symbol_exists() cannot answer it either: it probes by taking the
# symbol's address, and an enumerator has none. Compiling a use of each is the only test left.
if(SENDSPIN_CLI_MDNS_ENABLED)
    include(CheckCXXSourceCompiles)

    # Empty on Apple, where dns_sd.h is in the SDK's default search path. Elsewhere it is the
    # same directory the mDNS sources are compiled against, so the probe reads the header the
    # build will really use rather than whichever one happens to be first on the system path.
    set(CMAKE_REQUIRED_INCLUDES "${SENDSPIN_CLI_DNS_SD_INCLUDE_DIR}")

    check_cxx_source_compiles(
        "#include <dns_sd.h>
         int main() { return kDNSServiceErr_ServiceNotRunning; }"
        SENDSPIN_CLI_HAVE_ERR_SERVICE_NOT_RUNNING)
    check_cxx_source_compiles(
        "#include <dns_sd.h>
         int main() { return kDNSServiceErr_Timeout; }"
        SENDSPIN_CLI_HAVE_ERR_TIMEOUT)

    unset(CMAKE_REQUIRED_INCLUDES)
endif()

# ==============================================================================
# Warnings
# ==============================================================================

# Off by default so a contributor on a newer compiler -- which invents diagnostics every
# release -- is not blocked by a warning that has nothing to do with the change they are
# making. CI passes -DSENDSPIN_CLI_WERROR=ON, which is what actually holds the line.
option(SENDSPIN_CLI_WERROR "Turn warnings from sendspin-cli's own sources into errors" OFF)

# Carried in a variable and applied per-target rather than set as a directory or global
# flag, because only our own three targets are ours to keep clean: the FetchContent'd
# sendspin and googletest trees build with these appended to whatever they already use, and
# ixwebsocket alone emits enough -Wunused-parameter to fail every build under -Werror.
set(SENDSPIN_CLI_WARNING_FLAGS -Wall -Wextra -Wpedantic)
if(SENDSPIN_CLI_WERROR)
    list(APPEND SENDSPIN_CLI_WARNING_FLAGS -Werror)
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
    src/control_client.cpp
    src/control_common.cpp
    src/control_socket.cpp
    src/config_file.cpp
    src/daemon.cpp
    src/key_value_file.cpp
    src/log.cpp
    src/mdns_common.cpp
    src/null_sink.cpp
    src/outbound.cpp
    src/pcm_volume.cpp
    src/player_listener.cpp
    src/state_store.cpp
    src/supported_formats.cpp
)

# One of these two, never both: they define the same MdnsService, and which one a build
# gets is a property of the host rather than a runtime choice. The pure parts of discovery
# -- URL construction, candidate selection -- are in mdns_common.cpp above, so they are
# compiled and tested in either configuration.
if(SENDSPIN_CLI_MDNS_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/mdns_dnssd.cpp)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_MDNS)
    # PRIVATE where SENDSPIN_CLI_HAVE_MDNS is PUBLIC: these two only ever reach a switch inside
    # mdns_dnssd.cpp, so nothing that links this archive has to be told about them.
    if(SENDSPIN_CLI_HAVE_ERR_SERVICE_NOT_RUNNING)
        target_compile_definitions(sendspin-cli-core PRIVATE
            SENDSPIN_CLI_HAVE_ERR_SERVICE_NOT_RUNNING)
    endif()
    if(SENDSPIN_CLI_HAVE_ERR_TIMEOUT)
        target_compile_definitions(sendspin-cli-core PRIVATE SENDSPIN_CLI_HAVE_ERR_TIMEOUT)
    endif()
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
target_compile_options(sendspin-cli-core PRIVATE ${SENDSPIN_CLI_WARNING_FLAGS})
target_compile_definitions(sendspin-cli-core PUBLIC
    SENDSPIN_CLI_VERSION="${PROJECT_VERSION}"
    SENDSPIN_CLI_LIB_TAG="${SENDSPIN_GIT_TAG}"
)

add_executable(sendspin-cli src/main.cpp)
target_link_libraries(sendspin-cli PRIVATE sendspin-cli-core)
target_compile_options(sendspin-cli PRIVATE ${SENDSPIN_CLI_WARNING_FLAGS})

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
        tests/config_file_test.cpp
        tests/control_test.cpp
        tests/daemon_test.cpp
        tests/device_spec_test.cpp
        tests/discovery_test.cpp
        tests/pcm_volume_test.cpp
        tests/state_store_test.cpp
        tests/supported_formats_test.cpp
    )
    target_link_libraries(sendspin-cli-tests PRIVATE sendspin-cli-core GTest::gtest_main)
    target_compile_options(sendspin-cli-tests PRIVATE ${SENDSPIN_CLI_WARNING_FLAGS})
    gtest_discover_tests(sendspin-cli-tests)
endif()
