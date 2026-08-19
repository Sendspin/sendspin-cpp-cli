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

# Whether this project is being built on its own rather than vendored into another one through
# FetchContent or add_subdirectory. Two things below hang off it, and a parent wants neither:
# the install rules, which would put our binary, our docs and a systemd unit naming *that*
# build's prefix into somebody else's payload, and the test suite, which would fetch and build
# GoogleTest and add our test names to their CTest run. (PROJECT_IS_TOP_LEVEL says this
# directly, but wants CMake 3.21 and this project's floor is 3.16.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SENDSPIN_CLI_IS_TOP_LEVEL ON)
else()
    set(SENDSPIN_CLI_IS_TOP_LEVEL OFF)
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

# IXWebSocket -- which sendspin-cpp fetches in turn -- declares install() rules and defaults
# them ON, so without this every `cmake --install` drops libixwebsocket.a, its headers, a
# pkg-config file and a set of CMake export files into the payload. Nothing links this binary
# at runtime and nothing builds against it, so none of that belongs in a tarball. Turned off
# here rather than left to the install component below, since it has an option and using it
# keeps the export set out of the build entirely; GoogleTest's is turned off the same way where
# it is fetched. ArduinoJson is the dependency that has no such option -- see the component in
# the install section.
#
# No FORCE, so a build tree configured before this line existed keeps the cached ON until it is
# reconfigured from scratch: that tree stages more than the component names, and is worth
# deleting rather than reasoning about.
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "Install IXWebSocket alongside our own targets")

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
    src/sink_recovery.cpp
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
# Install
# ==============================================================================

# Top-level only, for the reason the variable is declared with: a parent that vendors this
# project has no use for our binary, our README or a unit naming its prefix -- which is the
# same complaint the component below exists to answer, and it would be poor manners to make it
# of somebody else while making it of ArduinoJson.
if(SENDSPIN_CLI_IS_TOP_LEVEL)
    include(GNUInstallDirs)

    # Every rule below names this component, and the payload is staged by asking for it:
    #
    #     cmake --install build --prefix <dir> --component sendspin-cli
    #
    # The component is what makes that payload only ours. ArduinoJson -- fetched by sendspin-cpp
    # in turn -- installs its headers and three CMake export files unconditionally, with no
    # option to say otherwise, so a bare `cmake --install` stages 143 files nothing in the
    # payload links or builds against. Asking for one component is the stock way to say which
    # install rules are ours, the alternative being to patch a fetched subproject; it also holds
    # if a later SENDSPIN_GIT_TAG brings in a dependency that installs, where a list of upstream
    # options would have to be extended to match.
    set(SENDSPIN_CLI_COMPONENT sendspin-cli)

    # share/doc/sendspin-cli rather than GNUInstallDirs' own CMAKE_INSTALL_DOCDIR, which is built
    # from the *project* name: everything anyone types is `sendspin-cli`, and a doc directory
    # named after the repository would be the one place the other spelling surfaced.
    set(SENDSPIN_CLI_DOC_DIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/sendspin-cli")

    install(TARGETS sendspin-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${SENDSPIN_CLI_COMPONENT}
    )

    # sendspin-cli-core is deliberately absent: it exists so the tests can link the parser
    # without a process entry point, and nothing outside this build has any use for an archive
    # of it.
    install(FILES
        README.md
        LICENSE
        packaging/sendspin-cli.conf.example
        DESTINATION ${SENDSPIN_CLI_DOC_DIR}
        COMPONENT ${SENDSPIN_CLI_COMPONENT}
    )

    # The unit names an absolute ExecStart, so it is generated from the prefix this build was
    # configured for rather than copied -- a unit installed from one prefix cannot then start a
    # binary from another. Note that this is the *configure-time* prefix: `cmake --install
    # --prefix` relocates where the files land without changing what the unit says, which is what
    # makes a staged payload a payload for /usr/local (the default) rather than for the staging
    # directory.
    #
    # Generated on every platform though only Linux installs it, so the one file whose content
    # depends on the configure line can be read back wherever the build happened.
    set(SENDSPIN_CLI_INSTALLED_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/sendspin-cli")
    configure_file(packaging/sendspin-cli.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/sendspin-cli.service @ONLY)

    # lib/systemd/system, not ${CMAKE_INSTALL_LIBDIR}/systemd/system: a unit file is
    # architecture-independent and systemd reads <prefix>/lib/systemd/system, so Debian's
    # multiarch libdir would put it in lib/aarch64-linux-gnu/systemd/system where nothing looks
    # for it. /usr/local/lib/systemd/system is on that search path as well as
    # /usr/lib/systemd/system, which is what lets the default prefix work with no drop-in and no
    # copying by hand.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sendspin-cli.service
            DESTINATION lib/systemd/system
            COMPONENT ${SENDSPIN_CLI_COMPONENT}
        )

        # The account the unit's User= names. lib/sysusers.d for the same reason as
        # lib/systemd/system above -- the fragment is architecture-independent and
        # systemd-sysusers reads <prefix>/lib/sysusers.d, /usr/local included -- and copied
        # rather than generated, since nothing in it depends on the configure line.
        #
        # RENAME because the source name says which of this project's files it is and the
        # installed name is what sysusers.d wants: one .conf per package, named after it.
        install(FILES packaging/sendspin-cli.sysusers.conf
            DESTINATION lib/sysusers.d
            RENAME sendspin-cli.conf
            COMPONENT ${SENDSPIN_CLI_COMPONENT}
        )
    endif()
endif()

# ==============================================================================
# Tests
# ==============================================================================

# Defaults ON only when this is the top-level project: a parent that vendors sendspin-cli
# via FetchContent should not pay to fetch and build GoogleTest, nor find our test names
# in its own CTest run. An option rather than a plain `if`, unlike the install rules, because
# a contributor turning the suite off for a quick build is a reasonable thing to want.
option(SENDSPIN_CLI_BUILD_TESTS "Build the unit tests" ${SENDSPIN_CLI_IS_TOP_LEVEL})

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
        tests/sink_recovery_test.cpp
        tests/state_store_test.cpp
        tests/supported_formats_test.cpp
    )
    target_link_libraries(sendspin-cli-tests PRIVATE sendspin-cli-core GTest::gtest_main)
    target_compile_options(sendspin-cli-tests PRIVATE ${SENDSPIN_CLI_WARNING_FLAGS})
    gtest_discover_tests(sendspin-cli-tests)
endif()
