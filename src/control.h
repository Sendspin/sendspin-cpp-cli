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

/// @file control.h
/// @brief The local control channel: a Unix socket in the daemon, subcommands on the same binary

#pragma once

#include <sendspin/controller_role.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief The name of the control socket the daemon binds under `$XDG_RUNTIME_DIR`.
///
/// The port is in the leaf rather than in a subdirectory, so two players on one host get
/// their own socket without either having to create a directory the other might own.
inline constexpr const char* CONTROL_SOCKET_PREFIX = "sendspin-cli-";
inline constexpr const char* CONTROL_SOCKET_SUFFIX = ".sock";

/// @brief The longest request line the daemon will assemble, in bytes.
///
/// A bound rather than a protocol limit, for the reason `src/last_server.cpp` bounds its own
/// read: the socket is the daemon's, but anything that can reach it can also send bytes
/// forever. The longest legal request is `seek-rel -2147483648`, at 20 bytes.
inline constexpr size_t MAX_CONTROL_LINE_BYTES = 256;

/// @brief How many control connections the daemon will hold at once.
///
/// One command per connection and a reply measured in microseconds, so the only way to reach
/// this is a peer that connects and does not talk -- which is what the idle deadline below is
/// for. Bounded anyway, so a local user cannot spend the daemon's descriptors.
inline constexpr size_t MAX_CONTROL_CONNECTIONS = 8;

/// @brief How long a control connection may stay silent before the daemon drops it.
///
/// A connection is opened, one line is written and the reply is read: a peer that has sent
/// nothing for this long is not going to. Generous enough that a hand-driven `socat` session
/// is not cut off mid-type.
inline constexpr int64_t CONTROL_IDLE_TIMEOUT_MS = 5000;

/// @brief What `sendspin-cli <subcommand>` can ask for.
///
/// One entry per command in the spec's `controller@v1` role, which is the whole transport
/// surface: the `player` role carries only this endpoint's own volume, mute and static delay,
/// so anything that moves the *group* has to go out as a controller command.
enum class ControlCommand : uint8_t {
    Status,  ///< not a protocol command: the daemon's own view, formatted locally
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
};

/// @brief How a subcommand run ended, and the exit status it leaves.
///
/// The three failures the brief keeps distinct are distinct values here, because they call
/// for three different actions: start the daemon, connect it to a server, or stop asking for
/// a command this server does not offer. Collapsing them into one non-zero status would make
/// a script unable to tell "nothing is running" from "the server said no".
///
/// 1 is deliberately absent: that is what this binary already exits with when its command line
/// does not parse, which includes a subcommand's own arguments -- `vol 500` is refused by
/// parse_options() before a socket is opened, exactly as a bad `--buffer-ms` is. So `Usage`
/// here is specifically the *daemon's* refusal of an argument that parsed locally, which today
/// means a `seek` past the `seek_max_ms` only the daemon knows.
enum class ControlStatus : uint8_t {
    Ok = 0,            ///< the command was sent, or `status` printed
    Usage = 2,         ///< the daemon refused the argument, or the request line was malformed
    NoDaemon = 3,      ///< nothing is listening on the control socket
    NotConnected = 4,  ///< the daemon is up, but has no server connection
    Unsupported = 5,   ///< the server did not offer this command in `supported_commands`
    Failed = 6,        ///< the exchange broke down
};

/// @brief One row of the subcommand table.
struct ControlSubcommand {
    const char* name;         ///< as typed, e.g. "seek-rel"
    ControlCommand command;   ///< what it asks for
    unsigned arity;           ///< how many words follow the name: 0 or 1
    const char* argument;     ///< what that word must be, for diagnostics; nullptr when arity is 0
    const char* description;  ///< the --help line
};

/// @brief Every subcommand, in the order `--help` lists them.
///
/// The single source of truth for the name, the argument count and the help text, so a
/// subcommand cannot exist in the parser and be missing from `--help`.
const std::vector<ControlSubcommand>& control_subcommands();

/// @brief Looks up a subcommand by the name typed, or nullptr when there is no such thing.
const ControlSubcommand* find_control_subcommand(const std::string& name);

/// @brief Every subcommand name, comma-separated, for a diagnostic that has to list them.
std::string control_subcommand_list();

