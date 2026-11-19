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

TEST(PairingTracker, ASuccessAfterThePromptIsWithdrawnRecordsPaired) {
    PairingTracker tracker;
    tracker.started("S");
    // The library withdraws the prompt before it reports the success.
    tracker.ended();
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Abandoned);

    tracker.succeeded("S");
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Paired);
    EXPECT_EQ(tracker.last().server_id, "S");
}

TEST(PairingTracker, AFailureKeepsItsReason) {
    PairingTracker tracker;
    tracker.started("S");
    tracker.failed("S", "the pairing code did not match");
    tracker.ended();
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Failed);
    EXPECT_EQ(tracker.last().server_id, "S");
    EXPECT_EQ(tracker.last().reason, "the pairing code did not match");
}

TEST(PairingTracker, AnEndedAttemptIsAbandoned) {
    PairingTracker tracker;
    tracker.started("S");
    tracker.ended();
    EXPECT_EQ(tracker.last().attempt, 1u);
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Abandoned);
    EXPECT_EQ(tracker.last().server_id, "S");
}

TEST(PairingTracker, EndingWithNoAttemptRecordsNothing) {
    PairingTracker tracker;
    tracker.ended();
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

    tracker.succeeded("B");
    EXPECT_EQ(tracker.last().attempt, 2u);
    EXPECT_EQ(tracker.last().server_id, "B");
    EXPECT_EQ(tracker.last().outcome, PairingOutcome::Paired);
}

TEST(PairingTracker, AnOutcomeForAnotherServerIsItsOwnAttempt) {
    PairingTracker tracker;
    tracker.started("A");
    tracker.ended();
    tracker.failed("B", "x");
    EXPECT_EQ(tracker.last().attempt, 2u);
    EXPECT_EQ(tracker.last().server_id, "B");
}
