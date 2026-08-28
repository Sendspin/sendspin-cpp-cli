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
/// @brief When the outbound mode is allowed to dial again, and what the last dial may claim

#pragma once

#include <cstdint>
#include <string>

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

/// @brief The last dial, and what SENDSPIN_SERVER_URL may honestly claim of it.
///
/// The library reports that a connection is up without saying where it came from -- there
/// is no connect callback, and nothing exposes a connection's URL or direction -- so the
/// dialled URL is exported only while nothing contradicts it being the connection's
/// origin. A dial to a discovered server knows which server_id it dialled, because the
/// mDNS instance label *is* the protocol server_id, so its URL is answered only for that
/// server; a dial to a literal -s URL promises nothing about who answers and is taken at
/// its word. Losing the connection forgets the dial either way: whatever its URL described
/// is gone, and no answer beats a stale one.
class LastDial {
public:
    /// @brief Records that connect_to() has just been called with `url`.
    /// @param server_id The dialled server's id when discovery chose it, or empty for a
    /// literal -s URL.
    void note_dial(const std::string& url, const std::string& server_id);

    /// @brief Forgets the dial: the connection it could have described is gone.
    void note_lost();

    /// @brief The URL to export for a stream arriving from `connected_server_id`.
    /// @return The dialled URL, or empty when no dial is live -- or when the dial named a
    /// server other than the connected one, an unknown one included.
    std::string url_for(const std::string& connected_server_id) const;

private:
    std::string url_;
    std::string server_id_;  ///< who url_ was expected to reach; empty means unverifiable
};

}  // namespace sendspin_cli
