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

/// @file main.cpp
/// @brief sendspin-cli: a headless Sendspin player endpoint
///
/// Boots a SendspinClient in the `player` role, points it at an AudioSink, and pumps
/// client.loop() until a signal arrives. A Sendspin server drives everything else.
///
/// Two connection modes, which the spec makes mutually exclusive: by default the player
/// advertises `_sendspin._tcp` and waits to be dialled, and any -s instead makes it dial
/// out -- to an address, or to a server it discovers over mDNS -- with the advertisement
/// suppressed.
///
/// The same binary is also its own client: `sendspin-cli <subcommand>` talks to a running
/// player over that player's control socket and exits, without opening a device or a port.

#include "audio_sink.h"
#include "cli.h"
#include "control.h"
#include "daemon.h"
#include "log.h"
#include "mdns.h"
#include "outbound.h"
#include "player_listener.h"
#include "state_store.h"
#include "supported_formats.h"

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/types.h>

// For sigaction(), which <csignal> is not required to declare.
#include <signal.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// The entry point drives several subsystems in turn, so most of its lines say `cli` and the
/// ones speaking for another subsystem call log_line() with that tag explicitly.
static constexpr const char* LOG_TAG = sendspin_cli::LOG_TAG_CLI;

namespace {

using namespace sendspin_cli;  // NOLINT(google-build-using-namespace) -- this is the app itself
using sendspin::LogLevel;

/// How long to sleep between client.loop() calls. The library does its own timing on a
/// background thread, so this only bounds how quickly the main loop reacts to events.
constexpr int LOOP_INTERVAL_MS = 10;

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) {
    g_running.store(false);
}

/// A monotonic millisecond count, for pacing redials and the mDNS retry.
///
/// steady_clock rather than system_clock so neither schedule is disturbed by the host's
/// wall clock being stepped, which on a small player is most likely to happen at boot --
/// exactly when the retry loop is busiest.
int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The library asks the platform whether the network is usable before it starts serving.
/// On a host it always is: if the interface were down, bind() would be the thing to fail.
struct HostNetworkProvider : sendspin::SendspinNetworkProvider {
    bool is_network_ready() override {
        return true;
    }
};

/// The library's own persistence hook, answered out of the state store.
///
/// Both pairs are the library's to drive rather than ours. The last-server hash is produced by
/// `ConnectionManager::fnv1_hash()`, which is not installed, so this stores the number it is
/// handed and hands it straight back -- the library uses it to prefer the last-played server among
/// *inbound* connections. The static delay it reads at `start_server()` and writes on every
/// change, which is what closes the spec's requirement that clients persist `static_delay_ms`
/// across reboots and server reconnections. Applying that delay to the audio path is a separate
/// want, and is docs/ROADMAP.md item 13's.
///
/// Volume and mute are deliberately absent: the provider has no hook for either, so that half is
/// PlayerListener's.
class CliPersistenceProvider final : public sendspin::SendspinPersistenceProvider {
public:
    /// `store` must outlive this provider, which must in turn outlive the client.
    explicit CliPersistenceProvider(StateStore& store) : store_(store) {}

    bool save_last_server_hash(uint32_t hash) override {
        return this->store_.set_last_server_hash(hash);
    }

    std::optional<uint32_t> load_last_server_hash() override {
        return this->store_.last_server_hash();
    }

    bool save_static_delay(uint16_t delay_ms) override {
        return this->store_.set_static_delay_ms(delay_ms);
    }

    std::optional<uint16_t> load_static_delay() override {
        return this->store_.static_delay_ms();
    }

private:
    StateStore& store_;
};

/// Logs what the server says is playing, and keeps the last of it for `status` to read.
///
/// The object is cached rather than only logged because `status` needs the track and the
/// transport speed, and the role itself keeps neither: `MetadataRole` exposes the interpolated
/// progress and duration but not the artist, the title or the `playback_speed` they arrived
/// with.
///
/// THREAD SAFETY: both callbacks fire on the main loop thread via `drain_events()`, and
/// `state()` is read from the control socket's poll on that same thread, so the cache needs no
/// synchronisation.
struct MetadataLogger : sendspin::MetadataRoleListener {
    void on_metadata(const sendspin::ServerMetadataStateObject& metadata) override {
        this->state_ = metadata;
        if (!metadata.title.has_value()) {
            return;
        }
        log_line(LogLevel::INFO, LOG_TAG_METADATA, "Now playing: %s - %s",
                 metadata.artist.value_or("Unknown artist").c_str(), metadata.title->c_str());
    }

    void on_metadata_clear() override {
        // Cleared rather than kept: the server has said this metadata no longer describes
        // anything, so leaving it would have `status` reporting a track that has gone.
        this->state_.reset();
        log_line(LogLevel::INFO, LOG_TAG_METADATA, "Metadata cleared");
    }

