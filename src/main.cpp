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

#include "audio_sink.h"
#include "cli.h"
#include "log.h"
#include "player_listener.h"
#include "supported_formats.h"

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/types.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
        cli_log(LogLevel::INFO, "Now playing: %s - %s",
                metadata.artist.value_or("Unknown artist").c_str(), metadata.title->c_str());
    }

    void on_metadata_clear() override {
        cli_log(LogLevel::INFO, "Metadata cleared");
    }
};

/// Writes a pidfile and removes it again on destruction, so every exit path -- including
/// a failed start_server() -- leaves no stale file behind.
class PidFile {
public:
    PidFile() = default;

    ~PidFile() {
        if (!this->path_.empty()) {
            std::remove(this->path_.c_str());
        }
    }

    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    bool write(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) {
            cli_log(LogLevel::ERROR, "cannot write pidfile %s: %s", path.c_str(),
                    std::strerror(errno));
            return false;
        }
        std::fprintf(file, "%ld\n", static_cast<long>(getpid()));
        if (std::fclose(file) != 0) {
            cli_log(LogLevel::ERROR, "cannot write pidfile %s: %s", path.c_str(),
                    std::strerror(errno));
            return false;
        }
        this->path_ = path;
        return true;
    }

private:
    std::string path_;
};

/// Points stderr -- where all logging goes -- at a file, appending so a restart does not
/// truncate history.
bool redirect_log(const std::string& path) {
    if (std::freopen(path.c_str(), "a", stderr) == nullptr) {
        // stderr is now unusable, so complain on stdout instead.
        std::fprintf(stdout, "error: cannot open logfile %s: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }
    return true;
}

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
        cli_log(LogLevel::WARN,
                "Output device '%s' reports no format sendspin-cli can emit -- advertising "
                "everything and letting the device refuse per stream",
                sink.name().c_str());
        formats = supported_formats(SinkCapabilities::permissive());
    }

    cli_log(LogLevel::INFO, "Advertising %zu formats for '%s': %s", formats.size(),
            sink.name().c_str(), describe_formats(formats).c_str());
    // The digest above groups the axes, which cannot show which combinations really went
    // out. At debug the entries are listed one per line, exactly as the server sees them.
    for (const sendspin::AudioSupportedFormatObject& format : formats) {
        cli_log(LogLevel::DEBUG, "  %s", describe_formats({format}).c_str());
    }
    return formats;
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

    if (opts.daemonize) {
        // Failing loudly beats pretending: a service manager that was told to expect a
        // forked daemon would otherwise wait forever on a foreground process.
        std::fprintf(stderr,
                     "error: -z (daemonize) is not implemented yet. Run in the foreground and "
                     "supervise with systemd or a container. See docs/ROADMAP.md.\n");
        return 1;
    }

    if (!opts.logfile.empty() && !redirect_log(opts.logfile)) {
        return 1;
    }

    sendspin::SendspinClient::set_log_level(opts.log_level);

    // A closed downstream pipe on -o stdout must not kill the daemon: the sink notices
    // the short write and degrades to discarding.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::string sink_error;
    std::unique_ptr<AudioSink> sink = make_audio_sink(opts.device, opts.buffer_ms, sink_error);
    if (!sink) {
        std::fprintf(stderr, "error: %s\n", sink_error.c_str());
        return 1;
    }

    PidFile pidfile;
    if (!opts.pidfile.empty() && !pidfile.write(opts.pidfile)) {
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
        std::fprintf(stderr, "error: could not start the Sendspin server on port %u\n", opts.port);
        return 1;
    }

    cli_log(LogLevel::INFO, "sendspin-cli %s listening on port %u as \"%s\" (output: %s)",
            SENDSPIN_CLI_VERSION, opts.port, opts.name.c_str(), sink->name().c_str());
    cli_log(LogLevel::INFO,
            "This build does not advertise over mDNS yet: point a server at "
            "ws://<this-host>:%u/sendspin, or dial one with -s. See docs/ROADMAP.md.",
            opts.port);

    // Already validated and resolved during parsing, so there is nothing left to get
    // wrong here -- and nothing to fail on after the server is up.
    if (!opts.server_url.empty()) {
        cli_log(LogLevel::INFO, "Connecting to %s", opts.server_url.c_str());
        client.connect_to(opts.server_url);
    }

    while (g_running.load()) {
        client.loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_INTERVAL_MS));
    }

    cli_log(LogLevel::INFO, "Shutting down");
    client.disconnect(sendspin::SendspinGoodbyeReason::SHUTDOWN);
    sink->stop();
    return 0;
}
