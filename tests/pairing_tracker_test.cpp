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

/// PairingTracker: how the library's callback order maps onto attempts and their outcomes.

#include "pairing_tracker.h"

#include <gtest/gtest.h>

using sendspin_cli::PairingOutcome;
using sendspin_cli::PairingTracker;

TEST(PairingTracker, ASuccessAfterTheCodeIsWithdrawnRecordsPaired) {
    PairingTracker tracker;
    tracker.started("S");
    tracker.code_shown("123456");
    // The library clears the code before it reports the success.
    tracker.code_cleared();
    EXPECT_EQ(tracker.in_progress_server_id(), "S");
    EXPECT_EQ(tracker.code(), "");
    EXPECT_EQ(tracker.last().attempt, 0u);

    tracker.succeeded("S");
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Paired);
    EXPECT_EQ(tracker.last().server_id, "S");
    EXPECT_EQ(tracker.in_progress_server_id(), "");
}

TEST(PairingTracker, AFailureKeepsItsReason) {
    PairingTracker tracker;
    tracker.started("S");
    tracker.failed("S", "the pairing code did not match");
    tracker.code_cleared();
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Failed);
    EXPECT_EQ(tracker.last().server_id, "S");
    EXPECT_EQ(tracker.last().reason, "the pairing code did not match");
    EXPECT_EQ(tracker.in_progress_server_id(), "");
}

TEST(PairingTracker, ADroppedConnectionAbandonsAnOpenAttempt) {
    PairingTracker tracker;
    tracker.started("S");
    tracker.code_shown("123456");
    tracker.code_cleared();
    tracker.connection_ended();
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Abandoned);
    EXPECT_EQ(tracker.last().server_id, "S");
    EXPECT_EQ(tracker.code(), "");
    EXPECT_EQ(tracker.in_progress_server_id(), "");
}

TEST(PairingTracker, ConnectionEndingWithNoAttemptRecordsNothing) {
    PairingTracker tracker;
    tracker.connection_ended();
    EXPECT_EQ(tracker.last().attempt, 0u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::None);
}

TEST(PairingTracker, ANewAttemptAbandonsTheOpenOne) {
    PairingTracker tracker;
    tracker.started("A");
    tracker.started("B");
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().server_id, "A");
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Abandoned);
    EXPECT_EQ(tracker.in_progress_server_id(), "B");

    tracker.succeeded("B");
    EXPECT_EQ(tracker.last().attempt, 2u);
    EXPECT_EQ(tracker.last().server_id, "B");
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Paired);
}

TEST(PairingTracker, TheWindowFlagFollowsOpenAndClose) {
    PairingTracker tracker;
    EXPECT_FALSE(tracker.window_open());
    tracker.window_opened();
    EXPECT_TRUE(tracker.window_open());
    tracker.window_closed();
    EXPECT_FALSE(tracker.window_open());
}