    /// The last metadata the server sent, or nothing since the last clear.
    const std::optional<sendspin::ServerMetadataStateObject>& state() const {
        return this->state_;
    }

private:
    std::optional<sendspin::ServerMetadataStateObject> state_;
};

/// The formats to advertise for `sink`, derived from what its device will actually take.
///
/// Logged as well as returned: a field report on "the server never sent me anything I could
/// play" starts with what went out in `client/hello`, and on a derived list that is a
/// property of the host rather than of this binary.
std::vector<sendspin::AudioSupportedFormatObject> advertised_formats(const AudioSink& sink) {
    std::vector<sendspin::AudioSupportedFormatObject> formats =
        supported_formats(sink.capabilities());
    if (formats.empty()) {
        // The device opens but takes nothing this player emits. Advertising an empty list
        // would leave the server unable to send anything at all, so fall back to the
        // permissive set and let the refusal path report per stream what really happens.
        log_line(LogLevel::WARN, LOG_TAG_AUDIO,
                 "Output device '%s' reports no format sendspin-cli can emit -- advertising "
                 "everything and letting the device refuse per stream",
                 sink.name().c_str());
        formats = supported_formats(SinkCapabilities::permissive());
    }

    log_line(LogLevel::INFO, LOG_TAG_AUDIO, "Advertising %zu formats for '%s': %s", formats.size(),
             sink.name().c_str(), describe_formats(formats).c_str());
    // The digest above groups the axes, which cannot show which combinations really went
    // out. At debug the entries are listed one per line, exactly as the server sees them.
    for (const sendspin::AudioSupportedFormatObject& format : formats) {
        log_line(LogLevel::DEBUG, LOG_TAG_AUDIO, "  %s", describe_formats({format}).c_str());
    }
    return formats;
}

/// The outbound half of the daemon: choose a server, dial it, and keep dialling.
///
/// Only used when -s was given. The library deliberately does not do this for us --
/// `ConnectionManager::connect_to()` turns auto-reconnect off, and there is no connect or
/// disconnect callback on `SendspinClientListener` -- and in this direction nothing else
/// re-establishes the link: per the spec, "servers cannot reclaim clients by reconnecting".
class OutboundMode {
public:
    /// `store` must outlive this mode, and is where the chosen server is remembered.
    OutboundMode(const Options& opts, MdnsService& mdns, StateStore& store)
        : opts_(opts), mdns_(mdns), store_(store), remembered_(store.last_server()) {
        if (!this->remembered_.empty() && opts.discover) {
            // Only worth saying when discovering: with an address there is nothing to
            // choose between, and the memory only exists to break that tie.
            log_line(LogLevel::INFO, LOG_TAG_OUTBOUND,
                     "Last server used was \"%s\" -- it wins if it turns up among the candidates",
                     this->remembered_.c_str());
        }
    }

    /// @brief One main-loop tick: reconnect if it is time to, and remember what answered.
    ///
    /// Must run on the main loop thread, which is what makes the connect_to() below legal.
    void tick(sendspin::SendspinClient& client, int64_t now_ms) {
        const bool connected = client.is_connected();
        if (this->pacer_.note_connection_state(connected, now_ms)) {
            log_line(LogLevel::WARN, LOG_TAG_OUTBOUND, "Connection lost -- reconnecting in %u ms",
                     this->pacer_.delay_ms());
            // The next connection may be to a different server, so it is owed its own look.
            this->remembered_this_connection_ = false;
        }
        // Covers an inbound connection too: a server that dialled us first is a connection,
        // and dialling out over the top of it would only fight with it.
        if (connected) {
            this->remember(client);
            return;
        }
        if (!this->pacer_.should_dial(now_ms)) {
            return;
        }

        std::string url;
        if (this->opts_.discover) {
            if (!this->choose(url)) {
                return;
            }
        } else {
            url = this->opts_.server_url;
            log_line(LogLevel::INFO, LOG_TAG_OUTBOUND, "Connecting to %s", url.c_str());
        }

        // Stamped before the dial rather than after, so the backoff measures from when the
        // attempt started -- which is the whole point of pacing from the dial.
        this->pacer_.note_dial(now_ms);
        client.connect_to(url);
    }

private:
    /// Picks a discovered server, or reports that there is nothing to dial yet.
    bool choose(std::string& url) {
        const std::vector<DiscoveredServer> servers = this->mdns_.servers();
        std::string reason;
        const DiscoveredServer* chosen =
            select_server(servers, this->opts_.discover_name, this->remembered_, reason);
        if (chosen == nullptr) {
            return false;
        }
        std::string error;
        if (!discovered_server_url(*chosen, url, error)) {
            // Discovery already said why, at debug, when the instance first resolved.
            return false;
        }
        log_line(LogLevel::INFO, LOG_TAG_OUTBOUND,
                 "Connecting to %s (server \"%s\") -- chosen because %s", url.c_str(),
                 chosen->instance.c_str(), reason.c_str());
        return true;
    }

