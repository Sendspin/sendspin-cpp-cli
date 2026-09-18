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

/// When a sink whose device died mid-stream should try to get it back, and when to give up.

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace sendspin_cli {

/// Delay before the first rescan after an escalation; also floors retry cycles across streams.
inline constexpr int64_t SINK_RESCAN_DELAY_MS = 2000;

/// Ceiling the doubling delay between retried rescans grows to.
inline constexpr int64_t SINK_RESCAN_MAX_DELAY_MS = 30000;

/// Rescan attempts allowed before the budget is refilled.
inline constexpr int SINK_RESCAN_ATTEMPTS = 5;

/// Decides when a sink reopens a dead device in place and when it rescans or reconnects.
/// Budget: one reopen, then up to SINK_RESCAN_ATTEMPTS rescans. Only a recovered rescan or a newly
/// configured stream refills it; a recovered reopen leaves the next outage the rescans alone.
/// Every method but pending() must be called under the lock that serialises the sink's stream.
class SinkRecovery {
public:
    /// Whether write() should reopen the device in place; true at most once per refill.
    /// @return true if the caller should reopen now and report to reopen_done().
    bool reopen_due();

    /// Records the reopen's outcome; a failure escalates to the rescan.
    /// Call exactly once per reopen_due() that returned true: a second call is not guarded.
    void reopen_done(bool recovered);

    /// Whether the main loop should make the second attempt (rescan or reconnect) now.
    /// @return true once the delay is up; again only after rescan_done(false).
    bool rescan_due(int64_t now_ms);

    /// Records the second attempt's outcome; a recovery refills the budget for the next outage.
    /// A report with no attempt outstanding does nothing.
    void rescan_done(bool recovered);

    /// Ends the outstanding attempt with nothing refilled and nothing more owed: a shutdown, or a
    /// one-shot backend's failure. A call with no attempt outstanding does nothing.
    void rescan_abandoned();

    /// Brings an owed rescan forward to the next tick, for a backend whose OS has told it the
    /// device list changed. Never arms one that is not owed, and the attempt it releases still
    /// counts against the budget, so a burst of notifications stays bounded.
    void rescan_soon();

    /// True while a rescan is still owed. The one method safe to call without the lock.
    bool pending() const;

    /// Records frames accepted and discarded with no device to play them; saturates.
    /// Stays pending through a recovered rescan until a timed write takes it.
    void discard_frames(uint32_t frames);

    /// Returns and clears the frames accumulated by discard_frames().
    uint32_t take_discarded_frames();

    /// Drops the discarded-frame count, leaving the recovery budget alone.
    void forget_discarded_frames();

    /// Refills the budget; call only when configure() really opened a stream.
    void reset();

private:
    void escalate_();
    /// Refills the attempt budget, leaving the discarded-frame count alone.
    void refill_();

    static constexpr int64_t NOT_STAMPED = INT64_MIN;

    /// Delay before the attempt after `attempts_made`, doubling up to SINK_RESCAN_MAX_DELAY_MS.
    static int64_t delay_for_(int attempts_made);

    bool reopen_spent_{false};
    /// Latches when the second attempt is retired.
    bool rescan_spent_{false};
    /// Set from handing out an attempt until rescan_done(); blocks re-arming and double counting.
    bool rescan_in_flight_{false};
    /// Set by rescan_soon(); makes the next rescan_due() skip the backoff exactly once.
    bool rescan_immediate_{false};
    int rescan_attempts_{0};
    /// Frames accepted with no device to play them, not yet retired; see discard_frames().
    uint32_t discarded_frames_{0};
    /// Read by the main loop without the sink's lock; see pending().
    std::atomic<bool> rescan_owed_{false};
    int64_t rescan_at_ms_{NOT_STAMPED};
};

/// @brief Carries an outage gap from a sink's locked write() to a callback that takes no lock.
///
/// PortAudioSink and PipeWireSink report playback from a realtime callback, which SinkRecovery's
/// locking rule puts out of its reach. So write() moves the gap in here, under the sink's lock,
/// once the stream is alive again, and the callback takes it with the first report that has a
/// timestamp to retire it against. Saturates rather than wraps, like discard_frames().
class OutageGapHandoff {
public:
    /// Producer side, under the sink's lock. Safe against a concurrent take().
    void add(uint32_t frames);

    /// Consumer side, from the audio callback: the whole gap, leaving none behind.
    uint32_t take();

    /// For a stream that ended before its gap was retired; see forget_discarded_frames().
    void forget();

private:
    std::atomic<uint32_t> frames_{0};
};

/// @brief The count for one on_frames_played() report that retires `gap_frames` alongside
/// `played_frames`.
///
/// Saturated rather than wrapped: the report is 32-bit and a gap can already sit at the ceiling.
constexpr uint32_t frames_with_gap(uint32_t gap_frames, uint64_t played_frames) {
    const uint64_t total = static_cast<uint64_t>(gap_frames) + played_frames;
    const uint64_t ceiling = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>((total < ceiling) ? total : ceiling);
}

}  // namespace sendspin_cli
