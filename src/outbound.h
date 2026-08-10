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

/// @file outbound.h
/// @brief When the outbound mode is allowed to dial again

#pragma once

#include <cstdint>

namespace sendspin_cli {

/// @brief The first redial delay, in milliseconds.
inline constexpr uint32_t MIN_RETRY_DELAY_MS = 1000;

/// @brief The longest a redial ever waits, in milliseconds.
///
/// Matched to the library's own `NURSERY_ESTABLISH_TIMEOUT_S` (30 s, in its private
/// `src/connection_manager.h`), which is when it reaps an outbound attempt that never
/// completed a handshake. Waiting longer than that would leave the player idle with nothing
/// in flight. Not included from there -- that header is not installed -- so a
/// `SENDSPIN_GIT_TAG` bump should re-check it.
inline constexpr uint32_t MAX_RETRY_DELAY_MS = 30000;

/// @brief How long to wait after `attempt` dials have already been made.
///
/// Exponential from MIN_RETRY_DELAY_MS, doubling per attempt, saturating at
/// MAX_RETRY_DELAY_MS.
uint32_t next_retry_delay_ms(uint32_t attempt);

/// @brief Paces outbound dials so a redial never cancels the attempt before it.
///
/// The library does not retry for us, and deliberately so: `ConnectionManager::connect_to()`
/// calls `set_auto_reconnect(false)`, and `SendspinClientListener` has no connect or
/// disconnect callback -- so the only signal available is polling
/// `SendspinClient::is_connected()`.
///
/// That signal is the reason this class exists rather than a bare timer. `is_connected()`
/// only goes true on a *completed handshake*, so it reads false for the whole of an
/// in-flight attempt; redialling on every tick that reports false would tear down the
/// attempt in progress each time, since `connect_to()` releases any previous outbound
/// nursery entry with ANOTHER_SERVER before pushing the new one. So dials are paced from
/// the last dial, never from observing "not connected".
///
/// Holds no clock of its own: every method takes the caller's monotonic milliseconds, which
/// is what makes the schedule testable without waiting for it.
class RetryPacer {
public:
    /// @brief Feeds in the client's current connection state.
    ///
    /// A completed handshake resets the schedule; losing one restarts it from
    /// MIN_RETRY_DELAY_MS, since a dropped link is usually transient and worth retrying
    /// promptly -- but not instantly, which would spin against a server that is shutting
    /// down.
    /// @return true when this call observed a connection being lost.
    bool note_connection_state(bool connected, int64_t now_ms);

    /// @brief True once the backoff since the last dial has elapsed. False while connected.
    bool should_dial(int64_t now_ms) const;

    /// @brief Records that connect_to() has just been called, and advances the backoff.
    void note_dial(int64_t now_ms);

    /// @brief How long the next dial will wait, in milliseconds.
    ///
    /// Indexed off the dials already made rather off `dials_` itself, so the wait *after*
    /// the first dial is MIN_RETRY_DELAY_MS rather than twice it.
    uint32_t delay_ms() const {
        return next_retry_delay_ms(this->dials_ > 0 ? this->dials_ - 1 : 0);
    }

private:
    int64_t last_dial_ms_{0};
    uint32_t dials_{0};  ///< dials made since the last completed handshake
    bool dialled_{false};
    bool connected_{false};
};

}  // namespace sendspin_cli
