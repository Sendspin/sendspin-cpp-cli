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

#include <ixwebsocket/IXCancellationRequest.h>
#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXHttp.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketServer.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// After winsock2.h, which it needs.
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

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

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

/// A port of 1-65535 written as digits only; strtoul alone accepts a sign and leading spaces.
bool parse_port(const std::string& text, uint16_t& port) {
    if (text.empty() || text.size() > 5 ||
        text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    const unsigned long number = std::strtoul(text.c_str(), nullptr, 10);
    if (number == 0 || number > 65535UL) {
        return false;
    }
    port = static_cast<uint16_t>(number);
    return true;
}

/// Whether the first `prefix` bits of `a` and `b` agree.
bool same_prefix(const std::array<unsigned char, 16>& a, const std::array<unsigned char, 16>& b,
                 int prefix) {
    const int whole = prefix / 8;
    if (std::memcmp(a.data(), b.data(), static_cast<size_t>(whole)) != 0) {
        return false;
    }
    const int bits = prefix % 8;
    if (bits == 0) {
        return true;
    }
    const auto mask = static_cast<unsigned char>(0xff << (8 - bits));
    return (a[whole] & mask) == (b[whole] & mask);
}

/// The address a sockaddr holds, for IPv4 and IPv6 only.
bool from_sockaddr(const sockaddr* address, IpSubnet& subnet) {
    if (address == nullptr) {
        return false;
    }
    if (address->sa_family == AF_INET) {
        subnet.family = AF_INET;
        std::memcpy(subnet.address.data(), &reinterpret_cast<const sockaddr_in*>(address)->sin_addr,
                    4);
        return true;
    }
    if (address->sa_family == AF_INET6) {
        subnet.family = AF_INET6;
        std::memcpy(subnet.address.data(),
                    &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr, 16);
        return true;
    }
    return false;
}

#ifndef _WIN32
/// The number of leading one bits in a netmask.
int mask_length(const sockaddr* mask, int family) {
    IpSubnet bits;
    if (!from_sockaddr(mask, bits) || bits.family != family) {
        return 0;
    }
    int length = 0;
    for (const unsigned char byte : bits.address) {
        for (int bit = 7; bit >= 0 && (byte & (1U << bit)) != 0; --bit) {
            ++length;
        }
        if (byte != 0xff) {
            break;
        }
    }
    return length;
}
#endif

/// Whether a Host header names `address` and `port`, compared as addresses rather than as text.
bool host_names_endpoint(const std::string& host, const std::string& address, uint16_t port) {
    std::string host_address;
    std::string port_text;
    if (!host.empty() && host.front() == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host_address = host.substr(1, close - 1);
        const std::string rest = host.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return false;
            }
            port_text = rest.substr(1);
            if (port_text.empty()) {
                return false;
            }
        }
    } else {
        const size_t colon = host.find(':');
        host_address = host.substr(0, colon);
        if (colon != std::string::npos) {
            port_text = host.substr(colon + 1);
            if (port_text.empty()) {
                return false;
            }
        }
    }

    // A client leaves the port out of Host only when it is the scheme's default.
    uint16_t host_port = 80;
    if (!port_text.empty() && !parse_port(port_text, host_port)) {
        return false;
    }
    int host_family = 0;
    int family = 0;
    std::array<unsigned char, 16> host_bytes{};
    std::array<unsigned char, 16> bytes{};
    return parse_ip(host_address, host_family, host_bytes) && parse_ip(address, family, bytes) &&
           host_family == family && host_bytes == bytes && host_port == port;
}

}  // namespace

/// One request per connection, each on its own worker thread.
///
/// Not ix::HttpServer: that counts only upgraded WebSocket clients against its connection limit,
/// so plain HTTP requests would each get a thread with no cap. This counts every connection.
class PairingServer final : public ix::SocketServer {
public:
    using Handler =
        std::function<PairingReply(const std::string& method, const std::string& uri,
                                   const std::string& host, const std::string& remote_ip)>;

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

    /// Cancels requests still being read, so the join below does not wait out their deadline.
    void stop() override {
        this->cancel_ = true;
        ix::SocketServer::stop();
    }

private:
    /// Counts itself in `live` from its accept until its worker thread is reaped.
    class CountedConnection : public ix::ConnectionState {
    public:
        explicit CountedConnection(std::shared_ptr<std::atomic<size_t>> live)
            : live_(std::move(live)) {
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

    void handleConnection(std::unique_ptr<ix::Socket> socket,
                          std::shared_ptr<ix::ConnectionState> state) override {
        // Byte by byte to the blank line, so neither a long head nor a body is ever buffered.
        const ix::CancellationRequest deadline =
            ix::makeCancellationRequestWithTimeout(TIMEOUT_SECS, this->cancel_);
        std::string head;
        bool complete = false;
        char byte = 0;
        while (head.size() < PAIRING_REQUEST_HEAD_LIMIT && socket->readByte(&byte, deadline)) {
            head.push_back(byte);
            if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) {
                complete = true;
                break;
            }
        }

        std::optional<PairingReply> reply;
        std::string method;
        std::string uri;
        std::string host;
        if (complete && parse_request_head(head, method, uri, host)) {
            reply = this->handler_(method, uri, host, state->getRemoteIp());
        } else if (complete) {
            reply = PairingReply{400, "bad request"};
        } else if (head.size() >= PAIRING_REQUEST_HEAD_LIMIT) {
            reply = PairingReply{431, "request header fields too large"};
        }
        if (reply.has_value()) {
            ix::WebSocketHttpHeaders headers;
            headers["Content-Type"] = "text/plain";
            headers["Cache-Control"] = "no-store";
            ix::Http::sendResponse(std::make_shared<ix::HttpResponse>(
                                       reply->status, reply->status == 200 ? "OK" : "Error",
                                       ix::HttpErrorCode::Ok, headers, reply->body),
                                   socket);
        }
        state->setTerminated();
    }

