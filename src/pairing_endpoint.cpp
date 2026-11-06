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

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXHttp.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketServer.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <utility>

namespace sendspin_cli {

namespace {

static constexpr const char* LOG_TAG = LOG_TAG_CLI;

constexpr int TIMEOUT_SECS = 5;

/// `::`, or `::ffff:0.0.0.0`, the IPv4 wildcard in mapped form.
bool is_ipv6_wildcard(const std::array<unsigned char, 16>& addr) {
    static constexpr std::array<unsigned char, 16> MAPPED_ANY{0, 0, 0,    0,    0, 0, 0, 0,
                                                              0, 0, 0xff, 0xff, 0, 0, 0, 0};
    return addr == std::array<unsigned char, 16>{} || addr == MAPPED_ANY;
}

bool equals_ignoring_case(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/// Counts itself in `live` from its accept until its worker thread is reaped.
class CountedConnection : public ix::ConnectionState {
public:
    explicit CountedConnection(std::shared_ptr<std::atomic<size_t>> live) : live_(std::move(live)) {
        ++*this->live_;
    }
    ~CountedConnection() override {
        --*this->live_;
    }
    CountedConnection(const CountedConnection&) = delete;
    CountedConnection& operator=(const CountedConnection&) = delete;

private:
    std::shared_ptr<std::atomic<size_t>> live_;
};

}  // namespace

/// One request per connection, each on its own worker thread.
///
/// Not ix::HttpServer: that counts only upgraded WebSocket clients against its connection limit,
/// so plain HTTP requests would each get a thread with no cap. This counts every connection.
class PairingServer final : public ix::SocketServer {
public:
    using Handler =
        std::function<ix::HttpResponsePtr(const ix::HttpRequestPtr&, const std::string& remote_ip)>;

    PairingServer(int port, const std::string& host, int family)
        : ix::SocketServer(port, host, kDefaultTcpBacklog, PAIRING_ENDPOINT_MAX_CONNECTIONS,
                           family) {
        // The factory runs on the accept thread before the next admission check, so a burst of
        // connections cannot slip past the limit before their workers start.
        this->setConnectionStateFactory(
            [live = this->live_] { return std::make_shared<CountedConnection>(live); });
    }

    // Joins the workers while handler_ is still alive; the base destructor would be too late.
    ~PairingServer() override {
        this->stop();
    }

    PairingServer(const PairingServer&) = delete;
    PairingServer& operator=(const PairingServer&) = delete;

    /// Call before start().
    void set_handler(Handler handler) {
        this->handler_ = std::move(handler);
    }

private:
    void handleConnection(std::unique_ptr<ix::Socket> socket,
                          std::shared_ptr<ix::ConnectionState> state) override {
        const auto parsed = ix::Http::parseRequest(socket, TIMEOUT_SECS);
        if (std::get<0>(parsed)) {
            ix::Http::sendResponse(this->handler_(std::get<2>(parsed), state->getRemoteIp()),
                                   socket);
        }
        state->setTerminated();
    }

    size_t getConnectedClientsCount() override {
        return *this->live_;
    }

    std::shared_ptr<std::atomic<size_t>> live_ = std::make_shared<std::atomic<size_t>>(0);
    Handler handler_;
};

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
                                 const std::string& host, const std::string& address, uint16_t port,
                                 const std::string& token) {
    // A client leaves the port out of Host only when it is the scheme's default.
    const bool names_endpoint = equals_ignoring_case(host, address + ":" + std::to_string(port)) ||
                                (port == 80 && equals_ignoring_case(host, address));
    if (!names_endpoint) {
        return {421, "misdirected request"};
    }
    const std::string path = uri.substr(0, uri.find('?'));
    if (path != PAIRING_TOKEN_PATH) {
        return {404, "not found"};
    }
    if (method != "GET") {
        return {405, "method not allowed"};
    }
    return {200, token};
}

PairingEndpoint::PairingEndpoint(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

PairingEndpoint::~PairingEndpoint() {
    this->stop();
}

bool PairingEndpoint::listen(std::string& error) {
    ix::initNetSystem();
    const int family = this->host_.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    this->server_ = std::make_unique<PairingServer>(this->port_, this->host_, family);
    const std::pair<bool, std::string> result = this->server_->listen();
    if (!result.first) {
        error = result.second;
        this->server_.reset();
        return false;
    }
    return true;
}

void PairingEndpoint::start(const std::string& token) {
    if (!this->server_) {
        return;
    }
    const std::string address =
        this->host_.find(':') != std::string::npos ? "[" + this->host_ + "]" : this->host_;
    const uint16_t port = this->port_;
    this->server_->set_handler(
        [token, address, port](const ix::HttpRequestPtr& request, const std::string& remote_ip) {
            const auto host = request->headers.find("Host");
            const PairingReply reply = pairing_token_reply(
                request->method, request->uri, host == request->headers.end() ? "" : host->second,
                address, port, token);
            // The URI and Host are the caller's text, so they stay out of the log.
            log_line(sendspin::LogLevel::DEBUG, LOG_TAG,
                     "Pairing token endpoint answered %s with %d", remote_ip.c_str(), reply.status);
            ix::WebSocketHttpHeaders headers;
            headers["Content-Type"] = "text/plain";
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
