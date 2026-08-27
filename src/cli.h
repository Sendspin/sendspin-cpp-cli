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
#include <optional>
#include <string>
#include <vector>

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
///
/// The two sound-server backends sit *below* both, so adding them changes what no existing host
/// resolves to: anywhere ALSA is built, `default` already reaches the same server through its
/// plugin PCM, and anywhere PortAudio is, it already follows the host's output. They are the
/// default only on a build that has neither -- where the alternative is `null`.
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

/// @brief How much audio each backend keeps buffered by default, in milliseconds.
///
/// Small enough that a device's own playout timing stays a useful sync signal, large enough
/// to ride out scheduling jitter. One figure for every backend, though each spends it its own
/// way: ALSA divides it into periods, PortAudio and PipeWire make it the ring (floored at the
/// device buffer and the graph quantum respectively), PulseAudio hands it to the server as
/// `pa_buffer_attr::tlength` -- a request the server may come back under -- and a sink with no
/// device ignores it.
inline constexpr uint32_t DEFAULT_BUFFER_MS = 100;

/// @brief What --buffer-ms will accept, either side inclusive.
///
/// Below the floor there is not enough audio queued for any device to ride out a scheduling
/// hiccup; above the ceiling the buffer dominates the sync loop and every seek or track
/// change waits the whole ring out.
inline constexpr uint32_t MIN_BUFFER_MS = 10;
inline constexpr uint32_t MAX_BUFFER_MS = 2000;

/// @brief Every option whose provenance the parser tracks, for precedence.
///
/// A config file layers *under* the command line, which means telling "this was supplied" apart
/// from "this is the parser's default". Which of the two supplied it is deliberately **not**
/// recorded here: no behaviour depends on it, and a second bitmask would be a second thing to keep
/// in step. (`src/cli.cpp` does keep the config file and line of each merged value, but only so a
/// refusal can name them -- nothing reads it to decide anything.)
///
/// Not every entry here can come from a config file. The settable set is this enum minus the run
/// shape -- `-l`, `-z`, `--config` and `--help`/`--version`, which have no place in a file -- and
/// `src/cli.cpp` holds that list beside the keys it maps.
enum class Opt : unsigned {
    Device,       ///< -o, --output
    ListDevices,  ///< -l
    Name,         ///< -n, --name
    Server,       ///< -s, --server
    Daemonize,    ///< -z
    Pidfile,      ///< -P, --pidfile
    Logfile,      ///< -f, --logfile
    LogLevel,     ///< -d, --log-level
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

/// @brief Everything the flag surface configures.
///
/// The short flags deliberately mirror squeezelite's, so anyone who runs a Lyrion
/// endpoint can drive this one from muscle memory.
struct Options {
    std::string device{DEFAULT_OUTPUT_DEVICE};  ///< -o <device>: audio output backend
    bool list_devices{false};    ///< -l: list output devices and exit
    std::string name;            ///< -n <name>: friendly name; defaults to the hostname

    /// --id <id>: this player's client_id, the *stable* identity a server files volume,
    /// group membership and pairing under -- where -n is only what it displays. Empty means
    /// the library derives one from the network interface MAC, which is the right identity
    /// for one fixed endpoint per host and the wrong one for two: a dual-mono pair of
    /// daemons on one machine derive the same id, and each server-side setting lands on
    /// whichever connected last. This flag is what gives each its own.
    std::string client_id;

    /// --manufacturer / --product-name <text>: the device info `client/hello` carries,
    /// shown by servers in their device lists. Defaults declare what this really is; the
    /// overrides exist for the integrator whose product embeds this player and should be
    /// listed as itself -- same as the Python CLI's flags of the same names.
    std::string manufacturer{"sendspin-cpp-cli"};
    std::string product_name{"sendspin-cli"};
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

