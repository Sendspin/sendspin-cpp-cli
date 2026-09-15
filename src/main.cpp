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

/// sendspin-cli: a headless Sendspin player, and its own control-socket client.

#include "audio_sink.h"
#include "cli.h"
#include "control.h"
#include "daemon.h"
#include "hooks.h"
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

static constexpr const char* LOG_TAG = sendspin_cli::LOG_TAG_CLI;

namespace {

using namespace sendspin_cli;  // NOLINT(google-build-using-namespace) -- this is the app itself
using sendspin::LogLevel;

/// Sleep between client.loop() calls; bounds main-loop reaction time only.
constexpr int LOOP_INTERVAL_MS = 10;

/// How long shutdown keeps pumping client.loop() for the stream's end (~50 ms needed).
constexpr int SHUTDOWN_DRAIN_MS = 500;

std::atomic<bool> g_running{true};

void handle_signal(int /*sig*/) {
    g_running.store(false);
}

/// Monotonic milliseconds, immune to wall-clock steps at boot.
int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// On a host the network is always usable; bind() fails if it is not.
struct HostNetworkProvider : sendspin::SendspinNetworkProvider {
    bool is_network_ready() override {
        return true;
    }
};

/// The library's persistence hook (server hash, static delay), backed by the state store.
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

/// Logs server metadata and caches the latest for `status`. Main loop only.
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
        // Cleared so `status` does not report a track that has gone.
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

/// The formats to advertise for `sink`, derived from its device and logged.
std::vector<sendspin::AudioSupportedFormatObject> advertised_formats(const AudioSink& sink) {
    std::vector<sendspin::AudioSupportedFormatObject> formats =
        supported_formats(sink.capabilities());
    if (formats.empty()) {
        // Advertise the permissive set rather than nothing; refusals are reported per stream.
        log_line(LogLevel::WARN, LOG_TAG_AUDIO,
                 "Output device '%s' reports no format sendspin-cli can emit -- advertising "
                 "everything and letting the device refuse per stream",
                 sink.name().c_str());
        formats = supported_formats(SinkCapabilities::permissive());
    }

    log_line(LogLevel::INFO, LOG_TAG_AUDIO, "Advertising %zu formats for '%s': %s", formats.size(),
             sink.name().c_str(), describe_formats(formats).c_str());
    // Every entry at debug, exactly as the server sees it.
    for (const sendspin::AudioSupportedFormatObject& format : formats) {
        log_line(LogLevel::DEBUG, LOG_TAG_AUDIO, "  %s", describe_formats({format}).c_str());
    }
    return formats;
}

/// The outbound (-s) mode: choose a discovered server, dial it, and keep redialling.
class OutboundMode {
public:
    /// `store` must outlive this mode, and is where the chosen server is remembered.
    OutboundMode(const Options& opts, MdnsService& mdns, StateStore& store)
        : opts_(opts), mdns_(mdns), store_(store), remembered_(store.last_server()) {
        if (!this->remembered_.empty()) {
            log_line(LogLevel::INFO, LOG_TAG_OUTBOUND,
                     "Last server used was \"%s\" -- it wins if it turns up among the candidates",
                     this->remembered_.c_str());
        }
    }

    /// One main-loop tick: redial if due, and remember what answered. Main loop only.
    void tick(sendspin::SendspinClient& client, int64_t now_ms) {
        const bool connected = client.is_connected();
        if (this->pacer_.note_connection_state(connected, now_ms)) {
            log_line(LogLevel::WARN, LOG_TAG_OUTBOUND, "Connection lost -- reconnecting in %u ms",
                     this->pacer_.delay_ms());
            this->remembered_this_connection_ = false;
            // The lost connection takes its dial's URL with it.
            this->last_dial_.note_lost();
        }
        // Includes inbound connections: never dial over one.
        if (connected) {
            this->remember(client);
            return;
        }
        if (!this->pacer_.should_dial(now_ms)) {
            return;
        }

        std::string url;
        std::string server_id;
        if (!this->choose(url, server_id)) {
            return;
        }

        // Stamped before dialling, so the backoff measures from the attempt's start.
        this->pacer_.note_dial(now_ms);
        this->last_dial_.note_dial(url, server_id);
        client.connect_to(url);
    }

    /// SENDSPIN_SERVER_URL for a stream from `server_id`; empty unless our dial reached it.
    std::string url_for(const std::string& server_id) const {
        return this->last_dial_.url_for(server_id);
    }

private:
    /// Picks a discovered server; `server_id` is its instance label.
    bool choose(std::string& url, std::string& server_id) {
        const std::vector<DiscoveredServer> servers = this->mdns_.servers();
        std::string reason;
        const DiscoveredServer* chosen =
            select_server(servers, this->opts_.discover_name, this->remembered_, reason);
        if (chosen == nullptr) {
            return false;
        }
        std::string error;
        if (!discovered_server_url(*chosen, url, error)) {
            return false;
        }
        server_id = chosen->instance;
        log_line(LogLevel::INFO, LOG_TAG_OUTBOUND,
                 "Connecting to %s (server \"%s\") -- chosen because %s", url.c_str(),
                 chosen->instance.c_str(), reason.c_str());
        return true;
    }

