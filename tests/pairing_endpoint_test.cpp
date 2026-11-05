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
#include <ixwebsocket/IXHttpClient.h>

#include <cstdint>
#include <memory>
#include <string>

namespace sendspin_cli {
namespace {

constexpr const char* TOKEN = "SP:EXAMPLETOKEN";

TEST(PairingEndpoint, SupportObjectCarriesThePort) {
    EXPECT_EQ(local_pairing_support_json(8930), "{\"port\":8930}");
}

TEST(PairingEndpoint, AnswersGetOnItsPathWithTheToken) {
    const PairingReply reply = pairing_token_reply("GET", "/", TOKEN);
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(reply.body, TOKEN);
    EXPECT_EQ(pairing_token_reply("GET", "/?x=1", TOKEN).status, 200);
}

TEST(PairingEndpoint, RefusesOtherPathsAndMethodsWithoutTheToken) {
    for (const PairingReply& reply :
         {pairing_token_reply("GET", "/pairing-token", TOKEN),
          pairing_token_reply("GET", "//", TOKEN), pairing_token_reply("POST", "/", TOKEN)}) {
        EXPECT_NE(reply.status, 200);
        EXPECT_EQ(reply.body.find(TOKEN), std::string::npos) << reply.body;
    }
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

}  // namespace
}  // namespace sendspin_cli