    /// Records the server a handshake just completed with, so a later run can prefer it.
    ///
    /// The spec's own concept is the last *playback* server, but v0.7.0 has neither
    /// `activities` nor `server/activate`, so a completed handshake is the strongest signal
    /// available here. Named for what it actually is rather than for what the spec means.
    void remember(sendspin::SendspinClient& client) {
        // Once per connection, not once per tick: this runs at the main loop's rate, and
        // get_server_information() builds a fresh object with its strings on every call.
        if (this->remembered_this_connection_) {
            return;
        }
        const std::optional<sendspin::ServerInformationObject> info =
            client.get_server_information();
        if (!info.has_value() || info->server_id.empty() || info->server_id == this->remembered_) {
            return;
        }
        this->remembered_this_connection_ = true;
        this->remembered_ = info->server_id;
        if (this->store_.path().empty()) {
            // Already said once at startup, where it covers everything else the store holds.
            return;
        }
        if (this->store_.set_last_server(this->remembered_)) {
            log_line(LogLevel::DEBUG, LOG_TAG_OUTBOUND, "Remembered server \"%s\" in %s",
                     this->remembered_.c_str(), this->store_.path().c_str());
        } else {
            log_line(LogLevel::WARN, LOG_TAG_OUTBOUND,
                     "Could not write %s -- this server will not be preferred after a restart",
                     this->store_.path().c_str());
        }
    }

    const Options& opts_;
    MdnsService& mdns_;
    StateStore& store_;
    std::string remembered_;
    bool remembered_this_connection_{false};
    RetryPacer pacer_;
};

/// Answers one control request out of the daemon's own state.
///
/// THREAD SAFETY: every member below is reached only from handle_control_request(), which
/// ControlSocket::poll() calls on the main loop. That is the whole reason the control channel
/// has no thread of its own: `send_command()` reaches `SendspinClient::send_text()` and
/// `ConnectionManager::current()`, documented main-thread-only, and `get_controller_state()`
/// returns a reference to a vector `drain_events()` move-assigns from inside `client.loop()`.
class ControlDispatcher final : public ControlHandler {
public:
    /// Every reference must outlive this dispatcher, which in main() they all do.
    ControlDispatcher(const Options& opts, sendspin::SendspinClient& client,
                      sendspin::ControllerRole& controller, sendspin::MetadataRole& metadata,
                      const MetadataLogger& metadata_logger,
                      const PlayerListener& player_listener, sendspin::PlayerRole& player,
                      const AudioSink& sink)
        : opts_(opts), client_(client), controller_(controller), metadata_(metadata),
          metadata_logger_(metadata_logger), player_listener_(player_listener), player_(player),
          sink_(sink) {}

    std::string handle_control_request(const std::string& line) override {
        std::string name;
        std::vector<std::string> args;
        if (!split_control_line(line, name, args)) {
            return encode_control_reply(ControlStatus::Usage, "empty request", "");
        }

        ControlRequest request;
        std::string error;
        // Parsed again on this side rather than trusted: the peer is whatever can reach the
        // socket, and the subcommand's own parse says nothing about what actually arrived.
        if (!parse_control_request(name, args, request, error)) {
            return encode_control_reply(ControlStatus::Usage, error, "");
        }

        ControlStatus refusal = ControlStatus::Failed;
        std::string reason;
        if (control_refusal(request, this->controller_snapshot(), refusal, reason)) {
            return encode_control_reply(refusal, reason, "");
        }

        if (request.command == ControlCommand::Status) {
            return encode_control_reply(ControlStatus::Ok, "", format_status(this->status()));
        }

        // Above send_command(), because this one is not a command to send: the static delay is this
        // endpoint's own player-role state. update_static_delay() persists it through
        // CliPersistenceProvider and republishes `client/state` itself, so the server learns the
        // new value without a controller command carrying it. The parser has already bounded the
        // value, so nothing here can reach the library's silent clamp.
        //
        // Safe from this thread, which is worth stating because it is the only place this daemon
        // writes into the player role from the control channel: the delay the sync task subtracts
        // is an atomic it re-reads per chunk, and the persistence write and `publish_state()` that
        // follow are both main-loop work, which is where this dispatcher runs. A change mid-stream
        // therefore lands on the next chunk and re-times its scheduling -- expect a brief resync.
        if (request.command == ControlCommand::Delay) {
            const uint16_t delay_ms = request.delay_ms.value_or(0);
            this->player_.update_static_delay(delay_ms);
            // Logged here rather than left to the listener, which the library invokes only for a
            // server's own `set_static_delay` -- so a locally set delay would otherwise be the one
            // change to this value that left no trace in the log.
            log_line(LogLevel::INFO, LOG_TAG_PLAYER, "Static delay set to %u ms locally",
                     static_cast<unsigned>(delay_ms));
            return encode_control_reply(ControlStatus::Ok, "", "");
        }

        log_line(LogLevel::DEBUG, LOG_TAG_CONTROL, "Sending '%s'",
                 encode_control_request(request).c_str());
        this->controller_.send_command(to_client_command(request));
        return encode_control_reply(ControlStatus::Ok, "", "");
    }

private:
    /// The server's controller state, copied out of the role for the reason control.h gives.
    ControllerSnapshot controller_snapshot() const {
        ControllerSnapshot snapshot;
        snapshot.connected = this->client_.is_connected();
        const sendspin::ServerStateControllerObject& state =
            this->controller_.get_controller_state();
        snapshot.supported_commands = state.supported_commands;
        snapshot.seek_max_ms = state.seek_max_ms;
        return snapshot;
    }