    /// Remembers the server a handshake completed with, for a later run to prefer.
    void remember(sendspin::SendspinClient& client) {
        // Once per connection: this runs every tick.
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
    LastDial last_dial_;
    bool remembered_this_connection_{false};
    RetryPacer pacer_;
};

/// Answers control requests from the daemon's state. Main loop only.
class ControlDispatcher final : public ControlHandler {
public:
    /// Every reference must outlive this dispatcher, which in main() they all do.
    ControlDispatcher(const Options& opts, sendspin::SendspinClient& client,
                      sendspin::ControllerRole& controller, sendspin::MetadataRole& metadata,
                      const MetadataLogger& metadata_logger, const PlayerListener& player_listener,
                      sendspin::PlayerRole& player, const AudioSink& sink)
        : opts_(opts),
          client_(client),
          controller_(controller),
          metadata_(metadata),
          metadata_logger_(metadata_logger),
          player_listener_(player_listener),
          player_(player),
          sink_(sink) {}

    std::string handle_control_request(const std::string& line) override {
        std::string name;
        std::vector<std::string> args;
        if (!split_control_line(line, name, args)) {
            return encode_control_reply(ControlStatus::Usage, "empty request", "");
        }

        ControlRequest request;
        std::string error;
        // Re-parsed: the peer is whatever can reach the socket.
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

        // Handled locally: update_static_delay() persists and republishes; safe on the main loop.
        if (request.command == ControlCommand::Delay) {
            const uint16_t delay_ms = request.delay_ms.value_or(0);
            this->player_.update_static_delay(delay_ms);
            // The listener only logs server-set delays, so log this one here.
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
                // Only a progress object makes transport state and position knowable.
                snapshot.playback_speed = metadata->progress->playback_speed;
                snapshot.progress_ms = this->metadata_.get_track_progress_ms();
                snapshot.duration_ms = this->metadata_.get_track_duration_ms();
            }
        }

        // Read separately: a refused format is still streaming.
        snapshot.streaming = this->player_listener_.streaming();
        snapshot.format = this->player_listener_.stream_format();

        const sendspin::ServerStateControllerObject& controller =
            this->controller_.get_controller_state();
        // Empty supported_commands means no state yet, or a dropped connection.
        snapshot.group_state_known = snapshot.connected && !controller.supported_commands.empty();
        snapshot.group_volume = controller.volume;
        snapshot.group_muted = controller.muted;
        snapshot.group_repeat = controller.repeat;
        snapshot.group_shuffle = controller.shuffle;

        // From the listener, the only thing that knows what the sink was told.
        snapshot.player_volume = this->player_listener_.applied_volume();
        snapshot.player_muted = this->player_listener_.applied_muted();
        snapshot.player_volume_source = this->player_listener_.volume_source();
        // From the role: `delay` changes it without invoking the listener.
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
    /// Non-const only for `delay`.
    sendspin::PlayerRole& player_;
    const AudioSink& sink_;
};

/// Binds the control socket, or logs why this run has none.
/// @return false only when another instance holds the lock and this run must stop.
bool start_control_socket(ControlSocket& socket, const Options& opts) {
    if (opts.no_control) {
        log_line(LogLevel::INFO, LOG_TAG_CONTROL,
                 "Not listening on a control socket: --no-control was given");
        return true;
    }
    if (opts.control_socket.empty()) {
        // Never a /tmp fallback: any local account could control the player.
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

/// Starts the mDNS advertisement, or logs why this run has none.
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
                 "This build has no mDNS support, so it can neither be discovered nor discover a "
                 "server: point a server at ws://<this-host>:%u%s. See docs/ROADMAP.md.",
                 opts.port, SENDSPIN_PATH);
        return;
    }

