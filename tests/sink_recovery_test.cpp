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
/// The decision half of every device-backed sink's recovery, which lives away from the sinks
/// precisely so it can be tested here: src/portaudio_sink.cpp, src/pulse_sink.cpp and
/// src/pipewire_sink.cpp are each compiled only where their library is, and this binary is built
/// everywhere. Nothing below opens a device or reads a clock.

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

    // PortAudioSink's contract, and the reason the reporting call could be added without
    // touching it: a rebuilt device list leaves the same nothing behind whatever it found, so
    // that backend reports rescan_done(true) and nothing re-arms. Asserted with no report at all
    // as well, because a caller that forgets must degrade to this rather than to a retry loop.
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

TEST(SinkRecovery, AFailedRescanIsTriedAgainAfterALongerDelay) {
    SinkRecovery recovery;
    escalate(recovery);

    // What a restarting sound server needs, and what a device-list rebuild does not: the server
    // was still down when the first attempt landed, and nothing about that says the next one
    // will fail too.
    const int64_t first = rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS);
    ASSERT_EQ(first, T0 + SINK_RESCAN_DELAY_MS);

    recovery.rescan_done(false);
    EXPECT_TRUE(recovery.pending());

    // Twice the first delay, not the same one: the rate has to fall as an outage lengthens, or a
    // server that never comes back costs the main loop a fixed share of every second. Asserted as
    // the firing time rather than by asking early, because the first ask after a failure is what
    // stamps the deadline -- an extra one would move it.
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
            // Pinned rather than merely bounded above: without this the whole loop would still
            // pass for a delay_for_() that never grew at all, or one that clamped to the wrong
            // figure. The last attempt is the one that must have reached the ceiling.
            EXPECT_EQ(delay, SINK_RESCAN_MAX_DELAY_MS);
        }
        previous_delay = delay;

        recovery.rescan_done(false);
        now = fired_at;
    }

    // The cap, and it is the code's rather than a promise's: an outage that outlived every
    // attempt was not going to be answered by asking the same question again.
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

    // A second report for the same attempt must not buy a second retry, or a sink with two exit
    // paths that both report would burn its budget at twice the rate its delays assume.
    recovery.rescan_done(false);
    EXPECT_EQ(rescan_fires_at(recovery, fired_at, fired_at + 10 * SINK_RESCAN_DELAY_MS),
              fired_at + (2 * SINK_RESCAN_DELAY_MS));

    // And a report that arrives after a configure() has already answered the outage is ignored
    // rather than re-arming a recovery against a stream that is playing.
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

    // A configure() that got a stream running answers the outage more completely than any number
    // of reconnects could, so the count goes back to zero along with everything else -- including
    // the delay, which starts again at SINK_RESCAN_DELAY_MS rather than at the ceiling.
    recovery.reset();
    escalate(recovery);

    EXPECT_EQ(rescan_fires_at(recovery, T0, T0 + 10 * SINK_RESCAN_DELAY_MS),
              T0 + SINK_RESCAN_DELAY_MS);
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
