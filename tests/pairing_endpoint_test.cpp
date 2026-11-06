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
                             "127.0.0.2:8930"}) {
        const PairingReply reply = reply_to("GET", "/", host);
        EXPECT_EQ(reply.status, 421) << host;
        expect_refused(reply);
    }
}

TEST(PairingEndpoint, MatchesHostLikeAnAddress) {
    EXPECT_EQ(pairing_token_reply("GET", "/", "[FD00::1]:8930", "[fd00::1]", 8930, TOKEN).status,
              200);
    EXPECT_EQ(pairing_token_reply("GET", "/", "127.0.0.1", "127.0.0.1", 80, TOKEN).status, 200);
    EXPECT_EQ(pairing_token_reply("GET", "/", "127.0.0.1:80", "127.0.0.1", 80, TOKEN).status, 200);
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

/// Sends a request on a raw connection, for headers ix::HttpClient will not send. Returns the
/// status line and the body, or "" when the connection closed before answering.
std::string raw_exchange(uint16_t port, const std::string& host) {
    std::unique_ptr<ix::Socket> socket = open_connection(port);
    if (!socket) {
        return "";
    }
    std::atomic<bool> cancel{false};
    const auto deadline = ix::makeCancellationRequestWithTimeout(2, cancel);
    if (!socket->writeBytes("GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n", deadline)) {
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

}  // namespace
}  // namespace sendspin_cli
