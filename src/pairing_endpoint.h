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

/// --pairing-token-endpoint: this player's pairing token over HTTP on one internal address.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sendspin_cli {

/// The application-specific role that advertises the endpoint in `client/hello`. Provisional.
inline constexpr const char* LOCAL_PAIRING_ROLE = "_ohf_local_pairing@v1";

/// The only path the endpoint answers.
inline constexpr const char* PAIRING_TOKEN_PATH = "/";

/// Connections served at once; a further one is closed as soon as it is accepted.
inline constexpr int PAIRING_ENDPOINT_MAX_CONNECTIONS = 4;

/// The most a request head may take. The endpoint never reads a body.
inline constexpr size_t PAIRING_REQUEST_HEAD_LIMIT = 4096;

/// Reads `<address>:<port>`. The address must be a numeric IPv4 or bracketed IPv6 literal and
/// not a wildcard, so the endpoint can never listen on every interface.
/// @param host Set to the address without brackets.
/// @param error Set to a human-readable reason when the return value is false.
bool parse_pairing_endpoint(const std::string& value, std::string& host, uint16_t& port,
                            std::string& error);

/// `[address]` for an IPv6 address, the address itself otherwise.
std::string bracketed(const std::string& address);

/// The role's support object: `{"port":N}`.
std::string local_pairing_support_json(uint16_t port);

/// An address and the length of its subnet's prefix.
struct IpSubnet {
    int family{0};                            ///< AF_INET or AF_INET6
    std::array<unsigned char, 16> address{};  ///< network order; IPv4 uses the first 4 bytes
    int prefix{0};
};

/// Reads a numeric IPv4 or IPv6 address, without brackets.
bool parse_ip(const std::string& text, int& family, std::array<unsigned char, 16>& address);

/// Whether `ip` is an address inside `subnet`.
bool subnet_contains(const IpSubnet& subnet, const std::string& ip);

/// The subnet of the local interface `address` belongs to, the narrowest if several hold it.
bool local_subnet_of(const std::string& address, IpSubnet& subnet, std::string& error);

/// The method, target and Host header of a request head; false when it is not a request.
bool parse_request_head(const std::string& head, std::string& method, std::string& uri,
                        std::string& host);

/// An HTTP status and plain-text body.
struct PairingReply {
    int status;
    std::string body;
};

/// The answer to one request: the token on `GET /`, an error otherwise.
/// @param host The request's Host header. It must name the endpoint itself, so a page that
///     rebinds its own hostname to the address gets no token.
/// @param address The listening address, without brackets.
PairingReply pairing_token_reply(const std::string& method, const std::string& uri,
                                 const std::string& host, const std::string& address, uint16_t port,
                                 const std::string& token);

class PairingServer;

/// The HTTP listener. Answers only peers on its address's own subnet; stops in its destructor.
class PairingEndpoint {
public:
    PairingEndpoint(std::string host, uint16_t port);
    ~PairingEndpoint();
    PairingEndpoint(const PairingEndpoint&) = delete;
    PairingEndpoint& operator=(const PairingEndpoint&) = delete;

    /// Binds the address and finds its subnet. Connections wait in the backlog until start().
    bool listen(std::string& error);

    /// Starts answering with this token. Call after listen() succeeded.
    void start(const std::string& token);

    void stop();

private:
    std::string host_;
    uint16_t port_;
    IpSubnet subnet_;
    std::unique_ptr<PairingServer> server_;
};

}  // namespace sendspin_cli