    std::string error;
    if (!mdns.advertise(opts.mdns_name, opts.port, SENDSPIN_PATH, opts.name, error)) {
        // Not fatal: MdnsService keeps retrying.
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

    // First: a subcommand run must not open a device, pidfile, server or mDNS.
    if (!opts.subcommand.empty()) {
        ControlRequest request;
        std::string error;
        if (!parse_control_request(opts.subcommand, opts.subcommand_args, request, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return static_cast<int>(ControlStatus::Usage);
        }
        return static_cast<int>(run_control_subcommand(request, opts.control_socket,
                                                       opts.control_absent_reason, stdout));
    }

    // Probed above -f so "already running" reaches the terminal; the child takes the real lock.
    if (opts.daemonize && !opts.pidfile.empty()) {
        std::string error;
        if (probe_pidfile(opts.pidfile, error) != PidFileStatus::Ok) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }

    // Likewise only the socket's lock: the socket itself is bound after the fork.
    if (opts.daemonize && !opts.control_socket.empty()) {
        std::string error;
        if (probe_control_socket(opts.control_socket, error) ==
            ControlSocketStatus::AlreadyRunning) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }

    // Before -z forks, so an unopenable path fails at the terminal.
    if (!opts.logfile.empty() && !log_to_file(opts.logfile)) {
        return 1;
    }

    // Set once: the library reads this non-atomic level from its threads.
    sendspin::SendspinClient::set_log_level(opts.log_level);

    // Logged before anything a configured value can fail on.
    if (opts.config_path.empty()) {
        cli_log(LogLevel::INFO, "No config file found; every option came from the command line "
                                "or a built-in default");
    } else {
        cli_log(LogLevel::INFO, "Config file: %s", opts.config_path.c_str());
    }

    // Returns only in the child; from here failures go to the log via log_fatal().
    if (opts.daemonize) {
        const bool discard_stderr = opts.logfile.empty();
        std::string error;
        if (!daemonize(discard_stderr, error)) {
            log_fatal(LOG_TAG, "%s", error.c_str());
            return 1;
        }
    }

    // A closed pipe on -o stdout must not kill the daemon.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    if (!opts.logfile.empty()) {
        // Only with -f. sigaction() so the handler survives repeated rotations.
        struct sigaction hup = {};
        hup.sa_handler = log_handle_sighup;
        sigemptyset(&hup.sa_mask);
        hup.sa_flags = SA_RESTART;
        sigaction(SIGHUP, &hup, nullptr);
    }

    // Above make_audio_sink(), so racing instances collide on the pidfile, not the sound card.
    PidFile pidfile;
    if (!opts.pidfile.empty()) {
        std::string error;
        if (pidfile.acquire(opts.pidfile, error) != PidFileStatus::Ok) {
            log_fatal(LOG_TAG, "%s", error.c_str());
            return 1;
        }
    }

    // After daemonize() (fork invariant, umask) and before make_audio_sink(), like the pidfile.
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

    // Loaded before the client, since its values must be in place before anything is published.
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
                log_line(LogLevel::DEBUG, LOG_TAG, "State file: %s", state_store.path().c_str());
                break;
            case StateLoadResult::Corrupt:
                // Not fatal, but warned: the next write discards the evidence.
                log_line(LogLevel::WARN, LOG_TAG,
                         "%s:%zu is not readable state -- starting with nothing remembered, and "
                         "overwriting the file on the next change",
                         state_store.path().c_str(), malformed_line);
                break;
        }
    }
    CliPersistenceProvider persistence(state_store);

    sendspin::SendspinClientConfig config;
    // Empty lets the library derive an id from the MAC.
    config.client_id = opts.client_id;
    config.name = opts.name;
    config.product_name = opts.product_name;
    config.manufacturer = opts.manufacturer;
    config.software_version = SENDSPIN_CLI_VERSION;
    config.server_port = opts.port;

    sendspin::SendspinClient client(std::move(config));

    // Before add_player() and start_server(), or the library never asks it.
    client.set_persistence_provider(&persistence);

    std::vector<sendspin::AudioSupportedFormatObject> formats = advertised_formats(*sink);
    // Pins reorder the advertised list; a pin it does not carry stops the run.
    if (!opts.audio_formats.empty()) {
        const std::vector<sendspin::AudioSupportedFormatObject> missing =
            pin_preferred_formats(formats, opts.audio_formats);
        if (!missing.empty()) {
            log_fatal(LOG_TAG_AUDIO,
                      "--audio-format asked for %s, which %s not among the formats advertised "
                      "for output device '%s' -- refusing to start. Run with -l to see what the "
                      "device itself reports -- not the same set as what gets advertised.",
                      format_list_spec(missing).c_str(), missing.size() == 1 ? "is" : "are",
                      sink->name().c_str());
            return 1;
        }
        log_line(LogLevel::INFO, LOG_TAG_AUDIO, "Preferred formats offered first, in order: %s",
                 format_list_spec(opts.audio_formats).c_str());
    }

    sendspin::PlayerRoleConfig player_config;
    player_config.audio_formats = std::move(formats);
    // Must stay 0: both sinks' timestamps already include their buffering.
    player_config.fixed_delay_us = 0;
    // extra_startup_silence_ms stays at the library default until measured on hardware.

    // A first-run default: a persisted delay wins. Set before add_player() loads it.
    player_config.initial_static_delay_ms = opts.static_delay_ms;
    sendspin::PlayerRole& player = client.add_player(std::move(player_config));
    // Also what makes the stored delay apply: non-adjustable delays are ignored.
    player.set_static_delay_adjustable(true);
    sendspin::MetadataRole& metadata = client.add_metadata();
    // Always added: advertised roles are a property of the build, not of --no-control.
    sendspin::ControllerRole& controller = client.add_controller();

