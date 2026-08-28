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

/// @file discovery_test.cpp
/// @brief The decisions outbound mode makes: which server, what URL, and when to redial
///
/// All of it without a socket or an mDNS daemon: the browse result is a plain struct, and
/// RetryPacer is handed the clock rather than reading one.

#include "mdns.h"
#include "outbound.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

/// A resolved instance, with the fields a test does not care about already plausible.
DiscoveredServer make_server(std::string instance, std::string name,
                             std::vector<std::string> addresses) {
    DiscoveredServer server;
    server.instance = std::move(instance);
    server.name = std::move(name);
    server.path = "/sendspin";
    server.port = 8927;
    server.addresses = std::move(addresses);
    return server;
}

// ---------------------------------------------------------------------------
// Browse result to URL
// ---------------------------------------------------------------------------

TEST(DiscoveredServerUrl, BuildsFromAddressPortAndPath) {
    const DiscoveredServer server = make_server("srv-1", "Living room", {"192.168.1.10"});

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(server, url, error)) << error;
    EXPECT_EQ(url, "ws://192.168.1.10:8927/sendspin");
}

TEST(DiscoveredServerUrl, UsesTheAdvertisedPathVerbatim) {
    DiscoveredServer server = make_server("srv-1", "", {"192.168.1.10"});
    server.path = "/other";
    server.port = 9000;

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(server, url, error)) << error;
    EXPECT_EQ(url, "ws://192.168.1.10:9000/other");
}

TEST(DiscoveredServerUrl, RejectsAMissingPath) {
    DiscoveredServer server = make_server("srv-1", "", {"192.168.1.10"});
    server.path.clear();

    std::string url;
    std::string error;
    EXPECT_FALSE(discovered_server_url(server, url, error));
    EXPECT_NE(error.find("path"), std::string::npos) << error;
}

TEST(DiscoveredServerUrl, RejectsAPathWithoutALeadingSlash) {
    DiscoveredServer server = make_server("srv-1", "", {"192.168.1.10"});
    server.path = "sendspin";

    std::string url;
    std::string error;
    EXPECT_FALSE(discovered_server_url(server, url, error));
    EXPECT_NE(error.find("'/'"), std::string::npos) << error;
}

TEST(DiscoveredServerUrl, RejectsAZeroPort) {
    DiscoveredServer server = make_server("srv-1", "", {"192.168.1.10"});
    server.port = 0;

    std::string url;
    std::string error;
    EXPECT_FALSE(discovered_server_url(server, url, error));
    EXPECT_NE(error.find("port"), std::string::npos) << error;
}

TEST(DiscoveredServerUrl, RejectsAnInstanceWithNoAddressYet) {
    const DiscoveredServer server = make_server("srv-1", "", {});

    std::string url;
    std::string error;
    EXPECT_FALSE(discovered_server_url(server, url, error));
}

TEST(DiscoveredServerUrl, PrefersIPv4OverIPv6WhateverTheOrder) {
    const DiscoveredServer v6_first = make_server("srv-1", "", {"2001:db8::5", "192.168.1.10"});

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(v6_first, url, error)) << error;
    EXPECT_EQ(url, "ws://192.168.1.10:8927/sendspin");
}

TEST(DiscoveredServerUrl, BracketsAnIPv6OnlyInstance) {
    const DiscoveredServer server = make_server("srv-1", "", {"2001:db8::5"});

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(server, url, error)) << error;
    EXPECT_EQ(url, "ws://[2001:db8::5]:8927/sendspin");
}

TEST(DiscoveredServerUrl, SkipsLinkLocalAndUnspecifiedAddresses) {
    const DiscoveredServer server =
        make_server("srv-1", "", {"169.254.3.4", "fe80::1", "0.0.0.0", "::", "192.168.1.10"});

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(server, url, error)) << error;
    EXPECT_EQ(url, "ws://192.168.1.10:8927/sendspin");
}

TEST(DiscoveredServerUrl, RejectsAnInstanceWithOnlyUnusableAddresses) {
    const DiscoveredServer server = make_server("srv-1", "", {"169.254.3.4", "fe80::1"});

    std::string url;
    std::string error;
    EXPECT_FALSE(discovered_server_url(server, url, error));
    EXPECT_NE(error.find("routable"), std::string::npos) << error;
}

TEST(DiscoveredServerUrl, IgnoresSomethingThatIsNotAnAddressAtAll) {
    const DiscoveredServer server = make_server("srv-1", "", {"not-an-address", "192.168.1.10"});

    std::string url;
    std::string error;
    ASSERT_TRUE(discovered_server_url(server, url, error)) << error;
    EXPECT_EQ(url, "ws://192.168.1.10:8927/sendspin");
}

