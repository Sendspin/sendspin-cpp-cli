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

/// Local control channel: a Unix socket in the daemon, subcommands on the same binary.

#pragma once

#include "audio_sink.h"

#include <sendspin/controller_role.h>

#ifndef SENDSPIN_ENABLE_CONTROLLER
#error "sendspin-cli's control channel needs the sendspin controller role: do not configure with -DSENDSPIN_ENABLE_CONTROLLER=OFF"
#endif

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Default socket name is PREFIX<port>SUFFIX.
inline constexpr const char* CONTROL_SOCKET_PREFIX = "sendspin-cli-";

inline constexpr const char* CONTROL_SOCKET_SUFFIX = ".sock";

/// Longest request line the daemon will assemble, in bytes.
inline constexpr size_t MAX_CONTROL_LINE_BYTES = 256;

inline constexpr size_t MAX_CONTROL_CONNECTIONS = 8;

/// Time from accept() to a complete request line; never refreshed by reads.
inline constexpr int64_t CONTROL_IDLE_TIMEOUT_MS = 5000;

/// Spec bound for static delay; sendspin-cpp silently clamps past it, so refuse at parse time.
inline constexpr uint16_t MAX_STATIC_DELAY_MS = 5000;

/// Every request a subcommand can make: controller@v1 commands plus the locally answered two.
enum class ControlCommand : uint8_t {
    Status,  ///< answered locally from the daemon's own state
    Play,
    Pause,
    Stop,
    Next,
    Previous,
    Volume,
    Mute,
    Seek,
    SeekRelative,
    Repeat,
    Shuffle,
    Switch,

    /// Answered locally: this endpoint's own static delay.
    /// Keep last: EveryCommandInTheEnumHasARow walks the enum up to here.
    Delay,
};

/// How a subcommand run ended; the value is its exit status.
/// 1 is deliberately unused: it is the exit status for a command line that does not parse.
enum class ControlStatus : uint8_t {
    Ok = 0,

    /// The daemon refused the request, e.g. `seek` past `seek_max_ms`.
    Usage = 2,

    NoDaemon = 3,      ///< nothing is listening on the control socket
    NotConnected = 4,  ///< the daemon is up, but has no server connection
    Unsupported = 5,   ///< the server did not offer this command in `supported_commands`
    Failed = 6,        ///< the exchange broke down
};

struct ControlSubcommand {
    const char* name;  ///< as typed, e.g. "seek-rel"
    ControlCommand command;
    unsigned arity;           ///< words after the name: 0 or 1
    const char* argument;     ///< argument shape for diagnostics; nullptr when arity is 0
    const char* description;  ///< the --help line
};

/// Every subcommand, in `--help` order.
const std::vector<ControlSubcommand>& control_subcommands();

/// Looks up a subcommand by name; nullptr when unknown.
const ControlSubcommand* find_control_subcommand(const std::string& name);

/// Every subcommand name, comma-separated.
std::string control_subcommand_list();

/// What one subcommand invocation asks for; only the field the command uses is set.
struct ControlRequest {
    ControlCommand command{ControlCommand::Status};
    std::optional<uint8_t> volume{};                       ///< `vol`, 0-100
    std::optional<bool> flag{};                            ///< `mute` and `shuffle`, on|off
    std::optional<uint32_t> position_ms{};                 ///< `seek`
    std::optional<int32_t> offset_ms{};                    ///< `seek-rel`
    std::optional<sendspin::SendspinRepeatMode> repeat{};  ///< `repeat`
    std::optional<uint16_t> delay_ms{};                    ///< `delay`, 0 to MAX_STATIC_DELAY_MS
};

/// A subcommand and its arguments, split off the front of argv.
struct ControlInvocation {
    std::string name;               ///< empty when this is a daemon run
    std::vector<std::string> args;  ///< exactly the subcommand's arity, when it parsed
    int consumed{0};                ///< argv words taken; flags start at argv[consumed]
};

