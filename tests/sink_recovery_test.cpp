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

/// @file sink_recovery_test.cpp
/// @brief What a sink whose device died mid-stream is allowed to try, and when it must stop
///
/// The decision half of PortAudioSink's recovery, which lives away from the sink precisely so it
/// can be tested here: src/portaudio_sink.cpp is only compiled where PortAudio is, and this
/// binary is built everywhere. Nothing below opens a device or reads a clock.

#include "sink_recovery.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace sendspin_cli {
namespace {

/// An arbitrary monotonic starting point. Nothing depends on its value -- only on the
/// differences -- which is the point of the class taking now_ms rather than reading a clock.
constexpr int64_t T0 = 1'000'000;

/// Drives a fresh recovery to the state a failed in-place reopen leaves it in: the inline
/// attempt spent, the rescan owed and not yet stamped with a deadline.
///
/// By reference rather than returned, because SinkRecovery holds an atomic and so is neither
/// copyable nor movable.
void escalate(SinkRecovery& recovery) {
    ASSERT_TRUE(recovery.reopen_due());
    recovery.reopen_done(false);
}

/// Ticks the main loop from `from_ms` until the rescan fires, or until `limit_ms` proves it will
/// not. Ticks are 10 ms, as the real loop's are.
/// @return The time the rescan fired at, or -1 if it never did.
int64_t rescan_fires_at(SinkRecovery& recovery, int64_t from_ms, int64_t limit_ms) {
    for (int64_t now = from_ms; now <= limit_ms; now += 10) {
        if (recovery.rescan_due(now)) {
            return now;
        }
    }
    return -1;
}

TEST(SinkRecovery, OffersNothingUntilAStreamDies) {
    SinkRecovery recovery;

    EXPECT_FALSE(recovery.pending());
    EXPECT_FALSE(recovery.rescan_due(T0));
    EXPECT_FALSE(recovery.rescan_due(T0 + 10 * SINK_RESCAN_DELAY_MS));
}

TEST(SinkRecovery, TriesTheInPlaceReopenFirstAndOnlyOnce) {
    SinkRecovery recovery;

    EXPECT_TRUE(recovery.reopen_due());
    // The reopen is what write() is doing right now, so nothing else is owed yet.
    EXPECT_FALSE(recovery.pending());
}

TEST(SinkRecovery, ASuccessfulReopenOwesNothingFurther) {
    SinkRecovery recovery;

    ASSERT_TRUE(recovery.reopen_due());
    recovery.reopen_done(true);

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, AFailedReopenEscalatesToTheRescan) {
    SinkRecovery recovery;
    escalate(recovery);

    EXPECT_TRUE(recovery.pending());
}

TEST(SinkRecovery, EveryFurtherWriteOfTheOutageIsToldToDiscard) {
    SinkRecovery recovery;
    escalate(recovery);

    // write() asks once per buffer -- around fifty times a second -- and every one of them after
    // the first must be told no, or a dead device would be reopened on each.
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(recovery.reopen_due()) << "write " << i;
    }
    // ...and asking has not re-armed anything.
    EXPECT_TRUE(recovery.pending());
}

TEST(SinkRecovery, TheRescanWaitsOutTheDelayBeforeItFires) {
    SinkRecovery recovery;
    escalate(recovery);

    // The deadline is stamped by the first tick after the escalation, not by the escalation
    // itself -- that happens on a thread with no business reading the loop's clock.
    EXPECT_FALSE(recovery.rescan_due(T0));
    EXPECT_FALSE(recovery.rescan_due(T0 + SINK_RESCAN_DELAY_MS - 10));
    EXPECT_TRUE(recovery.pending());

    EXPECT_TRUE(recovery.rescan_due(T0 + SINK_RESCAN_DELAY_MS));
}

TEST(SinkRecovery, TheRescanFiresOnceAndOnceOnly) {
    SinkRecovery recovery;
    escalate(recovery);

    const int64_t fired_at = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GE(fired_at, T0 + SINK_RESCAN_DELAY_MS);

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, fired_at + 10, fired_at + 10 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, GivesUpOnceBothAttemptsAreSpent) {
    SinkRecovery recovery;
    escalate(recovery);
    ASSERT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), T0);

    // A rescan that worked and one that did not leave the same nothing behind, which is why
    // there is no result to report back. Either way the sink discards until the next configure().
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, AStreamThatDiesAgainGoesStraightToTheRescan) {
    SinkRecovery recovery;

    // The device came back, and then immediately went away again -- half-present hardware, a
    // dock mid-handshake. Reopening a second time would be the same call against the same cached
    // device list, fifty times a second, so the budget must not have refilled.
    ASSERT_TRUE(recovery.reopen_due());
    recovery.reopen_done(true);

    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_TRUE(recovery.pending());
    EXPECT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, ResetPutsBothAttemptsBackInHand) {
    SinkRecovery recovery;
    escalate(recovery);
    ASSERT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), T0);
    ASSERT_FALSE(recovery.reopen_due());

    recovery.reset();

    EXPECT_FALSE(recovery.pending());
    EXPECT_TRUE(recovery.reopen_due());
    recovery.reopen_done(false);
    EXPECT_TRUE(recovery.pending());
    // And the delay is measured afresh, rather than from the outage reset() closed.
    EXPECT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, ResetDuringAnOutageCancelsWhatWasOwed) {
    SinkRecovery recovery;
    escalate(recovery);
    ASSERT_TRUE(recovery.pending());

    // A configure() that got a stream running answers the outage more completely than any
    // recovery could, so what was owed is no longer owed to anybody.
    recovery.reset();

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), -1);
}

}  // namespace
}  // namespace sendspin_cli
