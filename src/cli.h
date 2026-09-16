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

/// Command line surface for sendspin-cli.

#pragma once

#include <sendspin/client.h>
#include <sendspin/config.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sendspin_cli {

/// The WebSocket path this player serves and advertises.
inline constexpr const char* SENDSPIN_PATH = "/sendspin";

/// The `-s` prefix that asks for mDNS discovery; the name follows the first colon.
inline constexpr const char* DISCOVERY_PREFIX = "mdns:";

/// The -o default: the most direct real backend this build has, else `null`.
#ifdef SENDSPIN_CLI_HAVE_ALSA
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "default";
#elif defined(SENDSPIN_CLI_HAVE_PORTAUDIO)
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "portaudio";
#elif defined(SENDSPIN_CLI_HAVE_PULSE)
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "pulse";
#elif defined(SENDSPIN_CLI_HAVE_PIPEWIRE)
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "pipewire";
#else
inline constexpr const char* DEFAULT_OUTPUT_DEVICE = "null";
#endif

/// Default output buffer for every backend, in milliseconds.
inline constexpr uint32_t DEFAULT_BUFFER_MS = 100;

/// What --buffer-ms will accept, either side inclusive.
inline constexpr uint32_t MIN_BUFFER_MS = 10;
inline constexpr uint32_t MAX_BUFFER_MS = 2000;

/// Every option whose provenance the parser tracks, so a config file can layer under argv.
enum class Opt : unsigned {
    Device,         ///< -o, --output
    ListDevices,    ///< -l
    Name,           ///< -n, --name
    Server,         ///< -s, --server
    Daemonize,      ///< -z
    Pidfile,        ///< -P, --pidfile
    Logfile,        ///< -f, --logfile
    LogLevel,       ///< -d, --log-level
    Port,           ///< --port
    BufferMs,       ///< --buffer-ms
    StaticDelay,    ///< --static-delay
    NoMdns,         ///< --no-mdns
    MdnsName,       ///< --mdns-name
    ControlSocket,  ///< --control-socket
    NoControl,      ///< --no-control
    StateDir,       ///< --state-dir
    Config,         ///< --config
    HookStart,      ///< --hook-start
    HookStop,       ///< --hook-stop
    ClientId,       ///< --id
    Manufacturer,   ///< --manufacturer
    ProductName,    ///< --product-name
    AudioFormat,    ///< --audio-format
};

/// Everything the flag surface configures.
struct Options {
    std::string device{DEFAULT_OUTPUT_DEVICE};  ///< -o <device>: audio output backend
    bool list_devices{false};                   ///< -l: list output devices and exit
    std::string name;  ///< -n <name>: friendly name; defaults to the hostname

    /// --id <id>: stable client_id; empty derives one from the interface MAC.
    std::string client_id;

    /// --manufacturer / --product-name <text>: device info sent in `client/hello`.
    std::string manufacturer{"sendspin-cpp-cli"};
    std::string product_name{"sendspin-cli"};
    std::string server;     ///< -s mdns:[<name>]: discover a server and dial it
    bool daemonize{false};  ///< -z: detach and run in the background
    std::string pidfile;    ///< -P <path>: write our pid here
    std::string logfile;    ///< -f <path>: send log output to this file
    sendspin::LogLevel log_level{sendspin::LogLevel::INFO};  ///< -d [<category>=]<level>

    /// --port <port>: the port our own WebSocket server listens on.
    uint16_t port{sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT};

    /// --buffer-ms <ms>: buffer request, MIN_BUFFER_MS to MAX_BUFFER_MS; a sink may ignore it.
    uint32_t buffer_ms{DEFAULT_BUFFER_MS};

    /// --static-delay <ms>: hardware latency after the audio port, 0 to MAX_STATIC_DELAY_MS.
    /// A first-run default: a persisted delay wins.
    uint16_t static_delay_ms{0};

    /// --no-mdns: do not advertise `_sendspin._tcp`.
    bool no_mdns{false};

    /// --mdns-name <name>: instance label to advertise; empty uses -n.
    std::string mdns_name;

    /// --no-control: do not bind a control socket.
    bool no_control{false};

    /// --hook-start / --hook-stop <command>: run on stream start/stop; see hooks.h.
    std::string hook_start;
    std::string hook_stop;

    /// --audio-format <codec:rate:depth:channels>[,...]: preferred formats, in priority order.
    std::vector<sendspin::AudioSupportedFormatObject> audio_formats;

    /// --state-dir <dir>: where remembered state lives; empty uses state_store_path()'s search.
    std::string state_dir;

    /// --config's value, then the config file actually read; empty when none.
    std::string config_path;

    bool show_help{false};     ///< -h, --help
    bool show_version{false};  ///< --version

    /// The resolved control socket path, empty when there is none.
    std::string control_socket;

    /// Why `control_socket` is empty; also empty for --no-control.
    std::string control_absent_reason;

    /// The subcommand argv[1] named, empty for a daemon run.
    std::string subcommand;

    /// The words after the subcommand, exactly its arity.
    std::vector<std::string> subcommand_args;

    /// True when -s asked for discovery.
    bool discover{false};

    /// TXT `name` a discovered server must carry; empty takes any server.
    std::string discover_name;

    /// True when this run should advertise `_sendspin._tcp`: never with any -s.
    bool advertises() const {
        return !this->no_mdns && !this->was_given(Opt::Server);
    }

    /// True if `opt` was supplied on the command line or in a config file.
    bool was_given(Opt opt) const {
        return (this->given_ & Options::bit(opt)) != 0;
    }

    void mark_given(Opt opt) {
        this->given_ |= Options::bit(opt);
    }

private:
    static constexpr uint32_t bit(Opt opt) {
        return 1U << static_cast<unsigned>(opt);
    }

    uint32_t given_{0};
};

/// Parses and validates argv into `out`; with -z, relative paths are made absolute.
/// --help, --version, -l and subcommands are reported through `out`, not handled.
/// @param err Where diagnostics go.
/// @return true if the arguments were valid.
bool parse_options(int argc, char* argv[], Options& out, std::FILE* err = stderr);

/// Prints the flag reference.
void print_usage(std::FILE* out, const char* prog);

/// Prints our version and the sendspin-cpp tag this binary was built against.
void print_version(std::FILE* out);

/// Reads `mdns:<name>` or a bare `mdns:` from a -s value.
/// @param name Set to the TXT `name` filter, empty when none was given.
bool parse_discovery_spec(const std::string& server, std::string& name);

/// This host's name, or "sendspin-cli" if unavailable.
std::string default_client_name();

}  // namespace sendspin_cli
