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

namespace sendspin_cli {

/// Delay before the first rescan after an escalation; also floors retry cycles across streams.
inline constexpr int64_t SINK_RESCAN_DELAY_MS = 2000;

/// Ceiling the doubling delay between retried rescans grows to.
inline constexpr int64_t SINK_RESCAN_MAX_DELAY_MS = 30000;

/// Rescan attempts allowed per configured stream.
inline constexpr int SINK_RESCAN_ATTEMPTS = 5;

/// Decides when a sink reopens a dead device in place and when it rescans or reconnects.
/// Budget is per configured stream: one reopen, then up to SINK_RESCAN_ATTEMPTS rescans.
/// Every method but pending() must be called under the lock that serialises the sink's stream.
class SinkRecovery {
public:
    /// Whether write() should reopen the device in place; true at most once per stream.
    /// @return true if the caller should reopen now and report to reopen_done().
    bool reopen_due();

    /// Records the reopen's outcome; a failure escalates to the rescan.
    /// Call exactly once per reopen_due() that returned true: a second call is not guarded.
    void reopen_done(bool recovered);

    /// Whether the main loop should make the second attempt (rescan or reconnect) now.
    /// @return true once the delay is up; again only after rescan_done(false).
    bool rescan_due(int64_t now_ms);

    /// Records the second attempt's outcome; a one-shot backend reports true regardless.
    /// A report with no attempt outstanding does nothing.
    void rescan_done(bool recovered);

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

    static constexpr int64_t NOT_STAMPED = INT64_MIN;

    /// Delay before the attempt after `attempts_made`, doubling up to SINK_RESCAN_MAX_DELAY_MS.
    static int64_t delay_for_(int attempts_made);

    bool reopen_spent_{false};
    /// Latches when the second attempt is retired.
    bool rescan_spent_{false};
    /// Set from handing out an attempt until rescan_done(); blocks re-arming and double counting.
    bool rescan_in_flight_{false};
    int rescan_attempts_{0};
    /// Frames accepted with no device to play them, not yet retired; see discard_frames().
    uint32_t discarded_frames_{0};
    /// Read by the main loop without the sink's lock; see pending().
    std::atomic<bool> rescan_owed_{false};
    int64_t rescan_at_ms_{NOT_STAMPED};
};

}  // namespace sendspin_cli