/// @brief What one subcommand invocation asks the daemon to do.
///
/// Only the field the command uses is set, mirroring `ClientCommandControllerObject`. Kept as
/// our own type rather than that one because it also has to carry `status`, which is not a
/// protocol command, and `repeat`, which is three protocol commands rather than a parameter.
struct ControlRequest {
    ControlCommand command{ControlCommand::Status};
    std::optional<uint8_t> volume{};                  ///< `vol`, 0-100
    std::optional<bool> flag{};                       ///< `mute` and `shuffle`, on|off
    std::optional<uint32_t> position_ms{};            ///< `seek`
    std::optional<int32_t> offset_ms{};               ///< `seek-rel`
    std::optional<sendspin::SendspinRepeatMode> repeat{};  ///< `repeat`
};

/// @brief The subcommand and its arguments, split off the front of argv.
struct ControlInvocation {
    std::string name;               ///< empty when this is a daemon run
    std::vector<std::string> args;  ///< exactly the subcommand's arity, when it parsed
    int consumed{0};                ///< argv words the split took, so flags start at argv[consumed]
};

/// @brief Reads argv[1] as a subcommand, when that is what it is.
///
/// Done before `getopt_long()` rather than by reading its leftovers, because getopt's
/// treatment of a positional argument is not portable: glibc permutes argv so the flags after
/// it are still seen, and the BSDs stop at the first non-option word. `seek-rel -5000` settles
/// it on its own -- a negative offset is indistinguishable from a flag cluster to getopt, so
/// the argument has to be taken out of argv before getopt ever looks at it.
///
/// Only argv[1] is considered, so `sendspin-cli --port 9000 status` is *not* a subcommand run;
/// parse_options() reports that as a subcommand in the wrong place rather than as junk.
///
/// @param error Set to a human-readable reason when the return value is false.
/// @return false only when argv[1] names something that is not a subcommand, or the
/// subcommand's argument is missing. A daemon run yields an empty `out.name` and true.
bool split_subcommand(int argc, char* const argv[], ControlInvocation& out, std::string& error);

/// @brief Turns a subcommand and its arguments into a request, or says why it cannot.
///
/// Every range check the client can make without asking the daemon happens here: `vol` 0-100,
/// `seek` a non-negative value that fits `uint32_t`, `seek-rel` anything that fits `int32_t`,
/// `mute`/`shuffle` on|off, `repeat` off|one|all. `seek` against the server's published
/// `seek_max_ms` cannot be checked here, since only the daemon holds that -- see
/// control_refusal().
///
/// Also the daemon's own parser: it reads the request line back through this, so the two ends
/// cannot drift into disagreeing about what `vol 50` means.
/// @param error Set to a human-readable reason, naming the value and the accepted range.
bool parse_control_request(const std::string& name, const std::vector<std::string>& args,
                          ControlRequest& out, std::string& error);

/// @brief The request as one line, exactly as parse_control_request() reads it back.
std::string encode_control_request(const ControlRequest& request);

/// @brief Splits a request line into a subcommand name and its arguments.
///
/// Whitespace-separated, because every argument this protocol carries is a number or a
/// keyword. @return false when the line holds no name at all.
bool split_control_line(const std::string& line, std::string& name, std::vector<std::string>& args);

/// @brief The protocol command a request dispatches, or nothing when it dispatches none.
///
/// `status` is the only request with no protocol command behind it: it is answered out of the
/// daemon's own shadows and never reaches the server.
std::optional<sendspin::SendspinControllerCommand> protocol_command(const ControlRequest& request);

/// @brief The library command object a request becomes, ready for send_command().
sendspin::ClientCommandControllerObject to_client_command(const ControlRequest& request);

/// @brief What the daemon knows about the server's controller state, as plain data.
///
/// A snapshot rather than a reference into the role: `get_controller_state()` hands back a
/// reference to a vector that `drain_events()` move-assigns from inside `client.loop()`, so
/// anything that outlives one main-loop tick has to be a copy.
struct ControllerSnapshot {
    /// True on a completed handshake -- `SendspinClient::is_connected()`.
    bool connected{false};

    /// `ServerStateControllerObject::supported_commands`, or empty when the server has sent
    /// no `server/state` yet. Emptied again by `on_controller_state_clear()` on a disconnect,
    /// which is why `connected` is carried separately: without it, a dropped connection would
    /// be reported as "pause is not supported".
    std::vector<sendspin::SendspinControllerCommand> supported_commands{};