/// Splits argv[1] off as a subcommand, before getopt sees it; a daemon run leaves `out.name` empty.
/// @return false when argv[1] is not a subcommand or its argument is missing.
bool split_subcommand(int argc, char* const argv[], ControlInvocation& out, std::string& error);

/// Parses and range-checks a subcommand's arguments; also the daemon's parser for request lines.
bool parse_control_request(const std::string& name, const std::vector<std::string>& args,
                          ControlRequest& out, std::string& error);

/// The request as one line, as parse_control_request() reads it back.
std::string encode_control_request(const ControlRequest& request);

/// Splits a request line on whitespace. @return false when the line holds no name.
bool split_control_line(const std::string& line, std::string& name, std::vector<std::string>& args);

/// The controller command a request dispatches; nullopt for locally answered requests.
std::optional<sendspin::SendspinControllerCommand> protocol_command(const ControlRequest& request);

/// Only for requests protocol_command() gives a command for.
sendspin::ClientCommandControllerObject to_client_command(const ControlRequest& request);

/// Copy of the server's controller state, safe to keep across main-loop ticks.
struct ControllerSnapshot {
    /// A completed handshake.
    bool connected{false};

    /// Empty until the server sends `server/state`, and again after a disconnect.
    std::vector<sendspin::SendspinControllerCommand> supported_commands{};

    /// Absent for a live or unknown-duration stream.
    std::optional<uint32_t> seek_max_ms{};
};

/// Decides whether the daemon must refuse `request`: not connected, unsupported, or out of range.
/// Locally answered requests are never refused.
/// @return true if the request must not be dispatched.
bool control_refusal(const ControlRequest& request, const ControllerSnapshot& snapshot,
                     ControlStatus& status, std::string& reason);

/// Everything `status` prints.
struct StatusSnapshot {
    std::string name;  ///< -n, or the hostname default

    bool connected{false};
    std::string server_name;  ///< `ServerInformationObject::name`, empty when not known
    std::string server_id;    ///< `ServerInformationObject::server_id`

    /// 0 is paused, 1000 is normal speed; absent when no progress has arrived.
    std::optional<uint32_t> playback_speed{};

    /// Audio is arriving at this endpoint, independent of the group's transport state.
    bool streaming{false};
    std::optional<StreamFormat> format{};  ///< what the sink was configured for

    std::string artist;  ///< empty when unknown
    std::string title;

    /// Absent without metadata progress; a duration of 0 is a live stream.
    std::optional<uint32_t> progress_ms{};
    std::optional<uint32_t> duration_ms{};

    /// False until controller state arrives; the group fields then print `unknown`.
    bool group_state_known{false};
    uint8_t group_volume{0};
    bool group_muted{false};

    /// Last values the server published; a server that omits them reads as OFF/false.
    sendspin::SendspinRepeatMode group_repeat{sendspin::SendspinRepeatMode::OFF};
    bool group_shuffle{false};

    /// What the sink is really applying, from PlayerListener.
    uint8_t player_volume{DEFAULT_SINK_VOLUME};
    bool player_muted{false};

    VolumeSource player_volume_source{VolumeSource::SinkDefault};

    /// The role's effective static delay.
    uint16_t static_delay_ms{0};

    std::string output;  ///< the sink's name()
};

/// Formats a snapshot as `key: value` lines, newline-terminated.
std::string format_status(const StatusSnapshot& snapshot);

/// The reply block: `ok` or `error <kind>: <reason>`, then any payload.
std::string encode_control_reply(ControlStatus status, const std::string& reason,
                                const std::string& payload);

/// Reads a reply's first line back into a status and a reason.
/// @return false when the line is not a reply at all.
bool decode_control_reply(const std::string& line, ControlStatus& status, std::string& reason);

