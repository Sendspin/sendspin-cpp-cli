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

#include <cstdint>
#include <memory>
#include <string>

namespace ix {
class HttpServer;
}

namespace sendspin_cli {

/// The application-specific role that advertises the endpoint in `client/hello`. Provisional.
inline constexpr const char* LOCAL_PAIRING_ROLE = "_ohf_local_pairing@v1";

/// The only path the endpoint answers.
inline constexpr const char* PAIRING_TOKEN_PATH = "/pairing-token";

/// Reads `<address>:<port>`. The address must be a numeric IPv4 or bracketed IPv6 literal and
/// not a wildcard, so the endpoint can never listen on every interface.
/// @param host Set to the address without brackets.
/// @param error Set to a human-readable reason when the return value is false.
bool parse_pairing_endpoint(const std::string& value, std::string& host, uint16_t& port,
                            std::string& error);

/// The role's support object: `{"port":N}`.
std::string local_pairing_support_json(uint16_t port);

/// An HTTP status and JSON body.
struct PairingReply {
    int status;
    std::string body;
};

/// The answer to one request: the client id and token on `GET /pairing-token`, an error otherwise.
PairingReply pairing_token_reply(const std::string& method, const std::string& uri,
                                 const std::string& client_id, const std::string& token);

/// The HTTP listener; stops in its destructor.
class PairingEndpoint {
public:
    PairingEndpoint(std::string host, uint16_t port);
    ~PairingEndpoint();
    PairingEndpoint(const PairingEndpoint&) = delete;
    PairingEndpoint& operator=(const PairingEndpoint&) = delete;

    /// Binds the address. Connections wait in the backlog until start().
    bool listen(std::string& error);

    /// Starts answering with this identity. Call after listen() succeeded.
    void start(const std::string& client_id, const std::string& token);

    void stop();

private:
    std::string host_;
    uint16_t port_;
    std::unique_ptr<ix::HttpServer> server_;
};

}  // namespace sendspin_cli
