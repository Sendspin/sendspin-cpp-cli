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

/// @file mdns_common.cpp
/// @brief The parts of discovery that are pure string work, built into every configuration
///
/// Separate from mdns_dnssd.cpp so a build without `dns_sd.h` still compiles -- and still
/// tests -- the URL construction and the selection rule, which is where the protocol
/// decisions actually live.

#include "mdns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <string>
#include <vector>

namespace sendspin_cli {

namespace {

/// Which family an address literal is in, and whether it is usable as a dial target.
enum class AddressKind {
    Invalid,   ///< not an address literal at all
    Unusable,  ///< link-local or unspecified: unroutable, or names no host
    IPv4,      ///< a routable IPv4 literal
    IPv6,      ///< a routable IPv6 literal
};

/// Classifies one address literal the way the reference server's `_get_first_valid_ip()`
/// does: anything that does not parse, is link-local, or is unspecified is not a candidate.
///
/// Link-local is 169.254.0.0/16 for IPv4 and fe80::/10 for IPv6. A link-local address is
/// only reachable with a scope this player was never told, so dialling it would produce a
/// connection failure the user cannot act on.
AddressKind classify_address(const std::string& text) {
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        const uint32_t host_order = ntohl(v4.s_addr);
        const bool link_local = (host_order & 0xFFFF0000U) == 0xA9FE0000U;
        const bool unspecified = host_order == 0;
        return (link_local || unspecified) ? AddressKind::Unusable : AddressKind::IPv4;
    }

    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        const bool link_local = v6.s6_addr[0] == 0xFE && (v6.s6_addr[1] & 0xC0) == 0x80;
        bool unspecified = true;
        for (unsigned char byte : v6.s6_addr) {
            if (byte != 0) {
                unspecified = false;
                break;
            }
        }
        return (link_local || unspecified) ? AddressKind::Unusable : AddressKind::IPv6;
    }

    return AddressKind::Invalid;
}

}  // namespace

bool discovered_server_url(const DiscoveredServer& server, std::string& url, std::string& error) {
    // The spec makes TXT `path` REQUIRED, and the reference server drops any instance whose
    // path does not start with '/'. Matching that is what keeps a misconfigured peer from
    // being dialled at a URL neither side agrees on.
    if (server.path.empty()) {
        error = "no TXT 'path' record";
        return false;
    }
    if (server.path.front() != '/') {
        error = "TXT 'path' is '" + server.path + "', which does not start with '/'";
        return false;
    }
    if (server.port == 0) {
        error = "no SRV port";
        return false;
    }

    // IPv4 wins over IPv6 whenever both are advertised: the reference server binds
    // V4Only, so an IPv6 answer from it is a route to a listener that may not exist.
    const std::string* first_v6 = nullptr;
    for (const std::string& address : server.addresses) {
        switch (classify_address(address)) {
            case AddressKind::IPv4:
                url = "ws://" + address + ":" + std::to_string(server.port) + server.path;
                return true;
            case AddressKind::IPv6:
                if (first_v6 == nullptr) {
                    first_v6 = &address;
                }
                break;
            case AddressKind::Unusable:
            case AddressKind::Invalid:
                break;
        }
    }

    if (first_v6 != nullptr) {
        // Bracketed, which is also the only IPv6 form parse_server_url() accepts, so a URL
        // built here and one typed at -s read the same.
        url = "ws://[" + *first_v6 + "]:" + std::to_string(server.port) + server.path;
        return true;
    }

    error = server.addresses.empty() ? "no address yet"
                                     : "no routable address (only link-local or unspecified)";
    return false;
}

const DiscoveredServer* select_server(const std::vector<DiscoveredServer>& servers,
                                      const std::string& name_filter, const std::string& remembered,
                                      std::string& reason) {
    const DiscoveredServer* first_match = nullptr;

    for (const DiscoveredServer& server : servers) {
        if (!name_filter.empty() && server.name != name_filter) {
            continue;
        }
        // The remembered id is the instance label, so this match needs no handshake -- which
        // is the whole point: it has to be decidable before we choose who to dial.
        if (!remembered.empty() && server.instance == remembered) {
            reason = "it is the last server whose handshake completed";
            return &server;
        }
        if (first_match == nullptr) {
            first_match = &server;
        }
    }

    if (first_match == nullptr) {
        return nullptr;
    }
    reason = name_filter.empty() ? "it resolved first"
                                 : "it resolved first among those named '" + name_filter + "'";
    return first_match;
}

std::string truncate_utf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    // Back up off any continuation byte (10xxxxxx), so the result never ends mid-sequence.
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

}  // namespace sendspin_cli
