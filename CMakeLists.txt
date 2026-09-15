cmake_minimum_required(VERSION 3.16)

project(sendspin-cpp-cli
    VERSION 0.1.6
    DESCRIPTION "Headless CLI/daemon Sendspin audio player"
    HOMEPAGE_URL "https://github.com/Sendspin/sendspin-cpp"
    LANGUAGES CXX
)

# sendspin-cpp requires C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Top-level only (PROJECT_IS_TOP_LEVEL needs 3.21): install rules and tests are skipped when vendored.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SENDSPIN_CLI_IS_TOP_LEVEL ON)
else()
    set(SENDSPIN_CLI_IS_TOP_LEVEL OFF)
endif()

# sendspin-cpp

set(SENDSPIN_GIT_TAG "v0.7.2" CACHE STRING
    "sendspin-cpp git tag to build against")

# player, metadata and controller (group transport verbs); no FORCE, so -D overrides still win.
set(SENDSPIN_ENABLE_PLAYER ON CACHE BOOL "Enable sendspin player role")
set(SENDSPIN_ENABLE_METADATA ON CACHE BOOL "Enable sendspin metadata role")
set(SENDSPIN_ENABLE_CONTROLLER ON CACHE BOOL "Enable sendspin controller role")
set(SENDSPIN_ENABLE_COLOR OFF CACHE BOOL "Enable sendspin color role")
set(SENDSPIN_ENABLE_ARTWORK OFF CACHE BOOL "Enable sendspin artwork role")
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "Enable sendspin visualizer role")

# Never build the library's examples.
set(BUILD_EXAMPLES OFF CACHE BOOL "Build sendspin-cpp's own examples")

# Keep IXWebSocket's install rules out of our payload. No FORCE: reconfigure old build trees from scratch.
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Install IXWebSocket alongside our own targets")

include(FetchContent)
FetchContent_Declare(
    sendspin
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG        ${SENDSPIN_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sendspin)

# Audio backends

# Each backend is auto-detected; OFF forces it out.
option(SENDSPIN_CLI_WITH_ALSA "Build the ALSA output backend if libasound is available" ON)

option(SENDSPIN_CLI_WITH_PORTAUDIO
    "Build the PortAudio output backend if libportaudio is available" ON)

# Native sound-server backends; -o alsa:pulse and -o alsa:pipewire work without them.
option(SENDSPIN_CLI_WITH_PULSE
    "Build the PulseAudio output backend if libpulse is available" ON)
option(SENDSPIN_CLI_WITH_PIPEWIRE
    "Build the PipeWire output backend if libpipewire is available" ON)

set(SENDSPIN_CLI_BACKENDS null stdout)
set(SENDSPIN_CLI_ALSA_ENABLED OFF)
set(SENDSPIN_CLI_PORTAUDIO_ENABLED OFF)
set(SENDSPIN_CLI_PULSE_ENABLED OFF)
set(SENDSPIN_CLI_PIPEWIRE_ENABLED OFF)

# PW_KEY_TARGET_OBJECT arrived in 0.3.64; older versions silently ignore the node.
set(SENDSPIN_CLI_PIPEWIRE_MIN_VERSION 0.3.64)

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
    # No CMake module; IMPORTED_TARGET carries macOS framework links through.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
    endif()

    if(TARGET PkgConfig::PORTAUDIO)
        set(SENDSPIN_CLI_PORTAUDIO_ENABLED ON)
        list(APPEND SENDSPIN_CLI_BACKENDS portaudio)
    elseif(NOT PkgConfig_FOUND)
        # Distinct from "no portaudio", so a missing pkg-config is named.
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

# Found via pkg-config like PortAudio, and asked for again so disabling PortAudio does not skip them.
if(SENDSPIN_CLI_WITH_PULSE OR SENDSPIN_CLI_WITH_PIPEWIRE)
    find_package(PkgConfig QUIET)
endif()

if(SENDSPIN_CLI_WITH_PULSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse)
    endif()

    if(TARGET PkgConfig::PULSE)
        set(SENDSPIN_CLI_PULSE_ENABLED ON)
        list(APPEND SENDSPIN_CLI_BACKENDS pulse)
    elseif(NOT PkgConfig_FOUND)
        message(STATUS
            "sendspin-cli: pkg-config not found -- building without the PulseAudio backend. "
            "Install pkgconf or pkg-config, alongside libpulse, to enable it.")
    else()
        # Suggest the alsa: route only where this build has ALSA.
        set(SENDSPIN_CLI_PULSE_HINT "")
        if(SENDSPIN_CLI_ALSA_ENABLED)
            set(SENDSPIN_CLI_PULSE_HINT
                " A PulseAudio server stays reachable as -o alsa:pulse without it.")
        endif()
        message(STATUS
            "sendspin-cli: libpulse not found -- building without the PulseAudio backend. "
            "Install pulseaudio-libs-devel (Fedora) or libpulse-dev (Debian) to enable "
            "it.${SENDSPIN_CLI_PULSE_HINT}")
    endif()
endif()

if(SENDSPIN_CLI_WITH_PIPEWIRE)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET
            libpipewire-0.3>=${SENDSPIN_CLI_PIPEWIRE_MIN_VERSION})
    endif()

    if(TARGET PkgConfig::PIPEWIRE)
        set(SENDSPIN_CLI_PIPEWIRE_ENABLED ON)
        list(APPEND SENDSPIN_CLI_BACKENDS pipewire)
    elseif(NOT PkgConfig_FOUND)
        message(STATUS
            "sendspin-cli: pkg-config not found -- building without the PipeWire backend. "
            "Install pkgconf or pkg-config, alongside libpipewire, to enable it.")
    else()
        set(SENDSPIN_CLI_PIPEWIRE_HINT "")
        if(SENDSPIN_CLI_ALSA_ENABLED)
            set(SENDSPIN_CLI_PIPEWIRE_HINT
                " A PipeWire server stays reachable as -o alsa:pipewire without it.")
        endif()
        message(STATUS
            "sendspin-cli: libpipewire-0.3 >= ${SENDSPIN_CLI_PIPEWIRE_MIN_VERSION} not found -- "
            "building without the PipeWire backend. Install pipewire-devel (Fedora) or "
            "libpipewire-0.3-dev (Debian) to enable it.${SENDSPIN_CLI_PIPEWIRE_HINT}")
    endif()