// ---------------------------------------------------------------------------
// Choosing among candidates
// ---------------------------------------------------------------------------

TEST(SelectServer, TakesTheFirstToResolveWhenNothingIsPreferred) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
        make_server("srv-2", "Study", {"192.168.1.11"}),
    };

    std::string reason;
    const DiscoveredServer* chosen = select_server(servers, "", "", reason);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->instance, "srv-1");
    EXPECT_NE(reason.find("first"), std::string::npos) << reason;
}

TEST(SelectServer, PrefersTheRememberedServerWhereverItIsInTheList) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
        make_server("srv-2", "Study", {"192.168.1.11"}),
    };

    std::string reason;
    const DiscoveredServer* chosen = select_server(servers, "", "srv-2", reason);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->instance, "srv-2");
    EXPECT_NE(reason.find("handshake"), std::string::npos) << reason;
}

TEST(SelectServer, FallsBackWhenTheRememberedServerIsNotAround) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
    };

    std::string reason;
    const DiscoveredServer* chosen = select_server(servers, "", "srv-9", reason);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->instance, "srv-1");
}

TEST(SelectServer, NameFilterIsAHardConstraint) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
        make_server("srv-2", "Study", {"192.168.1.11"}),
    };

    std::string reason;
    const DiscoveredServer* chosen = select_server(servers, "Study", "", reason);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->instance, "srv-2");
    EXPECT_NE(reason.find("Study"), std::string::npos) << reason;
}

TEST(SelectServer, TheNameFilterOutranksTheRememberedServer) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
        make_server("srv-2", "Study", {"192.168.1.11"}),
    };

    std::string reason;
    const DiscoveredServer* chosen = select_server(servers, "Study", "srv-1", reason);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->instance, "srv-2");
}

TEST(SelectServer, NothingMatchesTheNameFilter) {
    const std::vector<DiscoveredServer> servers = {
        make_server("srv-1", "Kitchen", {"192.168.1.10"}),
    };

    std::string reason;
    EXPECT_EQ(select_server(servers, "Bathroom", "", reason), nullptr);
}

TEST(SelectServer, NoCandidatesAtAll) {
    std::string reason;
    EXPECT_EQ(select_server({}, "", "", reason), nullptr);
}

// ---------------------------------------------------------------------------
// Name truncation
// ---------------------------------------------------------------------------

TEST(TruncateUtf8, LeavesSomethingThatFitsAlone) {
    EXPECT_EQ(truncate_utf8("living-room", MDNS_MAX_LABEL_BYTES), "living-room");
}

TEST(TruncateUtf8, CutsAtTheByteLimit) {
    EXPECT_EQ(truncate_utf8(std::string(80, 'a'), MDNS_MAX_LABEL_BYTES),
              std::string(MDNS_MAX_LABEL_BYTES, 'a'));
}

TEST(TruncateUtf8, NeverSplitsAMultiByteSequence) {
    // "é" is two bytes, so a 63-byte limit lands mid-sequence and must back up to 62.
    const std::string name = std::string(62, 'a') + "\xC3\xA9";
    const std::string cut = truncate_utf8(name, MDNS_MAX_LABEL_BYTES);
    EXPECT_EQ(cut, std::string(62, 'a'));
}

TEST(TruncateUtf8, KeepsAMultiByteSequenceThatEndsExactlyOnTheLimit) {
    const std::string name = std::string(61, 'a') + "\xC3\xA9";
    EXPECT_EQ(truncate_utf8(name, MDNS_MAX_LABEL_BYTES), name);
}

// ---------------------------------------------------------------------------
// The backoff schedule
// ---------------------------------------------------------------------------

TEST(NextRetryDelay, DoublesFromTheFloorAndSaturatesAtTheCap) {
    EXPECT_EQ(next_retry_delay_ms(0), 1000U);
    EXPECT_EQ(next_retry_delay_ms(1), 2000U);
    EXPECT_EQ(next_retry_delay_ms(2), 4000U);
    EXPECT_EQ(next_retry_delay_ms(3), 8000U);
    EXPECT_EQ(next_retry_delay_ms(4), 16000U);
    EXPECT_EQ(next_retry_delay_ms(5), MAX_RETRY_DELAY_MS);
    EXPECT_EQ(next_retry_delay_ms(6), MAX_RETRY_DELAY_MS);
    // Far past the cap, where a naive shift would have overflowed.
    EXPECT_EQ(next_retry_delay_ms(1000), MAX_RETRY_DELAY_MS);
}

// ---------------------------------------------------------------------------
// Pacing the redials
// ---------------------------------------------------------------------------

TEST(RetryPacer, DialsImmediatelyOnTheFirstTick) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);
    EXPECT_TRUE(pacer.should_dial(0));
}

