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

/// The pairing token endpoint: what it answers, and that it listens only where it was told.

#include "pairing_endpoint.h"

#include <gtest/gtest.h>
#include <ixwebsocket/IXCancellationRequest.h>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXSocket.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sendspin_cli {
namespace {

constexpr const char* TOKEN = "SP:EXAMPLETOKEN";

/// A request to 127.0.0.1:8930, with the Host header that names it.
PairingReply reply_to(const std::string& method, const std::string& uri,
                      const std::string& host = "127.0.0.1:8930") {
    return pairing_token_reply(method, uri, host, "127.0.0.1", 8930, TOKEN);
}

void expect_refused(const PairingReply& reply) {
    EXPECT_NE(reply.status, 200);
    EXPECT_EQ(reply.body.find(TOKEN), std::string::npos) << reply.body;
}

TEST(PairingEndpoint, SupportObjectCarriesThePort) {
    EXPECT_EQ(local_pairing_support_json(8930), "{\"port\":8930}");
}

TEST(PairingEndpoint, AnswersGetOnItsPathWithTheToken) {
    const PairingReply reply = reply_to("GET", "/");
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, TOKEN);
    EXPECT_EQ(reply_to("GET", "/?x=1").status, 200);
}

TEST(PairingEndpoint, RefusesOtherPathsAndMethodsWithoutTheToken) {
    expect_refused(reply_to("GET", "/pairing-token"));
    expect_refused(reply_to("GET", "//"));
    expect_refused(reply_to("POST", "/"));
}

TEST(PairingEndpoint, RefusesAHostThatDoesNotNameIt) {
    // A rebound page sends its own hostname; the address alone is only right on port 80.
    for (const char* host : {"evil.example:8930", "evil.example", "", "127.0.0.1:8931", "127.0.0.1",
                             "127.0.0.2:8930", "127.0.0.1:", "[::1]:8930", "127.0.0.1:+8930"}) {
        const PairingReply reply = reply_to("GET", "/", host);
        EXPECT_EQ(reply.status, 421) << host;
        expect_refused(reply);
    }
}

TEST(PairingEndpoint, MatchesHostLikeAnAddress) {
    // Compared as addresses, so any spelling of the same one names the endpoint.
    for (const char* host : {"[FD00::1]:8930", "[fd00:0::1]:8930", "[fd00:0:0:0:0:0:0:1]:8930"}) {
        EXPECT_EQ(pairing_token_reply("GET", "/", host, "fd00::1", 8930, TOKEN).status, 200)
            << host;
    }
    EXPECT_EQ(pairing_token_reply("GET", "/", "[fd00::1]", "fd00::1", 80, TOKEN).status, 200);
    EXPECT_EQ(pairing_token_reply("GET", "/", "127.0.0.1", "127.0.0.1", 80, TOKEN).status, 200);
    EXPECT_EQ(pairing_token_reply("GET", "/", "127.0.0.1:80", "127.0.0.1", 80, TOKEN).status, 200);
}

TEST(PairingEndpoint, BracketsOnlyAnIpv6Address) {
    EXPECT_EQ(bracketed("fd00::1"), "[fd00::1]");
    EXPECT_EQ(bracketed("172.30.32.1"), "172.30.32.1");
}

TEST(PairingEndpoint, ReadsARequestHead) {
    std::string method;
    std::string uri;
    std::string host;
    ASSERT_TRUE(parse_request_head(
        "GET /?x=1 HTTP/1.1\r\nhOsT:  127.0.0.1:8930 \r\nAccept: */*\r\n\r\n", method, uri, host));
    EXPECT_EQ(method, "GET");
    EXPECT_EQ(uri, "/?x=1");
    EXPECT_EQ(host, "127.0.0.1:8930");

    ASSERT_TRUE(parse_request_head("GET / HTTP/1.0\r\n\r\n", method, uri, host));
    EXPECT_EQ(host, "");
}

TEST(PairingEndpoint, RefusesAHeadThatIsNotARequest) {
    std::string method;
    std::string uri;
    std::string host;
    for (const char* head :
         {"GET /\r\n\r\n", "GET / HTTP/1.1 extra\r\n\r\n", "GET  / HTTP/1.1\r\n\r\n",
          "GET / FTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nno colon\r\n\r\n",
          "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", "no line ending"}) {
        EXPECT_FALSE(parse_request_head(head, method, uri, host)) << head;
    }
}

