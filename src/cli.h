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

/// @brief The WebSocket endpoint this player serves, and the spec's recommended value.
///
/// One constant because it is now read three ways: `parse_server_url()` fills it into a
/// bare `-s <host>`, the mDNS advertisement carries it as the required TXT `path`, and a
/// discovered server's own TXT `path` is compared against nothing else -- the spec makes it
/// per-instance, so a server is free to serve elsewhere.
inline constexpr const char* SENDSPIN_PATH = "/sendspin";

/// @brief The reserved `-s` prefix that means "discover a server" rather than "dial this one".
///
/// Split on the **first** colon, exactly as `-o` splits `<backend>:<device>`, so every
/// existing `-s` form is untouched and `hifi:8927` is still a host and a port. A host
/// genuinely named `mdns` is still reachable as a bare `-s mdns`; only `mdns:` is reserved.
inline constexpr const char* DISCOVERY_PREFIX = "mdns:";

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

/// @brief How much audio each backend keeps buffered by default, in milliseconds.
///
/// Small enough that a device's own playout timing stays a useful sync signal, large enough
/// to ride out scheduling jitter. One figure for every backend: ALSA divides it into periods,
/// PortAudio makes it the ring, and a sink with no device ignores it.
inline constexpr uint32_t DEFAULT_BUFFER_MS = 100;

/// @brief What --buffer-ms will accept, either side inclusive.
///
/// Below the floor there is not enough audio queued for any device to ride out a scheduling
/// hiccup; above the ceiling the buffer dominates the sync loop and every seek or track
/// change waits the whole ring out.
inline constexpr uint32_t MIN_BUFFER_MS = 10;
inline constexpr uint32_t MAX_BUFFER_MS = 2000;

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
    BufferMs,     ///< --buffer-ms
    NoMdns,       ///< --no-mdns
    MdnsName,     ///< --mdns-name
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

    /// --buffer-ms <ms>: how much audio the output backend keeps buffered, MIN_BUFFER_MS to
    /// MAX_BUFFER_MS. One figure for every backend, which is why it is not squeezelite's
    /// `-a`: that flag's `<b>:<p>:<f>:<m>` grammar is ALSA-only, and two of its four
    /// subfields are already fixed here -- the format is negotiated from the stream and the
    /// access mode is pinned to interleaved. Long-only for the same reason as --port, so no
    /// squeezelite letter is squatted.
    ///
    /// A request rather than a promise: a device-less sink (`null`, `stdout`) has nothing to
    /// size and ignores it, and PortAudio's device-latency floor overrides a figure too
    /// small for one callback's worth of audio.
    uint32_t buffer_ms{DEFAULT_BUFFER_MS};

    /// --no-mdns: do not advertise `_sendspin._tcp`. Only meaningful without -s, which
    /// already suppresses the advertisement on its own.
    bool no_mdns{false};

    /// --mdns-name <name>: the instance label to advertise, when it should differ from -n.
    /// Empty means "use -n", which itself defaults to the hostname.
    std::string mdns_name;

    bool show_help{false};     ///< -h, --help
    bool show_version{false};  ///< --version

    /// The WebSocket URL `server` resolved to, validated during parsing. Empty when -s
    /// was not given, and when -s asked for discovery -- there is no URL until a server has
    /// been found. Resolved once here so nothing downstream re-parses a value that has
    /// already been accepted -- and so a bad -s fails before the daemon starts, rather
    /// than dialling something plausible-looking.
    std::string server_url;

    /// True when -s asked for discovery rather than naming an address.
    bool discover{false};

    /// The TXT `name` a discovered server must carry, from `-s mdns:<name>`. Empty for
    /// `-s mdns:`, which takes any server.
    std::string discover_name;

    /// @brief True when this run should advertise `_sendspin._tcp`.
    ///
    /// The spec's rule, not a preference: "Do not advertise `_sendspin._tcp` if the client
    /// plans to initiate the connection", which is what prevents both ends dialling each
    /// other. So *any* -s suppresses it, and there is deliberately no flag that forces the
    /// two modes on together.
    bool advertises() const {
        return !this->no_mdns && !this->was_given(Opt::Server);
    }

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
/// Two values are also normalized rather than merely checked: with -z, a relative -P or -f
/// path is made absolute against the current directory, because the daemon chdir()s to / and
/// a relative path would otherwise name a different file before and after the fork.
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

/// @brief Reads a -s value as the reserved discovery form, if that is what it is.
///
/// `mdns:<name>` asks for a server whose TXT `name` is `<name>`; a bare `mdns:` asks for
/// any server. Split on the first colon, so only the exact prefix is reserved -- `hifi:8927`
/// and a bare `mdns` are still a host, and go to parse_server_url() as they always did.
/// @param name Set to the TXT `name` filter, empty when none was given.
/// @return true if `server` is the discovery form.
bool parse_discovery_spec(const std::string& server, std::string& name);

/// @brief The default friendly name: this host's name, or "sendspin-cli" if unavailable.
std::string default_client_name();

}  // namespace sendspin_cli