endif()

# mDNS

# Auto-detected: Bonjour on macOS, libavahi-compat-libdnssd on Linux. OFF forces it out.
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

# Bonjour-only error enumerators, probed by compiling a use: #ifdef cannot see enumerators.
if(SENDSPIN_CLI_MDNS_ENABLED)
    include(CheckCXXSourceCompiles)

    # The same dns_sd.h include directory the build uses.
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

# Warnings

# Off by default for contributors on newer compilers; CI turns it on.
option(SENDSPIN_CLI_WERROR "Turn warnings from sendspin-cli's own sources into errors" OFF)

# Applied per target: FetchContent'd trees (ixwebsocket especially) would fail under -Werror.
set(SENDSPIN_CLI_WARNING_FLAGS -Wall -Wextra -Wpedantic)
if(SENDSPIN_CLI_WERROR)
    list(APPEND SENDSPIN_CLI_WARNING_FLAGS -Werror)
endif()

# sendspin-cli

# A static core library so tests link without main(); definitions and backend libraries are PUBLIC.
add_library(sendspin-cli-core STATIC
    src/cli.cpp
    src/audio_sink.cpp
    src/control_client.cpp
    src/control_common.cpp
    src/control_socket.cpp
    src/config_file.cpp
    src/daemon.cpp
    src/hooks.cpp
    src/key_value_file.cpp
    src/log.cpp
    src/mdns_common.cpp
    src/null_sink.cpp
    src/outbound.cpp
    src/pcm_ring.cpp
    src/pcm_volume.cpp
    src/player_listener.cpp
    src/sink_recovery.cpp
    src/state_store.cpp
    src/supported_formats.cpp
)

# One MdnsService implementation per build.
if(SENDSPIN_CLI_MDNS_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/mdns_dnssd.cpp)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_MDNS)
    # PRIVATE: only mdns_dnssd.cpp reads these.
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

if(SENDSPIN_CLI_PULSE_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/pulse_sink.cpp)
    target_link_libraries(sendspin-cli-core PUBLIC PkgConfig::PULSE)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_PULSE)
endif()

if(SENDSPIN_CLI_PIPEWIRE_ENABLED)
    target_sources(sendspin-cli-core PRIVATE src/pipewire_sink.cpp)
    target_link_libraries(sendspin-cli-core PUBLIC PkgConfig::PIPEWIRE)
    target_compile_definitions(sendspin-cli-core PUBLIC SENDSPIN_CLI_HAVE_PIPEWIRE)
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

# Install

if(SENDSPIN_CLI_IS_TOP_LEVEL)
    include(GNUInstallDirs)

    # Stage with --component sendspin-cli: ArduinoJson installs its own files unconditionally.
    set(SENDSPIN_CLI_COMPONENT sendspin-cli)

    # Named for the binary, not the project.
    set(SENDSPIN_CLI_DOC_DIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/sendspin-cli")

    install(TARGETS sendspin-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${SENDSPIN_CLI_COMPONENT}
    )

    # sendspin-cli-core is not installed.
    install(FILES
        README.md
        contributors.md
        LICENSE
        packaging/sendspin-cli.conf.example
        DESTINATION ${SENDSPIN_CLI_DOC_DIR}
        COMPONENT ${SENDSPIN_CLI_COMPONENT}
    )

    # Generated with the configure-time prefix, so ExecStart matches where the binary installs.
    set(SENDSPIN_CLI_INSTALLED_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/sendspin-cli")
    configure_file(packaging/sendspin-cli.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/sendspin-cli.service @ONLY)

    # lib/systemd/system, not the multiarch libdir, where systemd would not look.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sendspin-cli.service
            DESTINATION lib/systemd/system
            COMPONENT ${SENDSPIN_CLI_COMPONENT}
        )

        # lib/sysusers.d, renamed to the one-.conf-per-package name.
        install(FILES packaging/sendspin-cli.sysusers.conf
            DESTINATION lib/sysusers.d
            RENAME sendspin-cli.conf
            COMPONENT ${SENDSPIN_CLI_COMPONENT}
        )
    endif()
endif()

# Tests

# Defaults ON only at top level.
option(SENDSPIN_CLI_BUILD_TESTS "Build the unit tests" ${SENDSPIN_CLI_IS_TOP_LEVEL})

# Pinned, like SENDSPIN_GIT_TAG.
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
        tests/hooks_test.cpp
        tests/pcm_ring_test.cpp
        tests/pcm_volume_test.cpp
        tests/pipewire_sink_test.cpp
        tests/sink_recovery_test.cpp
        tests/state_store_test.cpp
        tests/supported_formats_test.cpp
    )
    target_link_libraries(sendspin-cli-tests PRIVATE sendspin-cli-core GTest::gtest_main)
    target_compile_options(sendspin-cli-tests PRIVATE ${SENDSPIN_CLI_WARNING_FLAGS})
    gtest_discover_tests(sendspin-cli-tests)
endif()