/// `text`/`prefix`, read as a subnet.
IpSubnet subnet_of(const std::string& text, int prefix) {
    IpSubnet subnet;
    EXPECT_TRUE(parse_ip(text, subnet.family, subnet.address)) << text;
    subnet.prefix = prefix;
    return subnet;
}

TEST(PairingEndpoint, KnowsWhichPeersShareASubnet) {
    // Home Assistant OS's internal network.
    const IpSubnet hassio = subnet_of("172.30.32.1", 23);
    EXPECT_TRUE(subnet_contains(hassio, "172.30.32.1"));
    EXPECT_TRUE(subnet_contains(hassio, "172.30.33.254"));
    EXPECT_FALSE(subnet_contains(hassio, "172.30.34.1"));
    EXPECT_FALSE(subnet_contains(hassio, "192.168.1.20"));

    const IpSubnet loopback = subnet_of("127.0.0.1", 8);
    EXPECT_TRUE(subnet_contains(loopback, "127.0.0.1"));
    EXPECT_TRUE(subnet_contains(loopback, "127.9.9.9"));
    EXPECT_FALSE(subnet_contains(loopback, "10.0.0.1"));

    const IpSubnet ula = subnet_of("fd00::1", 64);
    EXPECT_TRUE(subnet_contains(ula, "fd00::abcd"));
    EXPECT_FALSE(subnet_contains(ula, "fd00:0:0:1::1"));
    EXPECT_TRUE(subnet_contains(subnet_of("::1", 128), "::1"));

    // Another family, or not an address at all, is never inside.
    EXPECT_FALSE(subnet_contains(loopback, "::1"));
    EXPECT_FALSE(subnet_contains(ula, "127.0.0.1"));
    EXPECT_FALSE(subnet_contains(loopback, "localhost"));
    EXPECT_FALSE(subnet_contains(loopback, ""));
}

TEST(PairingEndpoint, FindsTheLoopbackSubnet) {
    IpSubnet subnet;
    std::string error;
    ASSERT_TRUE(local_subnet_of("127.0.0.1", subnet, error)) << error;
    EXPECT_EQ(subnet.prefix, 8);
    EXPECT_TRUE(subnet_contains(subnet, "127.0.0.1"));
    EXPECT_FALSE(subnet_contains(subnet, "10.0.0.1"));
}

TEST(PairingEndpoint, RefusesAnAddressNoInterfaceHolds) {
    // TEST-NET-1, reserved for documentation, so no machine running this test holds it.
    IpSubnet subnet;
    std::string error;
    EXPECT_FALSE(local_subnet_of("192.0.2.1", subnet, error));
    EXPECT_NE(error.find("192.0.2.1"), std::string::npos) << error;
}

/// Listens on the first free port from a fixed range; 0 when none was.
uint16_t listen_somewhere(std::unique_ptr<PairingEndpoint>& endpoint) {
    for (uint16_t port = 38930; port < 38990; ++port) {
        endpoint = std::make_unique<PairingEndpoint>("127.0.0.1", port);
        std::string error;
        if (endpoint->listen(error)) {
            return port;
        }
    }
    return 0;
}

TEST(PairingEndpoint, ServesTheTokenOverHttp) {
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    ix::HttpClient client;
    ix::HttpRequestArgsPtr args = client.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    const ix::HttpResponsePtr ok = client.get(base + PAIRING_TOKEN_PATH, args);
    ASSERT_EQ(ok->statusCode, 200) << ok->errorMsg;
    EXPECT_EQ(ok->body, TOKEN);
    EXPECT_EQ(ok->headers["Content-Type"], "text/plain");
    EXPECT_EQ(ok->headers["Cache-Control"], "no-store");

    const ix::HttpResponsePtr missing = client.get(base + "/pairing-token", args);
    EXPECT_EQ(missing->statusCode, 404);
    endpoint->stop();
}

TEST(PairingEndpoint, ListensOnlyOnItsAddress) {
#ifdef __APPLE__
    GTEST_SKIP() << "macOS configures only 127.0.0.1, so nothing could answer on 127.0.0.2 anyway";
#endif
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    // Another loopback address on the same port: bound to 127.0.0.1 only, so nothing answers.
    ix::HttpClient client;
    ix::HttpRequestArgsPtr args = client.createRequest();
    args->connectTimeout = 2;
    args->transferTimeout = 2;
    const ix::HttpResponsePtr other =
        client.get("http://127.0.0.2:" + std::to_string(port) + PAIRING_TOKEN_PATH, args);
    EXPECT_NE(other->statusCode, 200);
    endpoint->stop();
}