TEST(RetryPacer, WaitsTheBackoffBeforeRedialling) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);
    pacer.note_dial(0);

    // The point of the whole class: a tick during an in-flight attempt must not redial,
    // because connect_to() would tear that attempt down.
    EXPECT_FALSE(pacer.should_dial(10));
    EXPECT_FALSE(pacer.should_dial(999));
    EXPECT_TRUE(pacer.should_dial(1000));
}

TEST(RetryPacer, EachFailedDialWidensTheGap) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);

    int64_t now = 0;
    for (const uint32_t expected : {1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U}) {
        ASSERT_TRUE(pacer.should_dial(now));
        pacer.note_dial(now);
        EXPECT_EQ(pacer.delay_ms(), expected);
        now += expected - 1;
        EXPECT_FALSE(pacer.should_dial(now)) << "redialled a millisecond early at " << expected;
        now += 1;
    }
}

TEST(RetryPacer, NeverDialsWhileConnected) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);
    pacer.note_dial(0);
    pacer.note_connection_state(true, 100);

    EXPECT_FALSE(pacer.should_dial(100));
    EXPECT_FALSE(pacer.should_dial(1'000'000));
}

TEST(RetryPacer, ACompletedHandshakeResetsTheSchedule) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);
    int64_t now = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        pacer.note_dial(now);
        now += pacer.delay_ms();
    }
    ASSERT_GT(pacer.delay_ms(), MIN_RETRY_DELAY_MS);

    pacer.note_connection_state(true, now);
    EXPECT_EQ(pacer.delay_ms(), MIN_RETRY_DELAY_MS);
}

TEST(RetryPacer, LosingAConnectionRestartsFromTheFloor) {
    RetryPacer pacer;
    pacer.note_connection_state(false, 0);
    pacer.note_dial(0);
    pacer.note_connection_state(true, 500);

    const int64_t lost_at = 900'000;
    EXPECT_TRUE(pacer.note_connection_state(false, lost_at));
    EXPECT_EQ(pacer.delay_ms(), MIN_RETRY_DELAY_MS);
    // Paced from the drop, not from the dial that established the link hours ago -- which
    // would otherwise make the redial instant.
    EXPECT_FALSE(pacer.should_dial(lost_at + 999));
    EXPECT_TRUE(pacer.should_dial(lost_at + 1000));
}

TEST(RetryPacer, TheLostTransitionIsReportedExactlyOnce) {
    RetryPacer pacer;
    pacer.note_connection_state(true, 0);
    EXPECT_TRUE(pacer.note_connection_state(false, 100));
    EXPECT_FALSE(pacer.note_connection_state(false, 200));
    EXPECT_FALSE(pacer.note_connection_state(true, 300));
}

// ---------------------------------------------------------------------------
// What the last dial may claim
// ---------------------------------------------------------------------------

TEST(LastDial, StartsWithNothingToExport) {
    const LastDial dial;

    EXPECT_EQ(dial.url_for("srv-1"), "");
}

TEST(LastDial, ALiteralUrlIsTakenAtItsWord) {
    // A -s URL promises nothing about who answers, so there is nothing to check the
    // connected server against: the URL is exported as dialled.
    LastDial dial;
    dial.note_dial("ws://hifi:8927/sendspin", "");

    EXPECT_EQ(dial.url_for("srv-1"), "ws://hifi:8927/sendspin");
    EXPECT_EQ(dial.url_for(""), "ws://hifi:8927/sendspin");
}

TEST(LastDial, ADiscoveryDialAnswersOnlyForTheServerItDialled) {
    LastDial dial;
    dial.note_dial("ws://192.168.1.10:8927/sendspin", "srv-1");

    EXPECT_EQ(dial.url_for("srv-1"), "ws://192.168.1.10:8927/sendspin");
    // A different server answered -- it dialled in, or beat the attempt -- and an unknown
    // one cannot be checked at all. Either way the URL would describe the wrong connection.
    EXPECT_EQ(dial.url_for("srv-2"), "");
    EXPECT_EQ(dial.url_for(""), "");
}

TEST(LastDial, ALostConnectionForgetsTheDial) {
    LastDial dial;
    dial.note_dial("ws://hifi:8927/sendspin", "");
    dial.note_lost();

    EXPECT_EQ(dial.url_for(""), "");
}

TEST(LastDial, ARedialAfterALossIsExportedAgain) {
    LastDial dial;
    dial.note_dial("ws://one:8927/sendspin", "srv-1");
    dial.note_lost();
    dial.note_dial("ws://two:8927/sendspin", "srv-2");

    EXPECT_EQ(dial.url_for("srv-2"), "ws://two:8927/sendspin");
    EXPECT_EQ(dial.url_for("srv-1"), "");
}

}  // namespace
}  // namespace sendspin_cli
