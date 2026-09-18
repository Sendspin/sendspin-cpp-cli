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

/// SinkRecovery: what a sink whose device died mid-stream may try, and when it must stop.

#include "sink_recovery.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace sendspin_cli {
namespace {

/// An arbitrary monotonic start; only differences matter.
constexpr int64_t T0 = 1'000'000;

/// Drives a fresh recovery to where a failed in-place reopen leaves it: rescan owed, unstamped.
void escalate(SinkRecovery& recovery) {
    ASSERT_TRUE(recovery.reopen_due());
    recovery.reopen_done(false);
}

/// Ticks every 10 ms from `from_ms` until the rescan fires or `limit_ms` passes.
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

TEST(SinkRecovery, ReturnsTheDiscardedGapOnceWhenADeviceComesBack) {
    SinkRecovery recovery;
    escalate(recovery);

    // Fourteen seconds at 48 kHz, accepted while no DAC played them.
    recovery.discard_frames(14U * 48'000U);

    EXPECT_EQ(recovery.take_discarded_frames(), 672'000U);
    EXPECT_EQ(recovery.take_discarded_frames(), 0U);
}

TEST(SinkRecovery, DiscardedGapSaturatesInsteadOfWrapping) {
    SinkRecovery recovery;
    recovery.discard_frames(std::numeric_limits<uint32_t>::max() - 10U);
    recovery.discard_frames(100U);

    EXPECT_EQ(recovery.take_discarded_frames(), std::numeric_limits<uint32_t>::max());
}

TEST(SinkRecovery, TheDiscardedGapOutlivesARecoveredRescanUntilTaken) {
    SinkRecovery recovery;
    escalate(recovery);
    recovery.discard_frames(48'000U);
    ASSERT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), T0);

    // The reopen has no device timestamp to retire the gap against; the first timed write does.
    recovery.rescan_done(true);

    EXPECT_EQ(recovery.take_discarded_frames(), 48'000U);
    EXPECT_EQ(recovery.take_discarded_frames(), 0U);
}

TEST(SinkRecovery, ForgettingTheDiscardedGapLeavesTheBudgetAlone) {
    SinkRecovery recovery;
    escalate(recovery);
    recovery.discard_frames(48'000U);

    // What a flush does with no device open: the outage is still owed what it was.
    recovery.forget_discarded_frames();

    EXPECT_EQ(recovery.take_discarded_frames(), 0U);
    EXPECT_TRUE(recovery.pending());
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, ForgettingTheDiscardedGapDoesNotRefillASpentBudget) {
    SinkRecovery recovery;
    escalate(recovery);
    int64_t now = T0;
    for (int attempt = 0; attempt < SINK_RESCAN_ATTEMPTS; ++attempt) {
        now = rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS);
        ASSERT_GT(now, 0) << "attempt " << attempt << " never fired";
        recovery.rescan_done(false);
    }
    recovery.discard_frames(48'000U);

    recovery.forget_discarded_frames();

    EXPECT_FALSE(recovery.pending());
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_EQ(rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, ResetClearsTheDiscardedGap) {
    SinkRecovery recovery;
    escalate(recovery);
    recovery.discard_frames(48'000U);

    recovery.reset();

    EXPECT_EQ(recovery.take_discarded_frames(), 0U);
}

TEST(OutageGapHandoff, HandsTheGapToTheCallbackOnce) {
    OutageGapHandoff handoff;
    handoff.add(48'000U);
    handoff.add(1'000U);

    // A second take() would be a second report of the same outage, which is the playhead jump
    // this exists to prevent in the other direction.
    EXPECT_EQ(handoff.take(), 49'000U);
    EXPECT_EQ(handoff.take(), 0U);
}

TEST(OutageGapHandoff, SaturatesInsteadOfWrapping) {
    OutageGapHandoff handoff;
    handoff.add(std::numeric_limits<uint32_t>::max() - 10U);
    handoff.add(100U);

    EXPECT_EQ(handoff.take(), std::numeric_limits<uint32_t>::max());
}

TEST(OutageGapHandoff, ForgettingDropsAGapTheCallbackHasNotTaken) {
    OutageGapHandoff handoff;
    handoff.add(48'000U);

    // What clear() and configure() do: the player starts the next stream from zero, so a gap
    // still waiting for the callback is owed to nobody.
    handoff.forget();
    EXPECT_EQ(handoff.take(), 0U);

    // And the next outage is counted from nothing rather than refused.
    handoff.add(480U);
    EXPECT_EQ(handoff.take(), 480U);
}

TEST(OutageGapHandoff, TheReportCountSaturatesInsteadOfWrapping) {
    EXPECT_EQ(frames_with_gap(0U, 480U), 480U);
    EXPECT_EQ(frames_with_gap(672'000U, 480U), 672'480U);
    EXPECT_EQ(frames_with_gap(std::numeric_limits<uint32_t>::max() - 5U, 480U),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(frames_with_gap(1U, std::numeric_limits<uint64_t>::max() - 1U),
              std::numeric_limits<uint32_t>::max());
}

TEST(SinkRecovery, EveryFurtherWriteOfTheOutageIsToldToDiscard) {
    SinkRecovery recovery;
    escalate(recovery);

    // write() asks per buffer; every ask after the first must be told no.
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(recovery.reopen_due()) << "write " << i;
    }
    // ...and asking has not re-armed anything.
    EXPECT_TRUE(recovery.pending());
}

TEST(SinkRecovery, TheRescanWaitsOutTheDelayBeforeItFires) {
    SinkRecovery recovery;
    escalate(recovery);

    // The first tick after escalation stamps the deadline.
    EXPECT_FALSE(recovery.rescan_due(T0));
    EXPECT_FALSE(recovery.rescan_due(T0 + SINK_RESCAN_DELAY_MS - 10));
    EXPECT_TRUE(recovery.pending());

    EXPECT_TRUE(recovery.rescan_due(T0 + SINK_RESCAN_DELAY_MS));
}

TEST(SinkRecovery, TheRescanDoesNotFireAgainWhileNothingHasReportedBack) {
    SinkRecovery recovery;
    escalate(recovery);

    const int64_t fired_at = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GE(fired_at, T0 + SINK_RESCAN_DELAY_MS);

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, fired_at + 10, fired_at + 10 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, ACallerThatReportsNothingGetsExactlyOneRescan) {
    SinkRecovery recovery;
    escalate(recovery);
    ASSERT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), T0);

    // A caller that never reports gets exactly one rescan.
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, ASuccessfulRescanIsNotRetried) {
    SinkRecovery recovery;
    escalate(recovery);
    const int64_t fired_at = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GE(fired_at, T0);

    recovery.rescan_done(true);

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, fired_at, fired_at + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, ASecondOutageInTheSameStreamRecovers) {
    SinkRecovery recovery;
    escalate(recovery);
    int64_t now = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GT(now, 0);
    recovery.rescan_done(false);
    now = rescan_fires_at(recovery, now, now + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GT(now, 0);
    recovery.rescan_done(true);

    // The device died again: the reopen is back in hand, and so is a fresh ladder.
    escalate(recovery);
    EXPECT_TRUE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, now, now + 10 * SINK_RESCAN_DELAY_MS),
              now + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, EveryOutageAfterARecoveryStillGivesUp) {
    SinkRecovery recovery;
    escalate(recovery);
    int64_t now = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GT(now, 0);
    recovery.rescan_done(true);

    escalate(recovery);
    for (int attempt = 0; attempt < SINK_RESCAN_ATTEMPTS; ++attempt) {
        now = rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS);
        ASSERT_GT(now, 0) << "attempt " << attempt << " never fired";
        recovery.rescan_done(false);
    }

    EXPECT_FALSE(recovery.pending());
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_EQ(rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, TheDiscardedGapSurvivesTheRefillIntoTheNextOutage) {
    SinkRecovery recovery;
    escalate(recovery);
    recovery.discard_frames(48'000U);
    ASSERT_GE(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), T0);
    recovery.rescan_done(true);

    // Died again before a timed write took the first gap: both are still owed.
    escalate(recovery);
    recovery.discard_frames(1'000U);

    EXPECT_EQ(recovery.take_discarded_frames(), 49'000U);
}

TEST(SinkRecovery, AnAbandonedRescanRefillsNothing) {
    SinkRecovery recovery;
    escalate(recovery);
    const int64_t fired_at = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GT(fired_at, 0);

    recovery.rescan_abandoned();

    EXPECT_FALSE(recovery.pending());
    EXPECT_FALSE(recovery.reopen_due());
    EXPECT_EQ(rescan_fires_at(recovery, fired_at, fired_at + 100 * SINK_RESCAN_DELAY_MS), -1);

    // A late recovered report has no attempt to answer, so it cannot refill either.
    recovery.rescan_done(true);
    EXPECT_FALSE(recovery.reopen_due());
}

TEST(SinkRecovery, AbandoningWithNothingOutstandingDoesNothing) {
    SinkRecovery recovery;
    escalate(recovery);

    recovery.rescan_abandoned();

    EXPECT_TRUE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, AFailedRescanIsTriedAgainAfterALongerDelay) {
    SinkRecovery recovery;
    escalate(recovery);

    const int64_t first = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_EQ(first, T0 + SINK_RESCAN_DELAY_MS);

    recovery.rescan_done(false);
    EXPECT_TRUE(recovery.pending());

    // Twice the first delay. Measured by firing time: an extra early ask would move the deadline.
    const int64_t second = rescan_fires_at(recovery, first, first + 10 * SINK_RESCAN_DELAY_MS);
    EXPECT_EQ(second, first + (2 * SINK_RESCAN_DELAY_MS));
}

TEST(SinkRecovery, TheRetriesRunOutAndTheDelayStopsGrowing) {
    SinkRecovery recovery;
    escalate(recovery);

    int64_t now = T0;
    int64_t previous_delay = 0;
    for (int attempt = 1; attempt <= SINK_RESCAN_ATTEMPTS; ++attempt) {
        const int64_t fired_at = rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS);
        ASSERT_GT(fired_at, 0) << "attempt " << attempt << " never fired";

        const int64_t delay = fired_at - now;
        EXPECT_GE(delay, previous_delay) << "attempt " << attempt << " came sooner than the last";
        EXPECT_LE(delay, SINK_RESCAN_MAX_DELAY_MS)
            << "attempt " << attempt << " waited past the ceiling";
        if (attempt == SINK_RESCAN_ATTEMPTS) {
            // Pinned, not just bounded, so a delay that never grew cannot pass.
            EXPECT_EQ(delay, SINK_RESCAN_MAX_DELAY_MS);
        }
        previous_delay = delay;

        recovery.rescan_done(false);
        now = fired_at;
    }

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS), -1);
}

TEST(SinkRecovery, ReportingOnAnAttemptThatIsNotOutstandingDoesNothing) {
    SinkRecovery recovery;
    escalate(recovery);
    const int64_t fired_at = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_GT(fired_at, 0);

    recovery.rescan_done(false);
    ASSERT_TRUE(recovery.pending());

    // A duplicate report must not buy a second retry.
    recovery.rescan_done(false);
    EXPECT_EQ(rescan_fires_at(recovery, fired_at, fired_at + 10 * SINK_RESCAN_DELAY_MS),
              fired_at + (2 * SINK_RESCAN_DELAY_MS));

    // A report after reset() must not re-arm recovery.
    recovery.reset();
    recovery.rescan_done(false);
    EXPECT_FALSE(recovery.pending());
}

TEST(SinkRecovery, ResetPutsTheWholeRetryBudgetBack) {
    SinkRecovery recovery;
    escalate(recovery);
    int64_t now = T0;
    for (int attempt = 0; attempt < SINK_RESCAN_ATTEMPTS; ++attempt) {
        now = rescan_fires_at(recovery, now, now + 100 * SINK_RESCAN_DELAY_MS);
        ASSERT_GT(now, 0) << "attempt " << attempt << " never fired";
        recovery.rescan_done(false);
    }
    ASSERT_FALSE(recovery.pending());

    // reset() also returns the delay to SINK_RESCAN_DELAY_MS.
    recovery.reset();
    escalate(recovery);

    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
}

TEST(SinkRecovery, AStreamThatDiesAgainGoesStraightToTheRescan) {
    SinkRecovery recovery;

    // Came back and died again: the reopen budget must not have refilled.
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

    recovery.reset();

    EXPECT_FALSE(recovery.pending());
    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS), -1);
}

}  // namespace
}  // namespace sendspin_cli
