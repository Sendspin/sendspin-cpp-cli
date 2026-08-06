// Copyright 2026 sendspin-cpp-cli Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file cli.h
/// @brief squeezelite-style command line surface for sendspin-cli

#pragma once

#include <sendspin/client.h>
#include <sendspin/config.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace sendspin_cli {

/// @brief The -o default: a real sound card where this build has one, silence otherwise.
///
/// ALSA's own `default` PCM follows the host's configuration (PipeWire, PulseAudio or bare
/// hardware), so it is the name most likely to just make noise. A build without the ALSA
/// backend has no device to fall back to, so it defaults to discarding.
#ifdef SENDSPIN_CLI_HAVE_ALSA
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "default";
#else
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "null";
#endif

/// @brief Everything the flag surface configures.
///
/// The short flags deliberately mirror squeezelite's, so anyone who runs a Lyrion
/// endpoint can drive this one from muscle memory.
struct Options {
    std::string device{DEFAULT_OUTPUT_DEVICE};  ///< -o <device>: audio output backend
    bool list_devices{false};    ///< -l: list output devices and exit
    std::string name;            ///< -n <name>: friendly name; defaults to the hostname
    std::string server;          ///< -s <server>: dial this server instead of only listening
    bool daemonize{false};       ///< -z: detach and run in the background
    std::string pidfile;         ///< -P <path>: write our pid here
    std::string logfile;         ///< -f <path>: send log output to this file
    sendspin::LogLevel log_level{sendspin::LogLevel::INFO};  ///< -d [<category>=]<level>

    /// --port <port>: the port our own WebSocket server listens on. Not a squeezelite
    /// flag -- a sendspin player is dialled *by* the server, so the listen port is part
    /// of its identity. Long-only, to leave -p free for squeezelite's priority flag.
    uint16_t port{sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT};

    bool show_help{false};     ///< -h, --help
    bool show_version{false};  ///< --version
};

/// @brief Parses argv into `out`.
///
/// Writes only diagnostics for bad input; --help, --version and -l are reported back
/// through `out` for the caller to act on rather than being handled here.
/// @note Uses getopt_long, so it relies on getopt's process-global state: a second call
/// in the same process must reset `optind` to 1 first.
/// @return true if the arguments were valid.
bool parse_options(int argc, char* argv[], Options& out);

/// @brief Prints the flag reference.
void print_usage(std::FILE* out, const char* prog);

/// @brief Prints our version and the sendspin-cpp tag this binary was built against.
void print_version(std::FILE* out);

/// @brief Turns a -s value into a WebSocket URL.
///
/// Accepts a full ws:// or wss:// URL unchanged, otherwise `<host>[:<port>]`, filling in
/// the /sendspin path and, when no port is given, the port a Sendspin *server* listens on
/// (8927) -- which is not the port this player serves on (8928). IPv6 literals must be
/// bracketed (`[::1]:8927`) for the port to be split off correctly.
std::string server_url(const std::string& server);

/// @brief The default friendly name: this host's name, or "sendspin-cli" if unavailable.
std::string default_client_name();

}  // namespace sendspin_cli