    PlayerListener player_listener(player, *sink, &state_store);
    MetadataLogger metadata_logger;
    HostNetworkProvider network_provider;

    player.set_listener(&player_listener);
    metadata.set_listener(&metadata_logger);
    client.set_network_provider(&network_provider);

    // Restore the remembered gain before connecting, so the role reports what the sink applies.
    const std::optional<uint8_t> remembered_volume = state_store.volume();
    const std::optional<bool> remembered_muted = state_store.muted();
    const uint8_t volume = remembered_volume.value_or(DEFAULT_SINK_VOLUME);
    const bool muted = remembered_muted.value_or(false);
    // Only when something was remembered, since restore_volume() marks the pair as restored.
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

    cli_log(LogLevel::INFO, "sendspin-cli %s listening on port %u as \"%s\" (output: %s, mDNS: %s)",
            SENDSPIN_CLI_VERSION, opts.port, opts.name.c_str(), sink->name().c_str(),
            mdns_backend_name().c_str());

    // After start_server(), so the advertised port is already accepting.
    MdnsService mdns;
    start_advertising(mdns, opts);

    ControlDispatcher control_dispatcher(opts, client, controller, metadata, metadata_logger,
                                         player_listener, player, *sink);

    std::unique_ptr<OutboundMode> outbound;
    if (opts.discover) {
        std::string error;
        if (!mdns.browse(error)) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "%s -- retrying", error.c_str());
        }
        log_line(LogLevel::INFO, LOG_TAG_DISCOVERY, "Looking for a Sendspin server on %s%s%s%s",
                 MDNS_SERVER_SERVICE, opts.discover_name.empty() ? "" : " named \"",
                 opts.discover_name.c_str(), opts.discover_name.empty() ? "" : "\"");
        outbound = std::make_unique<OutboundMode>(opts, mdns, state_store);
    }

    // Wired only when a hook was given; declared after `outbound`, which the callback captures.
    HookRunner hooks;
    // The running stream's facts, kept for its stop event, when the server info is already gone.
    HookContext stream_context;
    if (!opts.hook_start.empty() || !opts.hook_stop.empty()) {
        player_listener.on_stream_event = [&opts, &client, &hooks, &outbound,
                                           &stream_context](bool started) {
            // Captured even without --hook-start, for the stop hook.
            if (started) {
                stream_context = HookContext{};
                // Only an explicit --id is exported; the MAC-derived id is not exposed.
                stream_context.client_id = opts.client_id;
                stream_context.client_name = opts.name;
                const std::optional<sendspin::ServerInformationObject> info =
                    client.get_server_information();
                if (info.has_value()) {
                    stream_context.server_id = info->server_id;
                    stream_context.server_name = info->name;
                }
                if (outbound) {
                    stream_context.server_url = outbound->url_for(stream_context.server_id);
                }
            }
            const std::string& command = started ? opts.hook_start : opts.hook_stop;
            if (command.empty()) {
                return;
            }
            hooks.run(command, started ? "start" : "stop", stream_context);
        };
    }

    while (g_running.load()) {
        const int64_t now_ms = monotonic_ms();
        client.loop();
        // All three need their callbacks on the main loop.
        mdns.poll(now_ms);
        control_socket.poll(now_ms, control_dispatcher);
        if (outbound) {
            outbound->tick(client, now_ms);
        }
        // Main-loop work a sink cannot do on the sync task's thread.
        sink->poll(now_ms);
        hooks.poll();
        // Not in the SIGHUP handler: the reopen is not async-signal-safe.
        log_reopen_if_requested();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }

    cli_log(LogLevel::INFO, "Shutting down");
    // Before the client disconnects, so a restart does not race a stale record or socket.
    mdns.stop();
    control_socket.close();
    client.disconnect(sendspin::SendspinGoodbyeReason::SHUTDOWN);
    // Pump until the stream ends so the stop hook runs; disconnect() only asks.
    for (int waited_ms = 0; player_listener.streaming(); waited_ms += LOOP_INTERVAL_MS) {
        if (waited_ms >= SHUTDOWN_DRAIN_MS) {
            cli_log(LogLevel::WARN,
                    "The stream did not end within %d ms of disconnecting -- any --hook-stop "
                    "has not run",
                    SHUTDOWN_DRAIN_MS);
            break;
        }
        client.loop();
        hooks.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }
    // The stop hook may be pending behind a hung start hook; run it anyway.
    hooks.flush();
    // The lambda references locals destroyed before the listener; drop it first.
    player_listener.on_stream_event = nullptr;
    sink->stop();
    return 0;
}