    /// --static-delay <ms>: `PlayerRoleConfig::initial_static_delay_ms`, 0 to
    /// MAX_STATIC_DELAY_MS.
    ///
    /// **How much latency this endpoint's hardware adds *after* the audio port** -- an amplifier,
    /// an external speaker, a DSP. Not a figure to shift playback by: the sync task *subtracts* it
    /// from every chunk's timestamp, so a positive value hands audio to the device that much
    /// *earlier*, and the sound then lands on the timestamp the server meant rather than late. The
    /// spec's own framing (`roles/player/v1.md`): 0 "means audio exits the device's audio port at
    /// the timestamp", and the value "compensates for additional delay beyond the port".
    ///
    /// **A first-run default, not an override.** The library prefers a persisted delay and reads
    /// this only when the state store has none, exactly as a restored volume beats
    /// DEFAULT_SINK_VOLUME -- so once a server or `sendspin-cli delay` has set one, this flag is
    /// inert until the state file is cleared. Long-only, for --port's reason.
    uint16_t static_delay_ms{0};

    /// --no-mdns: do not advertise `_sendspin._tcp`. Only meaningful without -s, which
    /// already suppresses the advertisement on its own.
    bool no_mdns{false};

    /// --mdns-name <name>: the instance label to advertise, when it should differ from -n.
    /// Empty means "use -n", which itself defaults to the hostname.
    std::string mdns_name;

    /// --no-control: do not bind a control socket at all.
    ///
    /// Not the mirror of --no-mdns, which the spec *requires* a dialling client to honour:
    /// this one exists because a systemd system unit has no `$XDG_RUNTIME_DIR`, so an operator
    /// who has decided this player is driven only by its server can say so and silence the
    /// warning rather than reading it on every start.
    bool no_control{false};

    /// --hook-start / --hook-stop <command>: a shell command run when a stream starts or
    /// stops -- an amplifier relay, a light, a notification. `/bin/sh -c` with the event's
    /// facts in `SENDSPIN_*` environment variables; see src/hooks.h for the contract, which
    /// is the Python CLI's. Fired on the stream lifecycle itself, so a stream whose format
    /// the device refused still switches the amplifier -- audio is arriving either way.
    std::string hook_start;
    std::string hook_stop;

    /// --audio-format <codec:rate:depth:channels>: pin a preferred format, e.g.
    /// `flac:48000:24:2` -- the way to hold a fussy DAC at the one shape it is happy in.
    ///
    /// A *reorder*, not a narrowing: the pinned entry moves to the front of the advertised
    /// list, which is what "preferred" means on the wire, and everything the device takes is
    /// still offered behind it. Parsing settles the shape here; whether the device really
    /// takes it is answered at startup, where a pin the derived advertisement does not
    /// contain is a hard refusal to start -- playing something else instead is the failure
    /// this flag exists to prevent. Grammar and behaviour match the Python CLI's flag of the
    /// same name, extended with `opus`.
    std::optional<sendspin::AudioSupportedFormatObject> audio_format;

    /// --state-dir <dir>: where the daemon keeps what it remembers across restarts.
    ///
    /// Empty means the XDG search in state_store_path() decides. It earns its place the way
    /// --no-control does: a systemd *system* unit has neither `$XDG_STATE_HOME` nor `$HOME`, and
    /// `StateDirectory=` hands it `/var/lib/sendspin-cli` to be pointed at.
    std::string state_dir;

    /// The config file this run is using, empty when it found none.
    ///
    /// Both an input and the answer: `--config` puts its value here for the search to skip, and
    /// whatever was really read is left here for the startup log to name. When `--config` was given
    /// the two are the same file, because a named file that cannot be read is fatal rather than
    /// falling back.
    std::string config_path;

    bool show_help{false};     ///< -h, --help
    bool show_version{false};  ///< --version