    StatusSnapshot status() const {
        StatusSnapshot snapshot;
        snapshot.name = this->opts_.name;
        snapshot.connected = this->client_.is_connected();
        const std::optional<sendspin::ServerInformationObject> info =
            this->client_.get_server_information();
        if (info.has_value()) {
            snapshot.server_id = info->server_id;
            snapshot.server_name = info->name;
        }

        const std::optional<sendspin::ServerMetadataStateObject>& metadata =
            this->metadata_logger_.state();
        if (metadata.has_value()) {
            snapshot.artist = metadata->artist.value_or("");
            snapshot.title = metadata->title.value_or("");
            if (metadata->progress.has_value()) {
                // The presence of a progress object is what makes the transport state and the
                // position knowable at all; the role's own getters return 0 either way, which
                // is indistinguishable from the start of a track.
                snapshot.playback_speed = metadata->progress->playback_speed;
                snapshot.progress_ms = this->metadata_.get_track_progress_ms();
                snapshot.duration_ms = this->metadata_.get_track_duration_ms();
            }
        }

        // Two separate facts, read separately. A stream whose format the device refused is
        // streaming with no format, and inferring one from the other would report the case the
        // player complains loudest about as `stream: idle`.
        snapshot.streaming = this->player_listener_.streaming();
        snapshot.format = this->player_listener_.stream_format();

        const sendspin::ServerStateControllerObject& controller =
            this->controller_.get_controller_state();
        // An empty supported_commands is how "no server/state has arrived" and "the connection
        // dropped" both look, since on_controller_state_clear() empties it -- so it is the
        // right test for whether the group figures below mean anything.
        snapshot.group_state_known =
            snapshot.connected && !controller.supported_commands.empty();
        snapshot.group_volume = controller.volume;
        snapshot.group_muted = controller.muted;
        snapshot.group_repeat = controller.repeat;
        snapshot.group_shuffle = controller.shuffle;

        // From the listener rather than from PlayerRole: the listener is the only caller of
        // `AudioSink::set_volume()`, so it is the only thing that knows what the sink was told --
        // and `VolumeSource` has no equivalent in the role at all.
        snapshot.player_volume = this->player_listener_.applied_volume();
        snapshot.player_muted = this->player_listener_.applied_muted();
        snapshot.player_volume_source = this->player_listener_.volume_source();
        // The other way round for the delay, and for the mirror-image reason: the role is the
        // authority, and it is changed by a path -- `delay` on the control socket -- that does not
        // invoke the listener. Any shadow here would be stale the moment that ran.
        snapshot.static_delay_ms = this->player_.get_static_delay_ms();
        snapshot.output = this->sink_.name();
        return snapshot;
    }

    const Options& opts_;
    sendspin::SendspinClient& client_;
    sendspin::ControllerRole& controller_;
    sendspin::MetadataRole& metadata_;
    const MetadataLogger& metadata_logger_;
    const PlayerListener& player_listener_;
    /// Non-const: `delay` reaches update_static_delay() through it. Every other member here is
    /// read-only, which is what makes this the one request that changes the player's own state.
    sendspin::PlayerRole& player_;
    const AudioSink& sink_;
};

