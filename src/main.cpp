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

#include "audio_sink.h"
#include "cli.h"
#include "daemon.h"
#include "last_server.h"
#include "log.h"
#include "mdns.h"
#include "outbound.h"
#include "player_listener.h"
#include "supported_formats.h"

#include <sendspin/client.h>
#include <sendspin/config.h>
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

/// Logs what the server says is playing. For a headless daemon this is the cheapest
/// proof that the connection is live and carrying real state.
struct MetadataLogger : sendspin::MetadataRoleListener {
    void on_metadata(const sendspin::ServerMetadataStateObject& metadata) override {
        if (!metadata.title.has_value()) {
            return;
        }
        log_line(LogLevel::INFO, LOG_TAG_METADATA, "Now playing: %s - %s",
                 metadata.artist.value_or("Unknown artist").c_str(), metadata.title->c_str());
    }

    void on_metadata_clear() override {
        log_line(LogLevel::INFO, LOG_TAG_METADATA, "Metadata cleared");
    }
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
    OutboundMode(const Options& opts, MdnsService& mdns)
        : opts_(opts), mdns_(mdns), state_path_(last_server_path()) {
        if (this->state_path_.empty()) {
            log_line(LogLevel::DEBUG, LOG_TAG_OUTBOUND,
                     "Neither $XDG_STATE_HOME nor $HOME is set, so the server used will not be "
                     "remembered across restarts");
        } else if (load_last_server(this->state_path_, this->remembered_) && opts.discover) {
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
        if (this->state_path_.empty()) {
            return;
        }
        if (save_last_server(this->state_path_, this->remembered_)) {
            log_line(LogLevel::DEBUG, LOG_TAG_OUTBOUND, "Remembered server \"%s\" in %s",
                     this->remembered_.c_str(), this->state_path_.c_str());
        } else {
            log_line(LogLevel::WARN, LOG_TAG_OUTBOUND,
                     "Could not write %s -- this server will not be preferred after a restart",
                     this->state_path_.c_str());
        }
    }

    const Options& opts_;
    MdnsService& mdns_;
    std::string state_path_;
    std::string remembered_;
    bool remembered_this_connection_{false};
    RetryPacer pacer_;
};

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

    // Opened before -z forks, so an unopenable path still fails at the terminal, and so the
    // child inherits an fd 2 that already points at the logfile.
    if (!opts.logfile.empty() && !log_to_file(opts.logfile)) {
        return 1;
    }

    // Set once, and never again: the library's own level is a plain non-atomic int read from
    // its background threads, so changing it on a running player would be a data race.
    sendspin::SendspinClient::set_log_level(opts.log_level);

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

    std::string sink_error;
    std::unique_ptr<AudioSink> sink = make_audio_sink(opts.device, opts.buffer_ms, sink_error);
    if (!sink) {
        log_fatal(LOG_TAG_AUDIO, "%s", sink_error.c_str());
        return 1;
    }

    sendspin::SendspinClientConfig config;
    // client_id is left empty on purpose: the library then derives a stable id from the
    // network interface MAC, which is the right identity for a fixed endpoint.
    config.name = opts.name;
    config.product_name = "sendspin-cli";
    config.manufacturer = "sendspin-cpp-cli";
    config.software_version = SENDSPIN_CLI_VERSION;
    config.server_port = opts.port;

    sendspin::SendspinClient client(std::move(config));

    sendspin::PlayerRoleConfig player_config;
    player_config.audio_formats = advertised_formats(*sink);
    sendspin::PlayerRole& player = client.add_player(std::move(player_config));
    player.set_static_delay_adjustable(true);
    sendspin::MetadataRole& metadata = client.add_metadata();

    PlayerListener player_listener(player, *sink);
    MetadataLogger metadata_logger;
    HostNetworkProvider network_provider;

    player.set_listener(&player_listener);
    metadata.set_listener(&metadata_logger);
    client.set_network_provider(&network_provider);

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
        outbound = std::make_unique<OutboundMode>(opts, mdns);
    }

    while (g_running.load()) {
        const int64_t now_ms = monotonic_ms();
        client.loop();
        // Both of these run dns_sd callbacks and connect_to() on this thread, which is what
        // each of them requires.
        mdns.poll(now_ms);
        if (outbound) {
            outbound->tick(client, now_ms);
        }
        // Here rather than in the SIGHUP handler: the reopen flushes the old stream and then
        // logs the result, and neither fflush() nor fprintf() is async-signal-safe -- the
        // open/dup2 pair on its own would be. This is what hands rotation to logrotate and
        // newsyslog.
        log_reopen_if_requested();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }

    cli_log(LogLevel::INFO, "Shutting down");
    // Withdrawn before the client goes, so a restart does not race a record still naming a
    // port nothing is listening on.
    mdns.stop();
    client.disconnect(sendspin::SendspinGoodbyeReason::SHUTDOWN);
    sink->stop();
    return 0;
}