/// A connection to 127.0.0.1:port that has sent nothing; null when it could not connect.
std::unique_ptr<ix::Socket> open_connection(uint16_t port) {
    auto socket = std::make_unique<ix::Socket>();
    std::string error;
    std::atomic<bool> cancel{false};
    if (!socket->init(error) ||
        !socket->connect("127.0.0.1", port, error,
                         ix::makeCancellationRequestWithTimeout(2, cancel))) {
        return nullptr;
    }
    return socket;
}

/// Sends raw request bytes, for requests ix::HttpClient will not send. Returns the status line
/// and the body, or "" when the connection closed before answering.
std::string raw_send(uint16_t port, const std::string& request) {
    std::unique_ptr<ix::Socket> socket = open_connection(port);
    if (!socket) {
        return "";
    }
    std::atomic<bool> cancel{false};
    const auto deadline = ix::makeCancellationRequestWithTimeout(2, cancel);
    if (!socket->writeBytes(request, deadline)) {
        return "";
    }
    // The endpoint closes after its one response, so this reads to the end. The body has no line
    // ending: readLine hands it back with false when the connection closes.
    std::string response;
    for (;;) {
        const auto line = socket->readLine(deadline);
        response += line.second;
        if (!line.first) {
            return response;
        }
    }
}

/// A plain GET with the given Host header, sent raw.
std::string raw_exchange(uint16_t port, const std::string& host) {
    return raw_send(port, "GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
}

TEST(PairingEndpoint, RefusesARebindingHostOverHttp) {
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    const std::string response = raw_exchange(port, "evil.example:" + std::to_string(port));
    EXPECT_NE(response.find(" 421 "), std::string::npos) << response;
    EXPECT_EQ(response.find(TOKEN), std::string::npos) << response;
    endpoint->stop();
}

TEST(PairingEndpoint, ClosesAConnectionBeyondItsLimit) {
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);
    const std::string host = "127.0.0.1:" + std::to_string(port);

    // Stalled connections that never send a request. Accepted in order, so each is counted
    // before the next connection is admitted.
    std::vector<std::unique_ptr<ix::Socket>> stalled;
    for (int i = 0; i < PAIRING_ENDPOINT_MAX_CONNECTIONS; ++i) {
        stalled.push_back(open_connection(port));
        ASSERT_NE(stalled.back(), nullptr);
    }
    EXPECT_EQ(raw_exchange(port, host), "");

    // Once they go, a request is answered again, after their workers are reaped.
    stalled.clear();
    std::string response;
    for (int attempt = 0; attempt < 40 && response.find(TOKEN) == std::string::npos; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        response = raw_exchange(port, host);
    }
    EXPECT_NE(response.find(TOKEN), std::string::npos) << response;
    endpoint->stop();
}

TEST(PairingEndpoint, RefusesAnOversizedRequestHead) {
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    const std::string padding(PAIRING_REQUEST_HEAD_LIMIT, 'a');
    const std::string response = raw_send(
        port, "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nX: " + padding);
    EXPECT_NE(response.find(" 431 "), std::string::npos) << response.substr(0, 80);
    EXPECT_EQ(response.find(TOKEN), std::string::npos);
    endpoint->stop();
}

TEST(PairingEndpoint, NeverReadsARequestBody) {
    // A body it read would be buffered; it answers from the head and closes instead.
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    const auto started = std::chrono::steady_clock::now();
    const std::string response =
        raw_send(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                           "\r\nContent-Length: 2147483647\r\n\r\npartial body");
    EXPECT_NE(response.find(TOKEN), std::string::npos) << response;
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    endpoint->stop();
}

TEST(PairingEndpoint, StopDoesNotWaitOutAStalledRequest) {
    std::unique_ptr<PairingEndpoint> endpoint;
    const uint16_t port = listen_somewhere(endpoint);
    ASSERT_NE(port, 0U);
    endpoint->start(TOKEN);

    // Half a request line, then nothing: its worker waits on a 5 s read deadline.
    std::unique_ptr<ix::Socket> stalled = open_connection(port);
    ASSERT_NE(stalled, nullptr);
    std::atomic<bool> cancel{false};
    ASSERT_TRUE(stalled->writeBytes("GET / HT", ix::makeCancellationRequestWithTimeout(2, cancel)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto started = std::chrono::steady_clock::now();
    endpoint->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

}  // namespace
}  // namespace sendspin_cli