/// Binds the control socket, or explains why this run has none.
///
/// Mirrors start_advertising(), and for the same reason: a player with no control channel is
/// still a player, so a socket that cannot be bound names its reason and the run carries on
/// rather than leaving a silence that reads like a bug.
///
/// One failure is not like that. A lock already held means the operator started a *second*
/// instance, which is what `-P` refuses outright -- so it is reported here and refused by the
/// caller.
/// @return false only when this run must stop.
bool start_control_socket(ControlSocket& socket, const Options& opts) {
    if (opts.no_control) {
        log_line(LogLevel::INFO, LOG_TAG_CONTROL,
                 "Not listening on a control socket: --no-control was given");
        return true;
    }
    if (opts.control_socket.empty()) {
        // No source produced a usable directory -- neither $XDG_RUNTIME_DIR nor, where the
        // platform has one, its own. Never a /tmp fallback: a world-writable directory would let
        // any local account pause playback and switch this endpoint out of its group.
        log_line(LogLevel::WARN, LOG_TAG_CONTROL, "No control socket: %s",
                 opts.control_absent_reason.c_str());
        return true;
    }

    std::string error;
    switch (socket.open(opts.control_socket, error)) {
        case ControlSocketStatus::Ok:
            log_line(LogLevel::INFO, LOG_TAG_CONTROL, "Listening on %s",
                     opts.control_socket.c_str());
            return true;
        case ControlSocketStatus::AlreadyRunning:
            log_fatal(LOG_TAG_CONTROL, "%s", error.c_str());
            return false;
        case ControlSocketStatus::Failed:
            break;
    }
    log_line(LogLevel::WARN, LOG_TAG_CONTROL,
             "%s -- carrying on without a control socket; this player can still be driven by its "
             "server",
             error.c_str());
    return true;
}