    size_t getConnectedClientsCount() override {
        return *this->live_;
    }

    std::shared_ptr<std::atomic<size_t>> live_ = std::make_shared<std::atomic<size_t>>(0);
    std::atomic<bool> cancel_{false};
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

    uint16_t number = 0;
    if (!parse_port(port_text, number)) {
        error = "expected a port of 1-65535";
        return false;
    }

    host = address;
    port = number;
    return true;
}

std::string bracketed(const std::string& address) {
    return address.find(':') != std::string::npos ? "[" + address + "]" : address;
}

std::string local_pairing_support_json(uint16_t port) {
    return "{\"port\":" + std::to_string(port) + "}";
}

bool parse_ip(const std::string& text, int& family, std::array<unsigned char, 16>& address) {
    address = {};
    if (inet_pton(AF_INET, text.c_str(), address.data()) == 1) {
        family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, text.c_str(), address.data()) == 1) {
        family = AF_INET6;
        return true;
    }
    return false;
}

bool subnet_contains(const IpSubnet& subnet, const std::string& ip) {
    int family = 0;
    std::array<unsigned char, 16> address{};
    return parse_ip(ip, family, address) && family == subnet.family &&
           same_prefix(address, subnet.address, subnet.prefix);
}

bool local_subnet_of(const std::string& address, IpSubnet& subnet, std::string& error) {
    int family = 0;
    std::array<unsigned char, 16> bytes{};
    if (!parse_ip(address, family, bytes)) {
        error = "'" + address + "' is not a numeric address";
        return false;
    }

    // Containment rather than equality: Linux answers on all of 127.0.0.0/8, listing only .1.
    std::optional<IpSubnet> best;
    const auto consider = [&](const IpSubnet& candidate) {
        if (candidate.family == family && same_prefix(bytes, candidate.address, candidate.prefix) &&
            (!best.has_value() || candidate.prefix > best->prefix)) {
            best = candidate;
        }
    };

#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) {
        error = "could not list this machine's network interfaces";
        return false;
    }
    for (const auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        for (const auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            IpSubnet candidate;
            if (from_sockaddr(unicast->Address.lpSockaddr, candidate)) {
                candidate.prefix = unicast->OnLinkPrefixLength;
                consider(candidate);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        error = "could not list this machine's network interfaces";
        return false;
    }
    for (const ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        IpSubnet candidate;
        if (entry->ifa_netmask != nullptr && from_sockaddr(entry->ifa_addr, candidate)) {
            candidate.prefix = mask_length(entry->ifa_netmask, candidate.family);
            consider(candidate);
        }
    }
    freeifaddrs(interfaces);
#endif

    if (!best.has_value()) {
        error = "no network interface on this machine holds " + address;
        return false;
    }
    subnet = *best;
    return true;
}

bool parse_request_head(const std::string& head, std::string& method, std::string& uri,
                        std::string& host) {
    const size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }
    const std::string request_line = head.substr(0, line_end);
    const size_t first = request_line.find(' ');
    const size_t second =
        first == std::string::npos ? std::string::npos : request_line.find(' ', first + 1);
    if (second == std::string::npos || request_line.find(' ', second + 1) != std::string::npos) {
        return false;
    }
    method = request_line.substr(0, first);
    uri = request_line.substr(first + 1, second - first - 1);
    if (method.empty() || uri.empty() || request_line.compare(second + 1, 5, "HTTP/") != 0) {
        return false;
    }

    host.clear();
    bool seen_host = false;
    for (size_t pos = line_end + 2; pos < head.size();) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) {
            end = head.size();
        }
        if (end == pos) {
            break;
        }
        const std::string line = head.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        if (equals_ignoring_case(line.substr(0, colon), "Host")) {
            // Two Host headers make the request ambiguous.
            if (seen_host) {
                return false;
            }
            seen_host = true;
            host = trimmed(line.substr(colon + 1));
        }
        pos = end + 2;
    }
    return true;
}

PairingReply pairing_token_reply(const std::string& method, const std::string& uri,
                                 const std::string& host, const std::string& address, uint16_t port,
                                 const std::string& token) {
    if (!host_names_endpoint(host, address, port)) {
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
    if (!local_subnet_of(this->host_, this->subnet_, error)) {
        return false;
    }
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
    this->server_->set_handler(
        [token, address = this->host_, port = this->port_, subnet = this->subnet_](
            const std::string& method, const std::string& uri, const std::string& host,
            const std::string& remote_ip) {
            // A peer off the address's own subnet came in through a route, from somewhere else.
            const PairingReply reply =
                subnet_contains(subnet, remote_ip)
                    ? pairing_token_reply(method, uri, host, address, port, token)
                    : PairingReply{403, "forbidden"};
            // The URI and Host are the caller's text, so they stay out of the log.
            log_line(sendspin::LogLevel::DEBUG, LOG_TAG,
                     "Pairing token endpoint answered %s with %d", remote_ip.c_str(), reply.status);
            return reply;
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