/// State of a line assembler after being fed.
enum class LineState {
    Incomplete,  ///< no '\n' yet, and still inside the length bound
    Ready,       ///< one whole line is available from line()
    TooLong,     ///< MAX_CONTROL_LINE_BYTES passed with no '\n'
    Invalid,     ///< an embedded NUL
};

/// Assembles one '\n'-terminated line across non-blocking reads, bounded in length.
class LineAssembler {
public:
    /// Feeds one read's bytes; bytes after the first '\n' are discarded.
    LineState feed(const char* data, size_t length);

    /// Takes what is buffered as the line, for a peer that closed without '\n'.
    /// @return Ready when anything was buffered, Incomplete otherwise.
    LineState finish();

    /// The assembled line, without its newline. Valid once feed() returned Ready.
    const std::string& line() const {
        return this->line_;
    }

private:
    std::string buffer_;
    std::string line_;
};

/// Why a line assembler gave up, for an `error` reply.
const char* line_state_reason(LineState state);

/// `<runtime_dir>/sendspin-cli-<port>.sock`, or empty when `runtime_dir` is empty.
std::string control_socket_path(const std::string& runtime_dir, uint16_t port);

/// Where the default control socket goes.
struct ControlRuntimeDir {
    /// Empty when no source produced a usable directory.
    std::string path;

    /// Set when `$XDG_RUNTIME_DIR` is used but is group- or world-writable.
    std::string warning;

    /// Why a platform candidate was refused, if one was.
    std::string rejection;
};

/// `$XDG_RUNTIME_DIR` if set and non-empty, else the platform's own; never a shared `/tmp`.
ControlRuntimeDir control_runtime_dir();

/// The platform's verified user-private runtime directory (macOS only), or empty.
/// @param rejection Set when a candidate was found and failed the check.
std::string control_platform_runtime_dir(std::string& rejection);

/// True if `path` is a directory owned by this user that no one else can write to.
bool is_private_runtime_dir(const std::string& path, std::string& reason);

/// Why there is no default control socket path, or empty when there is one.
std::string control_socket_absent_reason(const ControlRuntimeDir& runtime, const std::string& path);

/// True if `path` fits `sockaddr_un::sun_path`.
bool control_socket_path_fits(const std::string& path);

/// The longest control socket path this platform can bind.
size_t control_socket_path_limit();

/// How binding the control socket ended; AlreadyRunning stops the run, Failed does not.
enum class ControlSocketStatus {
    Ok,
    AlreadyRunning,
    Failed,
};

/// Probes the control socket's lock without keeping it, so -z can fail in the parent.
ControlSocketStatus probe_control_socket(const std::string& path, std::string& error);

/// Runs one control request and returns the reply block. Called on the main loop.
class ControlHandler {
public:
    virtual ~ControlHandler() = default;

    /// @param line One request line, without its newline, not yet parsed.
    virtual std::string handle_control_request(const std::string& line) = 0;
};

/// The daemon's control socket, pumped by the main loop; unlinked on destruction.
/// Every method must be called on the main loop thread.
class ControlSocket {
public:
    ControlSocket();
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /// Binds `path` under `<path>.lock`, taking over a stale socket and refusing a live one.
    /// Must be called after daemonize().
    ControlSocketStatus open(const std::string& path, std::string& error);

    /// Accepts and answers what is waiting, without blocking.
    /// @param now_ms Monotonic milliseconds, for the idle deadline.
    void poll(int64_t now_ms, ControlHandler& handler);

    /// Unlinks the socket and drops every connection. Idempotent.
    /// Call before client.disconnect() so no request hits a half-torn-down player.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Runs one subcommand against the daemon's socket and prints the reply.
/// @param absent_reason Why `path` is empty, when it is.
/// @return The status to exit with.
ControlStatus run_control_subcommand(const ControlRequest& request, const std::string& path,
                                     const std::string& absent_reason, std::FILE* out);

}  // namespace sendspin_cli
