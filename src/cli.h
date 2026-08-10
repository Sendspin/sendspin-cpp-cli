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
/// hardware), so it is the name most likely to just make noise. It wins wherever both
/// backends are built, because on Linux PortAudio is itself a layer over ALSA and going
/// direct is one layer fewer. `portaudio` on its own follows the host's default output, which
/// is what makes a bare run play on macOS. A build with neither backend has no device to fall
/// back to, so it defaults to discarding.
#ifdef SENDSPIN_CLI_HAVE_ALSA
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "default";
#elif defined(SENDSPIN_CLI_HAVE_PORTAUDIO)
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "portaudio";
#else
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "null";
#endif

/// @brief The options a config file could also supply, for precedence tracking.
///
/// A config file (docs/ROADMAP.md item 8) has to layer *under* the command line, which
/// means telling "the user typed this" apart from "this is the parser's default". Naming
/// each settable option gives that layer something to ask about; this task only provides
/// the question, not the answer.
enum class Opt : unsigned {
    Device,       ///< -o
    ListDevices,  ///< -l
    Name,         ///< -n
    Server,       ///< -s
    Daemonize,    ///< -z
    Pidfile,      ///< -P
    Logfile,      ///< -f
    LogLevel,     ///< -d
    Port,         ///< --port
};

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

    /// The WebSocket URL `server` resolved to, validated during parsing. Empty when -s
    /// was not given. Resolved once here so nothing downstream re-parses a value that has
    /// already been accepted -- and so a bad -s fails before the daemon starts, rather
    /// than dialling something plausible-looking.
    std::string server_url;

    /// @brief True if `opt` was named on the command line rather than left at its default.
    bool was_given(Opt opt) const {
        return (this->given_ & Options::bit(opt)) != 0;
    }

    /// @brief Records that `opt` was named on the command line.
    void mark_given(Opt opt) {
        this->given_ |= Options::bit(opt);
    }

private:
    static constexpr uint32_t bit(Opt opt) {
        return 1U << static_cast<unsigned>(opt);
    }

    uint32_t given_{0};
};

/// @brief Parses argv into `out`, rejecting anything it cannot make sense of.
///
/// Every value is validated here, so a caller that gets `true` back holds a set of
/// options it can act on without re-checking. --help, --version and -l are reported
/// through `out` for the caller to act on rather than being handled here.
///
/// @param err Where diagnostics go. Injected rather than hardcoded to stderr so tests can
/// capture and assert on the wording; the whole parse path writes only here.
/// @return true if the arguments were valid.
/// @note Callable repeatedly in one process: getopt's global scan state is reset on entry.
bool parse_options(int argc, char* argv[], Options& out, std::FILE* err = stderr);

/// @brief Prints the flag reference.
void print_usage(std::FILE* out, const char* prog);

/// @brief Prints our version and the sendspin-cpp tag this binary was built against.
void print_version(std::FILE* out);

/// @brief Turns a -s value into a WebSocket URL, or explains why it cannot.
///
/// Accepts a full ws:// or wss:// URL unchanged, otherwise `<host>[:<port>]`, filling in
/// the /sendspin path and, when no port is given, the port a Sendspin *server* listens on
/// (8927) -- which is not the port this player serves on (8928). IPv6 literals must be
/// bracketed (`[::1]:8927`), since an unbracketed one is indistinguishable from a host
/// with a port.
///
/// Rejects rather than guesses: an address that does not parse means the daemon would
/// dial *something*, and a player quietly talking to the wrong port is harder to diagnose
/// than one that refuses to start.
/// @param error Set to a human-readable reason when the return value is false.
/// @return true if `server` resolved to a URL.
bool parse_server_url(const std::string& server, std::string& url, std::string& error);

/// @brief The default friendly name: this host's name, or "sendspin-cli" if unavailable.
std::string default_client_name();

}  // namespace sendspin_cli