    /// `ServerStateControllerObject::seek_max_ms`. Absent for a live or unknown-duration
    /// stream, in which case `seek` carries no upper bound at all.
    std::optional<uint32_t> seek_max_ms{};
};

/// @brief Decides whether the daemon may dispatch `request`, and says why not.
///
/// Three refusals, in the order that makes each message the true one:
///
/// 1. **Not connected.** Nothing can be sent, and `SendspinClient::send_text()` no-ops
///    silently with no connection -- so a dispatch here would report a success that never
///    left the process.
/// 2. **Unsupported.** The server ignores a command outside its `supported_commands`, so
///    sending it would be a success that changes nothing.
/// 3. **Out of range for this server.** Only `seek` past a published `seek_max_ms`.
///
/// `status` is refused by none of them: it is answered locally, and a daemon with no server
/// connection is exactly when it is most worth reading.
/// @param status Set to the refusal's kind when the return value is true.
/// @param reason Set to a human-readable reason when the return value is true.
/// @return true if the request must not be dispatched.
bool control_refusal(const ControlRequest& request, const ControllerSnapshot& snapshot,
                     ControlStatus& status, std::string& reason);

/// @brief The stream format the output device was last configured for.
struct StreamFormat {
    uint32_t sample_rate{0};
    uint8_t channels{0};
    uint8_t bit_depth{0};
};

/// @brief Everything `status` prints, gathered from the daemon's main-loop shadows.
///
/// Plain data with no library types in it beyond the two the daemon really holds, so the
/// formatter is a pure function over a struct a test can build by hand.
struct StatusSnapshot {
    std::string name;  ///< -n, or the hostname it defaulted to

    bool connected{false};      ///< a completed handshake
    std::string server_name;    ///< `ServerInformationObject::name`, empty when not known
    std::string server_id;      ///< `ServerInformationObject::server_id`

    /// The metadata progress object's `playback_speed`, absent when the server has sent no
    /// progress at all. 0 is paused, 1000 is normal speed. Absent prints `unknown` rather
    /// than being guessed at as stopped: no state having arrived is not a state.
    std::optional<uint32_t> playback_speed{};

    /// This daemon's own `on_stream_start`/`on_stream_end`. Labelled separately from the
    /// transport state on purpose: "audio is arriving here" and "the group is playing" are
    /// different facts, and a player that has been dropped from the group has the second
    /// without the first.
    bool streaming{false};
    std::optional<StreamFormat> format{};  ///< what the sink was configured for, if anything

    std::string artist;  ///< from the cached metadata; empty when unknown
    std::string title;

    /// `MetadataRole::get_track_progress_ms()` and `get_track_duration_ms()`, absent when
    /// the server has sent no metadata progress. A duration of 0 is a live stream.
    std::optional<uint32_t> progress_ms{};
    std::optional<uint32_t> duration_ms{};

    /// True once the server has sent controller state this connection. False leaves the
    /// group lines reading `unknown`, since a default-constructed state is 0/unmuted and
    /// printing that would be a claim about the group rather than an absence of one.
    bool group_state_known{false};
    uint8_t group_volume{0};
    bool group_muted{false};

    /// `PlayerRole::get_volume()` and `get_muted()` -- this endpoint's own output, which is
    /// known whether or not a server is connected.
    uint8_t player_volume{0};
    bool player_muted{false};

    std::string output;  ///< the sink's `name()`
};

/// @brief Formats a status snapshot as the `key: value` block `status` prints.
///
/// Ends with a newline, and every line is `<key>: <value>` so `grep` and `cut -d:` both
/// reach a field. Group and player volume are separate, named lines: `vol` moves the group,
/// the server clamps it per player, and one ambiguous `volume:` would leave a reader unable
/// to tell which number their `vol 50` had moved.
std::string format_status(const StatusSnapshot& snapshot);

/// @brief The reply block for one request: a status line, then any payload.
///
/// The first line is `ok`, or `error <kind>: <reason>`. The kind is one machine-readable
/// token so the client can map a refusal onto its own exit status without parsing the reason,
/// and the whole line still reads as English to anyone driving the socket by hand.
std::string encode_control_reply(ControlStatus status, const std::string& reason,
                                const std::string& payload);

