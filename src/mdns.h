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

/// Advertising `_sendspin._tcp` and discovering `_sendspin-server._tcp`.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sendspin_cli {

/// What a player advertises so a server can dial it.
inline constexpr const char* MDNS_CLIENT_SERVICE = "_sendspin._tcp";

/// What a server advertises so a player can dial it.
inline constexpr const char* MDNS_SERVER_SERVICE = "_sendspin-server._tcp";

/// Longest DNS-SD instance label, in bytes; a longer name is truncated.
inline constexpr size_t MDNS_MAX_LABEL_BYTES = 63;

/// Longest single TXT record value, in bytes.
inline constexpr size_t MDNS_MAX_TXT_VALUE_BYTES = 255;

/// A `_sendspin-server._tcp` instance that resolved to something dialable.
struct DiscoveredServer {
    /// The instance label, which is also the server's protocol `server_id`.
    std::string instance;

    std::string name;                    ///< TXT `name`, the server's friendly name. Optional.
    std::string path;                    ///< TXT `path`. Required, and must start with '/'.
    uint16_t port{0};                    ///< SRV port.
    std::vector<std::string> addresses;  ///< Resolved literals, in the order they arrived.
};

/// The URL to dial: first routable IPv4, else IPv6; refuses a bad TXT `path` or port 0.
/// @return true if `server` yielded a URL.
bool discovered_server_url(const DiscoveredServer& server, std::string& url, std::string& error);

/// Picks the server to dial: `name_filter` must match, then the remembered one, else the first.
/// @param remembered `instance` of the last server whose handshake completed, or empty.
/// @param reason Set to why the winner won, for logging.
/// @return The chosen server, or nullptr when nothing matched.
const DiscoveredServer* select_server(const std::vector<DiscoveredServer>& servers,
                                      const std::string& name_filter, const std::string& remembered,
                                      std::string& reason);

/// Truncates `text` to at most `max_bytes`, never splitting a UTF-8 sequence.
std::string truncate_utf8(const std::string& text, size_t max_bytes);

/// True if this build can advertise and discover at all.
bool mdns_available();

/// The mDNS implementation this build has, for diagnostics.
std::string mdns_backend_name();

/// The mDNS registration and browse this daemon owns.
/// Every method must be called on the main loop thread.
class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    MdnsService(const MdnsService&) = delete;
    MdnsService& operator=(const MdnsService&) = delete;

    /// Registers `_sendspin._tcp`; the registered name may differ after a collision.
    /// @param instance Label to ask for; truncated to MDNS_MAX_LABEL_BYTES.
    /// @param friendly_name TXT `name`; omitted when empty.
    /// @return true if the registration was accepted for processing.
    bool advertise(const std::string& instance, uint16_t port, const std::string& path,
                   const std::string& friendly_name, std::string& error);

    /// Starts browsing `_sendspin-server._tcp` for the daemon's lifetime.
    /// @return true if the browse was accepted for processing.
    bool browse(std::string& error);

    /// Processes ready results without blocking, and retries failed registrations.
    /// @param now_ms Monotonic milliseconds, for the re-registration backoff.
    void poll(int64_t now_ms);

    /// The servers discovered so far, in the order they resolved.
    std::vector<DiscoveredServer> servers() const;

    /// Withdraws the registration and stops browsing. Idempotent; call before disconnecting.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sendspin_cli
