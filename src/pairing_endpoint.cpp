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

#include "pairing_endpoint.h"

#include "log.h"

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <array>
#include <cstdlib>
#include <utility>

namespace sendspin_cli {

namespace {

static constexpr const char* LOG_TAG = LOG_TAG_CLI;

constexpr int MAX_CONNECTIONS = 4;
constexpr int TIMEOUT_SECS = 5;

/// `::`, or `::ffff:0.0.0.0`, the IPv4 wildcard in mapped form.
bool is_ipv6_wildcard(const std::array<unsigned char, 16>& addr) {
    static constexpr std::array<unsigned char, 16> MAPPED_ANY{0, 0, 0,    0,    0, 0, 0, 0,
                                                              0, 0, 0xff, 0xff, 0, 0, 0, 0};
    return addr == std::array<unsigned char, 16>{} || addr == MAPPED_ANY;
}

}  // namespace

bool parse_pairing_endpoint(const std::string& value, std::string& host, uint16_t& port,
                            std::string& error) {
    const size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        error = "expected <address>:<port> with an explicit address";
        return false;
    }
    std::string address = value.substr(0, colon);
    const std::string port_text = value.substr(colon + 1);

    bool ipv6 = false;
    if (address.front() == '[') {
        if (address.size() < 3 || address.back() != ']') {
            error = "an IPv6 address needs its closing ']'";
            return false;
        }
        address = address.substr(1, address.size() - 2);
        ipv6 = true;
    } else if (address.find(':') != std::string::npos) {
        error = "bracket an IPv6 address, as in [fd00::1]:8930";
        return false;
    }

    if (ipv6) {
        std::array<unsigned char, 16> addr{};
        if (inet_pton(AF_INET6, address.c_str(), addr.data()) != 1) {
            error = "'" + address + "' is not a numeric IPv6 address";
            return false;
        }
        if (is_ipv6_wildcard(addr)) {
            error = "a wildcard address would listen on every interface";
            return false;
        }
    } else {
        in_addr addr{};
        if (inet_pton(AF_INET, address.c_str(), &addr) != 1) {
            error = "'" + address + "' is not a numeric IPv4 address";
            return false;
        }
        if (addr.s_addr == htonl(INADDR_ANY)) {
            error = "a wildcard address would listen on every interface";
            return false;
        }
    }

    // Digits only: strtoul accepts a sign and leading spaces.
    if (port_text.empty() || port_text.size() > 5 ||
        port_text.find_first_not_of("0123456789") != std::string::npos) {
        error = "expected a port of 1-65535";
        return false;
    }
    const unsigned long number = std::strtoul(port_text.c_str(), nullptr, 10);
    if (number == 0 || number > 65535UL) {
        error = "expected a port of 1-65535";
        return false;
    }

    host = address;
    port = static_cast<uint16_t>(number);
    return true;
}

std::string local_pairing_support_json(uint16_t port) {
    return "{\"port\":" + std::to_string(port) + "}";
}

PairingReply pairing_token_reply(const std::string& method, const std::string& uri,
                                 const std::string& client_id, const std::string& token) {
    const std::string path = uri.substr(0, uri.find('?'));
    if (path != PAIRING_TOKEN_PATH) {
        return {404, "{\"error\":\"not_found\"}"};
    }
    if (method != "GET") {
        return {405, "{\"error\":\"method_not_allowed\"}"};
    }
    // Both are base64url / base32 text, so neither needs JSON escaping.
    return {200, "{\"client_id\":\"" + client_id + "\",\"pairing_token\":\"" + token + "\"}"};
}

PairingEndpoint::PairingEndpoint(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

PairingEndpoint::~PairingEndpoint() {
    this->stop();
}

bool PairingEndpoint::listen(std::string& error) {
    ix::initNetSystem();
    const int family = this->host_.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    this->server_ = std::make_unique<ix::HttpServer>(this->port_, this->host_,
                                                     ix::SocketServer::kDefaultTcpBacklog,
                                                     MAX_CONNECTIONS, family, TIMEOUT_SECS);
    const std::pair<bool, std::string> result = this->server_->listen();
    if (!result.first) {
        error = result.second;
        this->server_.reset();
        return false;
    }
    return true;
}

void PairingEndpoint::start(const std::string& client_id, const std::string& token) {
    if (!this->server_) {
        return;
    }
    // A WebSocket upgrade bypasses the HTTP callback; without a handler ix logs an error.
    this->server_->setOnClientMessageCallback([](const std::shared_ptr<ix::ConnectionState>&,
                                                 ix::WebSocket& socket,
                                                 const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            socket.close();
        }
    });
    this->server_->setOnConnectionCallback([client_id, token](
                                               const ix::HttpRequestPtr& request,
                                               const std::shared_ptr<ix::ConnectionState>& state) {
        const PairingReply reply =
            pairing_token_reply(request->method, request->uri, client_id, token);
        // The URI is the caller's text, so it stays out of the log.
        log_line(sendspin::LogLevel::DEBUG, LOG_TAG, "Pairing token endpoint answered %s with %d",
                 state->getRemoteIp().c_str(), reply.status);
        ix::WebSocketHttpHeaders headers;
        headers["Content-Type"] = "application/json";
        headers["Cache-Control"] = "no-store";
        return std::make_shared<ix::HttpResponse>(reply.status,
                                                  reply.status == 200 ? "OK" : "Error",
                                                  ix::HttpErrorCode::Ok, headers, reply.body);
    });
    this->server_->start();
}

void PairingEndpoint::stop() {
    if (this->server_) {
        this->server_->stop();
        this->server_.reset();
    }
}

}  // namespace sendspin_cli