/// @brief Reads a reply's first line back into a status and a reason.
///
/// @param reason Set to the error's reason, empty on `ok`.
/// @return false when the line is not a reply at all, which is what a peer that is not a
/// sendspin-cli daemon looks like.
bool decode_control_reply(const std::string& line, ControlStatus& status, std::string& reason);

/// @brief What a line assembler holds after being fed some bytes.
enum class LineState {
    Incomplete,  ///< no '\n' yet, and still inside the length bound
    Ready,       ///< one whole line is available from line()
    TooLong,     ///< MAX_CONTROL_LINE_BYTES passed with no '\n'
    Invalid,     ///< an embedded NUL: this is a text protocol
};

/// @brief Assembles one '\n'-terminated line out of however many reads it takes.
///
/// A non-blocking socket hands over whatever has arrived, which for a line as short as a
/// control request will usually be all of it and is not guaranteed to be any of it. Bounded
/// because the peer decides how much to send.
class LineAssembler {
public:
    /// @brief Feeds one read's worth of bytes.
    ///
    /// Bytes after the first '\n' are discarded: this protocol is one command per
    /// connection, so a second line is a peer talking out of turn rather than pipelining.
    LineState feed(const char* data, size_t length);

    /// @brief Treats what is buffered as a whole line, for a peer that closed without a '\n'.
    ///
    /// @return Ready when there was anything to take, Incomplete when the buffer was empty.
    LineState finish();

    /// @brief The assembled line, without its newline. Valid once feed() returned Ready.
    const std::string& line() const {
        return this->line_;
    }

private:
    std::string buffer_;
    std::string line_;
};

/// @brief Why a line assembler gave up, as a reason for an `error` reply.
const char* line_state_reason(LineState state);

/// @brief The default control socket path for a player serving on `port`.
///
/// `<runtime_dir>/sendspin-cli-<port>.sock`. The port is in the name because the socket
/// belongs to one player and a host can run several; it is the serve port rather than an
/// index so a subcommand can derive the same path from the same `--port`.
///
/// @param runtime_dir `$XDG_RUNTIME_DIR`, from control_runtime_dir(). Empty yields an empty
/// path: there is deliberately no fallback to a shared directory -- see
/// control_socket_absent_reason().
std::string control_socket_path(const std::string& runtime_dir, uint16_t port);

/// @brief `$XDG_RUNTIME_DIR`, or empty when it is unset or set to nothing.
///
/// A variable set to the empty string counts as unset, which is what the XDG spec says and
/// what `last_server_path()` already does with `$XDG_STATE_HOME`: the alternative resolves to
/// a path starting at the filesystem root.
///
/// Split from control_socket_path() so that function stays pure and testable -- the
/// environment is read here, once, and handed in.
std::string control_runtime_dir();

/// @brief Why there is no default control socket path, or empty when there is one.
///
/// A user-session variable is the only acceptable home for this socket. `$XDG_RUNTIME_DIR` is
/// per-user and 0700, which is what makes the socket unreachable by another local user; a
/// systemd *system* unit has no such variable, and `/tmp` is world-writable, so falling back
/// there would hand any local account the ability to pause playback and `switch` this
/// endpoint out of its group. So an absent variable means no socket, not a worse one.
///
/// Non-fatal, and the wording says what to do instead: the player is still a player without
/// a control channel, exactly as it is still a player without an mDNS advertisement.
/// @param path The path control_socket_path() produced, so an over-long one is named as such.
std::string control_socket_absent_reason(const std::string& runtime_dir, const std::string& path);

/// @brief True if `path` fits `sockaddr_un::sun_path`, which is 104 bytes on macOS.
///
/// Checked at parse time and hard-failed rather than truncated: a silently shortened path
/// binds a socket nothing will find, and the subcommand would report "no daemon" against a
/// daemon that is running.
bool control_socket_path_fits(const std::string& path);

/// @brief The longest control socket path this platform can bind, for a diagnostic.
size_t control_socket_path_limit();