/// Starts the mDNS advertisement, or explains why this run has none.
///
/// The suppression rule is the spec's: "Do not advertise `_sendspin._tcp` if the client
/// plans to initiate the connection", which is what stops both ends dialling each other.
/// So it names the flag that caused it -- "not advertising" on its own reads like a bug.
void start_advertising(MdnsService& mdns, const Options& opts) {
    if (!opts.advertises()) {
        if (opts.was_given(Opt::Server)) {
            log_line(LogLevel::INFO, LOG_TAG_MDNS,
                     "Not advertising %s: -s makes this player the one initiating the "
                     "connection, and the Sendspin spec forbids advertising while it is",
                     MDNS_CLIENT_SERVICE);
        } else {
            log_line(LogLevel::INFO, LOG_TAG_MDNS, "Not advertising %s: --no-mdns was given",
                     MDNS_CLIENT_SERVICE);
        }
        return;
    }

    if (!mdns_available()) {
        log_line(LogLevel::INFO, LOG_TAG_MDNS,
                 "This build has no mDNS support, so it cannot be discovered: point a server at "
                 "ws://<this-host>:%u%s, or dial one with -s. See docs/ROADMAP.md.",
                 opts.port, SENDSPIN_PATH);
        return;
    }

    std::string error;
    if (!mdns.advertise(opts.mdns_name, opts.port, SENDSPIN_PATH, opts.name, error)) {
        // Not fatal: the player still serves on its port, so a server that is told the URL
        // can still reach it. The retry inside MdnsService keeps trying meanwhile.
        log_line(LogLevel::WARN, LOG_TAG_MDNS,
                 "%s -- retrying; until it succeeds, point a server at ws://<this-host>:%u%s",
                 error.c_str(), opts.port, SENDSPIN_PATH);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        std::fprintf(stderr, "Try '%s --help' for the full flag list.\n", argv[0]);
        return 1;
    }
    if (opts.show_help) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (opts.show_version) {
        print_version(stdout);
        return 0;
    }
    if (opts.list_devices) {
        print_audio_devices(stdout);
        return 0;
    }

    // Above every line below, and that is the whole contract: a subcommand run must not open an
    // audio device, take a pidfile, start a WebSocket server or touch mDNS. It talks to a player
    // that has already done all of that.
    if (!opts.subcommand.empty()) {
        ControlRequest request;
        std::string error;
        // Cannot fail here in practice -- parse_options() already ran the same parse to validate
        // the line -- but the request is built once, where it is used, rather than carried
        // through Options as a second representation of the same words.
        if (!parse_control_request(opts.subcommand, opts.subcommand_args, request, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return static_cast<int>(ControlStatus::Usage);
        }
        return static_cast<int>(run_control_subcommand(request, opts.control_socket,
                                                       opts.control_absent_reason, stdout));
    }

    // Probed here, above -f, purely so "already running" reaches the terminal: -f replaces
    // stderr, so nothing after it can be said to the shell that is still watching. The child
    // takes the lock for real after the fork, and that acquisition is the authoritative one.
    if (opts.daemonize && !opts.pidfile.empty()) {
        std::string error;
        if (probe_pidfile(opts.pidfile, error) != PidFileStatus::Ok) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }

    // Probed here for exactly the same reason, and only the *lock* is: the socket itself has to
    // be bound after the fork, so its own "already running" refusal would land in a log the
    // shell has already stopped watching. Refused only when the lock is held -- every other
    // failure is one the run carries on past, so it is left to the child to report.
    if (opts.daemonize && !opts.control_socket.empty()) {
        std::string error;
        if (probe_control_socket(opts.control_socket, error) ==
            ControlSocketStatus::AlreadyRunning) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }

    // Opened before -z forks, so an unopenable path still fails at the terminal, and so the
    // child inherits an fd 2 that already points at the logfile.
    if (!opts.logfile.empty() && !log_to_file(opts.logfile)) {
        return 1;
    }

    // Set once, and never again: the library's own level is a plain non-atomic int read from
    // its background threads, so changing it on a running player would be a data race.
    sendspin::SendspinClient::set_log_level(opts.log_level);

    // Which file the options came from, said even when there is nothing to say: it is the first
    // thing any support question needs, and "no config file" is as useful an answer as a path.
    //
    // **Above everything a configured value can kill the run with** -- the pidfile, the control
    // socket, the output device -- because those are exactly the failures where knowing which file
    // supplied the offending value is the whole diagnosis. A config with `output = hw:9,0` dies in
    // make_audio_sink() naming only the device, so this line has to already be in the log. It is
    // still below log_to_file(), so under -z it lands in the daemon's own log rather than on a
    // terminal that is about to be detached.
    if (opts.config_path.empty()) {
        cli_log(LogLevel::INFO, "No config file found; every option came from the command line "
                                "or a built-in default");
    } else {
        cli_log(LogLevel::INFO, "Config file: %s", opts.config_path.c_str());
    }

    // Returns only in the child. Everything cheap and fallible has been hoisted above it; from
    // here a failure reports into the log, which README.md says out loud -- so from here the
    // reports go through log_fatal(), which puts them in the log's own format.
    if (opts.daemonize) {
        // Named rather than passed inline: at the call site a bare boolean says nothing about
        // which way round it reads.
        const bool discard_stderr = opts.logfile.empty();
        std::string error;
        if (!daemonize(discard_stderr, error)) {
            log_fatal(LOG_TAG, "%s", error.c_str());
            return 1;
        }
    }

    // A closed downstream pipe on -o stdout must not kill the daemon: the sink notices
    // the short write and degrades to discarding.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    if (!opts.logfile.empty()) {
        // Only with -f. SIGHUP's default disposition is terminate, which is what a foreground
        // run should keep doing when its terminal closes -- staying alive with stderr on a
        // dead pty is worse than exiting.
        //
        // sigaction() rather than the std::signal() above, because this is the one handler
        // expected to fire again and again: a rotation arrives every day for the life of the
        // daemon. Only sigaction() *guarantees* the handler survives its own delivery -- if it
        // were reset, the second rotation would kill the player instead of reopening its log.
        // SA_RESTART matches what BSD-semantics signal() already gives the handlers above.
        struct sigaction hup = {};
        hup.sa_handler = log_handle_sighup;
        sigemptyset(&hup.sa_mask);
        hup.sa_flags = SA_RESTART;
        sigaction(SIGHUP, &hup, nullptr);
    }

    // Above make_audio_sink() on purpose: two instances racing should collide on the pidfile,
    // not on the sound card. A lost race that has already opened ALSA exclusively is a worse
    // failure than one that has opened nothing.
    PidFile pidfile;
    if (!opts.pidfile.empty()) {
        std::string error;
        if (pidfile.acquire(opts.pidfile, error) != PidFileStatus::Ok) {
            log_fatal(LOG_TAG, "%s", error.c_str());
            return 1;
        }
    }

    // Above make_audio_sink() for the reason the pidfile is: two instances racing should collide
    // on a lock, not on the sound card or on the WebSocket port. Below daemonize() because a
    // socket is exactly what that fork invariant forbids opening above it -- and because bind()
    // has to apply the 0600 umask daemonize() would otherwise have replaced with 0022.
    //
    // Only the listener is opened here. Nothing is answered until the main loop polls it, which
    // is after start_server(), so a `status` can never describe a player that is not up yet.
    ControlSocket control_socket;
    if (!start_control_socket(control_socket, opts)) {
        return 1;
    }

    std::string sink_error;
    std::unique_ptr<AudioSink> sink = make_audio_sink(opts.device, opts.buffer_ms, sink_error);
    if (!sink) {
        log_fatal(LOG_TAG_AUDIO, "%s", sink_error.c_str());
        return 1;
    }

    // What this daemon remembers about itself: the server it last handshook with, the static delay
    // a server set, and the volume and mute it was last applying. Loaded before the client exists,
    // because every one of those has to be in place before anything can be published or dialled.
    StateStore state_store(state_store_path(opts.state_dir));
    if (state_store.path().empty()) {
        log_line(LogLevel::DEBUG, LOG_TAG,
                 "Nothing set --state-dir, $XDG_STATE_HOME or $HOME, so this run remembers "
                 "nothing across restarts");
    } else {
        size_t malformed_line = 0;
        switch (state_store.load(malformed_line)) {
            case StateLoadResult::Loaded:
            case StateLoadResult::Absent:
                // Neither is worth a word beyond the path: a first run has no file, and a good one
                // needs no remark.
                log_line(LogLevel::DEBUG, LOG_TAG, "State file: %s", state_store.path().c_str());
                break;
            case StateLoadResult::Corrupt:
                // Not fatal -- this is a file we wrote ourselves, so refusing to start over it
                // would strand the player. Said at WARN anyway, because the alternative is a
                // volume and a static delay silently reverting to defaults, and the next change
                // overwriting the evidence that anything was ever wrong.
                log_line(LogLevel::WARN, LOG_TAG,
                         "%s:%zu is not readable state -- starting with nothing remembered, and "
                         "overwriting the file on the next change",
                         state_store.path().c_str(), malformed_line);
                break;
        }
    }
    CliPersistenceProvider persistence(state_store);

    sendspin::SendspinClientConfig config;
    // client_id is left empty on purpose: the library then derives a stable id from the
    // network interface MAC, which is the right identity for a fixed endpoint.
    config.name = opts.name;
    config.product_name = "sendspin-cli";
    config.manufacturer = "sendspin-cpp-cli";
    config.software_version = SENDSPIN_CLI_VERSION;
    config.server_port = opts.port;

    sendspin::SendspinClient client(std::move(config));

    // Above both add_player() and start_server(), and neither is negotiable: the pointer is copied
    // into PlayerRole at construction, and start_server() is what loads the remembered server hash.
    // Installed after either, it is a provider the library never asks. `persistence` outlives
    // `client` by being declared above it.
    client.set_persistence_provider(&persistence);

    sendspin::PlayerRoleConfig player_config;
    player_config.audio_formats = advertised_formats(*sink);
    // Explicitly 0, and it must stay 0: both real sinks already report *future* finish timestamps
    // that include their own buffering -- snd_pcm_delay() on ALSA, outputBufferDacTime on
    // PortAudio -- so folding device latency in here would count it twice and push playout early
    // by the ring's worth of audio. The library's own default is 0; it is written out so the
    // constraint lives beside the field rather than only in the roadmap.
    player_config.fixed_delay_us = 0;
    // `extra_startup_silence_ms` is deliberately left unassigned, at the library's
    // DEFAULT_EXTRA_STARTUP_SILENCE_MS of 50. It trades startup latency for decode-pipeline
    // headroom, and choosing a different figure needs underflow measurements on real hardware
    // across both backends -- not a guess from here.

    // A first-run default only: PlayerRole::load_static_delay() prefers whatever
    // CliPersistenceProvider hands back and reads this solely when nothing was persisted, so a
    // remembered delay wins. Set before add_player(), which is where the role loads it.
    player_config.initial_static_delay_ms = opts.static_delay_ms;
    sendspin::PlayerRole& player = client.add_player(std::move(player_config));
    // Advertises the `static_delay` command, which is also what makes the stored delay *apply*:
    // the library reports it as 0 in `client/state` and ignores it in sync timing while
    // adjustability is off, per the spec's rule that a delay not exposed as a knob must not be
    // applied.
    player.set_static_delay_adjustable(true);
    sendspin::MetadataRole& metadata = client.add_metadata();
    // Added unconditionally, so `client/hello` always carries `controller@v1` -- including under
    // --no-control, and including on a host with no $XDG_RUNTIME_DIR to put a socket in. That is
    // deliberate: which roles this client speaks is a property of the build, not of whether one
    // particular way of reaching it happens to be available. A server that saw the role appear
    // and disappear with an environment variable would have no way to plan around it.
    sendspin::ControllerRole& controller = client.add_controller();

    PlayerListener player_listener(player, *sink, &state_store);
    MetadataLogger metadata_logger;
    HostNetworkProvider network_provider;

    player.set_listener(&player_listener);
    metadata.set_listener(&metadata_logger);
    client.set_network_provider(&network_provider);

    // Put the remembered gain back, and report the gain the sink is really applying, before
    // anything can connect. Persisting these is the spec's RECOMMENDED for players; with nothing
    // remembered the pair falls back to what an untouched sink is already doing.
    //
    // Reporting it is not cosmetic and not a preference -- three spec rules make it necessary,
    // and they apply to a restored figure exactly as they did to the default. `client/state`'s
    // `volume` MUST be included when a player advertises the `volume` command, which this one
    // does. Group volume is *derived* from us: "Group volume is the average of the volumes of
    // players in the group that support the `volume` command", so a player reporting a figure it
    // is not applying corrupts the group reading for every controller in the group. And setting
    // group volume works off "delta = requested_volume - current_group_volume", so that wrong
    // figure then mis-applies every later group volume change by exactly the error -- a player
    // claiming 0 while playing at full hears a request for 30 as a cut from full, not a rise.
    //
    // The library's own default is 0 while every AudioSink starts at DEFAULT_SINK_VOLUME, so
    // without this the two disagree from the first message. The pair goes to the sink through
    // restore_volume() and to the role through update_volume()/update_muted(), which is what keeps
    // all three in step; neither role call invokes the listener's callbacks, which fire only for
    // server-initiated changes, so `status` still reports this as remembered rather than as
    // something a server chose.
    const std::optional<uint8_t> remembered_volume = state_store.volume();
    const std::optional<bool> remembered_muted = state_store.muted();
    const uint8_t volume = remembered_volume.value_or(DEFAULT_SINK_VOLUME);
    const bool muted = remembered_muted.value_or(false);
    // Only when something really was remembered: restore_volume() is also what marks the pair as
    // restored, and a run that found nothing must still report its volume as the default nobody
    // chose rather than claiming an earlier run picked it.
    if (remembered_volume.has_value() || remembered_muted.has_value()) {
        player_listener.restore_volume(volume, muted);
        log_line(LogLevel::INFO, LOG_TAG_PLAYER, "Restored volume %u%s from %s",
                 static_cast<unsigned>(volume), muted ? " (muted)" : "",
                 state_store.path().c_str());
    }
    player.update_volume(volume);
    player.update_muted(muted);

    if (!client.start_server()) {
        log_fatal(LOG_TAG, "could not start the Sendspin server on port %u", opts.port);
        return 1;
    }

    cli_log(LogLevel::INFO,
            "sendspin-cli %s listening on port %u as \"%s\" (output: %s, mDNS: %s)",
            SENDSPIN_CLI_VERSION, opts.port, opts.name.c_str(), sink->name().c_str(),
            mdns_backend_name().c_str());

    // Started after start_server(), so the port being advertised is one that is already
    // accepting -- a server that discovers us and dials immediately then finds a listener.
    MdnsService mdns;
    start_advertising(mdns, opts);

    // What answers the socket opened above. Built here rather than there because it holds
    // references to the client and its roles, all of which outlive it.
    ControlDispatcher control_dispatcher(opts, client, controller, metadata, metadata_logger,
                                        player_listener, player, *sink);

    // Already validated during parsing, so there is nothing left here that can be wrong --
    // and nothing to fail on after the server is up.
    std::unique_ptr<OutboundMode> outbound;
    if (opts.was_given(Opt::Server)) {
        if (opts.discover) {
            std::string error;
            if (!mdns.browse(error)) {
                log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "%s -- retrying", error.c_str());
            }
            log_line(LogLevel::INFO, LOG_TAG_DISCOVERY, "Looking for a Sendspin server on %s%s%s%s",
                     MDNS_SERVER_SERVICE, opts.discover_name.empty() ? "" : " named \"",
                     opts.discover_name.c_str(), opts.discover_name.empty() ? "" : "\"");
        }
        outbound = std::make_unique<OutboundMode>(opts, mdns, state_store);
    }

    while (g_running.load()) {
        const int64_t now_ms = monotonic_ms();
        client.loop();
        // All three of these run their callbacks on this thread, which is what each of them
        // requires: dns_sd's and connect_to()'s for the first two, and for the control socket
        // every read of the roles plus send_command() itself. A round trip is therefore bounded
        // by LOOP_INTERVAL_MS rather than by the socket -- the right trade, since the
        // alternative is a thread touching ConnectionManager::current() off the main loop.
        mdns.poll(now_ms);
        control_socket.poll(now_ms, control_dispatcher);
        if (outbound) {
            outbound->tick(client, now_ms);
        }
        // Here for the same reason the three above are: what a sink does with this tick is work
        // that may not run on the sync task's thread. PortAudioSink rebuilds its device list in
        // it after a device has gone away mid-stream, which is why the sink gets the loop's
        // clock rather than reading one of its own.
        sink->poll(now_ms);
        // Here rather than in the SIGHUP handler: the reopen flushes the old stream and then
        // logs the result, and neither fflush() nor fprintf() is async-signal-safe -- the
        // open/dup2 pair on its own would be. This is what hands rotation to logrotate and
        // newsyslog.
        log_reopen_if_requested();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }

    cli_log(LogLevel::INFO, "Shutting down");
    // Withdrawn before the client goes, so a restart does not race a record still naming a
    // port nothing is listening on. The control socket goes for the same reason and in the same
    // place: a request accepted after the client has disconnected would be answered out of a
    // half-torn-down player, and its path must be gone before a restart tries to bind it.
    mdns.stop();
    control_socket.close();
    client.disconnect(sendspin::SendspinGoodbyeReason::SHUTDOWN);
    sink->stop();
    return 0;
}