    /// The control socket path this run resolved to, empty when there is none.
    ///
    /// Resolved once during parsing -- from --control-socket, or from
    /// `$XDG_RUNTIME_DIR/sendspin-cli-<port>.sock` -- so the daemon and a subcommand derive
    /// the same path from the same flags, and so an over-long one is refused before anything
    /// is bound. Made absolute under -z, exactly as -P and -f are.
    std::string control_socket;

    /// Why `control_socket` is empty, for the diagnostic that has to say so. Empty when
    /// --no-control was the reason, which needs no explaining, or when there is a path.
    std::string control_absent_reason;

    /// The subcommand argv[1] named, empty for a daemon run. See split_subcommand().
    std::string subcommand;

    /// The words after the subcommand: exactly its arity, already checked to be present.
    std::vector<std::string> subcommand_args;

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
    /// two modes on together. A `server` out of a config file counts as one, which is why the
    /// config merge marks options as given rather than only setting them.
    bool advertises() const {
        return !this->no_mdns && !this->was_given(Opt::Server);
    }

    /// @brief True if `opt` was supplied rather than left at its default.
    ///
    /// "Supplied" means the command line **or** the config file. That is the whole of what the bit
    /// promises, and every reader wants it that way: the advertise rule above, the -s resolution
    /// and the control socket's length check all have to fire on a configured value exactly as they
    /// do on a typed one.
    bool was_given(Opt opt) const {
        return (this->given_ & Options::bit(opt)) != 0;
    }

    /// @brief Records that `opt` was supplied.
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
/// through `out` for the caller to act on rather than being handled here, and so is a
/// subcommand: `out.subcommand` names it, and the caller runs it instead of a daemon.
///
/// A subcommand and its arguments are taken off the front of argv *before* getopt sees them
/// (see split_subcommand()), so the flags after them are still parsed on every platform and
/// `seek-rel -5000` is an offset rather than a flag cluster.
///
/// Three values are normalized rather than merely checked. With -z, a relative -P, -f or
/// --control-socket path is made absolute against the current directory, because the daemon
/// chdir()s to / and a relative path would otherwise name a different file before and after
/// the fork.
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

/// @brief The spelling of a server URL that is safe to log: its userinfo masked.
///
/// A `-s` value may carry credentials -- `ws://user:token@host:8927/sendspin` -- and every
/// line that names a server is a line an operator pastes into an issue, so every message that
/// quotes one comes through here. The URL handed to the dial is untouched, which is also the
/// limit of what this buys: the library logs the URL it dials itself, and those lines are not
/// this layer's to redact.
///
/// Masks only the secret half. A `user:password` pair keeps its username, which is what makes
/// the line still worth reading -- `ws://user:***@host:8927/sendspin` names the endpoint that
/// was dialled. A userinfo field with no colon is indistinguishable from a bearer token, so
/// the whole of it goes: `ws://***@host`. The mask is a fixed `***` either way, since the
/// length of a secret is itself worth nothing to a reader and something to an attacker.
///
/// Reads the authority the way a URL parser would -- it ends at the first `/`, `?` or `#`, and
/// the userinfo at the last `@` inside it -- which is what makes this safe on strings that are
/// not URLs at all: an `@` in a path or a query, or a whole second URL smuggled into one, is
/// left alone. A value with no scheme is read as a bare authority, which is what a rejected
/// `-s` value is. Anything with no userinfo, an empty one (`ws://@host`) or an empty password
/// (`ws://user:@host`) comes back unchanged: there is nothing there to hide.
///
/// Reading it that way is also the one thing a caller has to know the limit of: a userinfo
/// field holding an *unencoded* `/`, `?` or `#` ends the authority early, so there is no `@`
/// left inside it to split on and the value comes back whole. RFC 3986 requires those
/// percent-encoded there, and such a URL does not name the host it appears to, so it is a
/// malformed value rather than a parse to repair by guesswork -- but it is not masked.
/// @return `url` with its userinfo masked, or `url` itself when it carries none.
std::string redact_url_userinfo(const std::string& url);

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