/// @brief How binding the control socket ended.
///
/// AlreadyRunning is separated from Failed for the reason PidFileStatus separates the same two:
/// a lock held by another instance is the operator having started a duplicate, which must stop
/// this run, while everything else -- an unwritable directory, a descriptor limit -- leaves a
/// player that still works and should carry on serving audio.
enum class ControlSocketStatus {
    Ok,
    AlreadyRunning,
    Failed,
};

/// @brief Runs one control request on the main loop and returns the whole reply block.
///
/// Implemented by the daemon, called by ControlSocket. An interface rather than a
/// std::function so the threading contract has somewhere to be written down: every
/// implementation is called from ControlSocket::poll(), and therefore from the main loop.
class ControlHandler {
public:
    virtual ~ControlHandler() = default;

    /// @param line One request line, without its newline. Not yet parsed or trusted.
    virtual std::string handle_control_request(const std::string& line) = 0;
};

/// @brief The daemon's half of the control channel: a bound Unix socket, pumped by the main loop.
///
/// THREAD SAFETY: every method must be called on the main loop thread, and poll() is what
/// runs the handler. That is not a convenience. A request reaches
/// `ControllerRole::send_command()`, which reaches `SendspinClient::send_text()` and
/// `ConnectionManager::current()` -- documented main-thread-only, with `current_shared()`
/// existing for off-thread callers. Reading is no safer: `get_controller_state()` returns a
/// reference to a vector `drain_events()` move-assigns from inside `client.loop()`. So there
/// is no reader thread and no command queue; the cost is that a round trip is bounded by the
/// main loop's tick rather than by the socket.
///
/// Lifetime mirrors PidFile: the socket is unlinked in the destructor, so the path is cleaned
/// up on the failed-startup paths too.
class ControlSocket {
public:
    ControlSocket();
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /// @brief Binds and listens on `path`, taking a stale socket over and refusing a live one.
    ///
    /// Held under an exclusive `flock()` on a sibling `<path>.lock` for the process's
    /// lifetime, then unlinked and bound under that lock. That ordering is what makes the
    /// two cases different: a crashed daemon's lock is gone with its descriptor, so its
    /// leftover socket file is simply unlinked and rebound, while a *running* daemon still
    /// holds the lock and the second instance is refused before it can unlink a socket that
    /// is being listened on. `unlink()`-then-`bind()` alone races the live socket away, and
    /// connecting to probe is a TOCTOU -- the same argument `src/daemon.cpp` makes for
    /// preferring `flock()` over an `O_EXCL` create and a pid-and-signal probe.
    ///
    /// Its own lock rather than a second use of the `-P` pidfile, so `--control-socket` does
    /// not acquire a dependency on `-P` being given.
    ///
    /// Must be called *after* `daemonize()`: a socket is one of the resources the fork
    /// invariant at `src/daemon.cpp` names, and `bind()` applies the umask that
    /// `daemonize()` sets.
    /// @param error Set to a human-readable reason on anything but Ok.
    ControlSocketStatus open(const std::string& path, std::string& error);

    /// @brief Accepts what is waiting, answers whatever sent a whole line, without blocking.
    ///
    /// @param now_ms A monotonic clock in milliseconds, for the idle deadline.
    /// @param handler Runs each assembled request. Called on this thread.
    void poll(int64_t now_ms, ControlHandler& handler);

    /// @brief Unlinks the socket and drops every connection. Idempotent; the destructor calls it.
    ///
    /// Called explicitly during shutdown, *before* `client.disconnect()`, for the reason
    /// `mdns.stop()` goes first: a request accepted after the client has gone would be
    /// answered out of a half-torn-down player.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Runs one subcommand against a daemon's control socket, and prints what it says.
///
/// The whole of a subcommand run: connect, write one line, read the reply, print it, and
/// return the status to exit with. Opens no audio device, starts no WebSocket server, takes
/// no pidfile and touches no mDNS -- a `status` must not disturb the daemon it is asking
/// about, let alone try to become one.
///
/// @param path The socket to talk to, from `--control-socket` or the default.
/// @param absent_reason Why `path` is empty, when it is, so "no socket" can say what to do.
/// @param out Where the reply's payload goes. Diagnostics go to stderr.
/// @return The status to exit with.
ControlStatus run_control_subcommand(const ControlRequest& request, const std::string& path,
                                     const std::string& absent_reason, std::FILE* out);

}  // namespace sendspin_cli
