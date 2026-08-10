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

/// @file mdns.h
/// @brief Advertising `_sendspin._tcp` and discovering `_sendspin-server._tcp`

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief What a player advertises so a server can dial it.
///
/// From the Sendspin spec (connection.md, "Server-Initiated Connections"): the client
/// advertises this type on the port it serves, with a REQUIRED TXT `path` and an optional
/// TXT `name`.
inline constexpr const char* MDNS_CLIENT_SERVICE = "_sendspin._tcp";

/// @brief What a server advertises so a player can dial it.
///
/// From the same spec section ("Client-Initiated Connections"). Deliberately *not*
/// MDNS_CLIENT_SERVICE, which upstream's `examples/tui_client` browses when looking for
/// servers -- that finds other players. The reference server (aiosendspin) registers this
/// type in `SendspinServer._start_mdns_advertising()` and browses the client type, which
/// is the arrangement the spec describes.
inline constexpr const char* MDNS_SERVER_SERVICE = "_sendspin-server._tcp";

/// @brief The longest a DNS-SD instance label may be, in bytes.
///
/// One DNS label, so 63 octets. A longer `-n` is truncated rather than rejected: the name
/// is cosmetic, and refusing to start over a long friendly name would be a worse trade
/// than advertising a shortened one and saying so.
inline constexpr size_t MDNS_MAX_LABEL_BYTES = 63;

/// @brief The longest a single TXT record value may be, in bytes.
inline constexpr size_t MDNS_MAX_TXT_VALUE_BYTES = 255;

/// @brief A `_sendspin-server._tcp` instance that resolved to something dialable.
struct DiscoveredServer {
    /// The instance label, which is also the protocol `server_id`: the reference server
    /// registers itself as `f"{self._id}.{service_type}"` and sends that same `self._id` as
    /// `server_id` in `server/hello`. That equality is what lets a remembered server be
    /// recognised at browse time, before any handshake.
    std::string instance;

    std::string name;                    ///< TXT `name`, the server's friendly name. Optional.
    std::string path;                    ///< TXT `path`. Required, and must start with '/'.
    uint16_t port{0};                    ///< SRV port.
    std::vector<std::string> addresses;  ///< Resolved literals, in the order they arrived.
};

/// @brief Turns a discovered instance into the URL to dial, or explains why it cannot.
///
/// Prefers the first non-link-local IPv4 and falls back to the first non-link-local IPv6,
/// which is then bracketed -- the same filter the reference server applies in
/// `_get_first_valid_ip()`, and the same preference its own listener binds with. A
/// link-local or unspecified address is skipped rather than dialled: it is either
/// unroutable from here or names no host at all.
///
/// Rejects an instance whose TXT `path` is missing or does not start with '/', matching the
/// reference server, and one whose SRV port is 0.
/// @param error Set to a human-readable reason when the return value is false.
/// @return true if `server` yielded a URL.
bool discovered_server_url(const DiscoveredServer& server, std::string& url, std::string& error);

/// @brief Picks which discovered server to dial.
///
/// The spec leaves this open -- "How clients handle multiple discovered servers, server
/// selection, and switching is implementation-defined" -- so the rule is: a `-s mdns:<name>`
/// filter is a hard constraint on the TXT `name`, and among what survives it the remembered
/// server wins, else the first to have resolved.
///
/// @param servers Candidates in the order they resolved.
/// @param name_filter TXT `name` to require, or empty for no filter.
/// @param remembered `instance` of the last server whose handshake completed, or empty.
/// @param reason Set to why the winner won, for logging.
/// @return The chosen server, or nullptr when nothing matched.
const DiscoveredServer* select_server(const std::vector<DiscoveredServer>& servers,
                                      const std::string& name_filter, const std::string& remembered,
                                      std::string& reason);

/// @brief Truncates `text` to at most `max_bytes`, never splitting a UTF-8 sequence.
///
/// mDNS counts bytes, so a name that fits a label in characters may not in octets --
/// and dns_sd takes TXT value lengths as a `uint8_t`, where an over-long value would
/// silently wrap rather than fail.
std::string truncate_utf8(const std::string& text, size_t max_bytes);

/// @brief True if this build can advertise and discover at all.
///
/// False where `dns_sd.h` was not found at configure time, in which case MdnsService is a
/// no-op that reports why. Mirrors the SENDSPIN_CLI_HAVE_MDNS compile definition, which is
/// what the parser uses to reject `-s mdns:` before the daemon starts.
bool mdns_available();

/// @brief Names the mDNS implementation this build has, for diagnostics.
std::string mdns_backend_name();

/// @brief The mDNS registration and browse this daemon owns.
///
/// One implementation per build -- `mdns_dnssd.cpp` where `dns_sd.h` was found, `mdns_null.cpp`
/// where it was not -- chosen by CMake rather than at runtime, so there is no vtable and no
/// registry.
///
/// THREAD SAFETY: every method must be called on the main loop thread, and poll() is what
/// runs the daemon's callbacks. That is not a convenience: a browse result is what triggers
/// SendspinClient::connect_to(), which is documented as main-loop-only.
class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    MdnsService(const MdnsService&) = delete;
    MdnsService& operator=(const MdnsService&) = delete;

    /// @brief Registers `_sendspin._tcp` so a server can discover this player.
    ///
    /// The daemon may auto-rename on a collision, so the name that actually registered is
    /// reported through the register callback and logged, not assumed to be `instance`.
    /// @param instance Instance label to ask for; truncated to MDNS_MAX_LABEL_BYTES.
    /// @param port The port this player's own WebSocket server listens on.
    /// @param path TXT `path` -- the WebSocket endpoint on that port.
    /// @param friendly_name TXT `name`; omitted from the record when empty.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @return true if the registration was accepted for processing.
    bool advertise(const std::string& instance, uint16_t port, const std::string& path,
                   const std::string& friendly_name, std::string& error);

    /// @brief Starts browsing `_sendspin-server._tcp`, and keeps browsing.
    ///
    /// Left open for the daemon's lifetime, so a server that appears later becomes a
    /// candidate for the next retry rather than needing a restart.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @return true if the browse was accepted for processing.
    bool browse(std::string& error);

    /// @brief Runs whatever the daemon has ready, without blocking, and retries what failed.
    ///
    /// Every registration and query is polled with a zero timeout and read only when its
    /// descriptor is already readable -- DNSServiceProcessResult() blocks until a result
    /// arrives, so it must never be called speculatively.
    /// @param now_ms A monotonic clock in milliseconds, for the re-registration backoff.
    void poll(int64_t now_ms);

    /// @brief The servers discovered so far, in the order they resolved.
    std::vector<DiscoveredServer> servers() const;

    /// @brief Withdraws the registration and stops browsing.
    ///
    /// Called explicitly during shutdown, before the client disconnects, so a restart does
    /// not race its own stale record. Idempotent; the destructor calls it too.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sendspin_cli
