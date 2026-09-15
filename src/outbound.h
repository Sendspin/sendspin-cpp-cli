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

/// When the outbound mode may dial again, and what the last dial may claim.

#pragma once

#include <cstdint>
#include <string>

namespace sendspin_cli {

/// The first redial delay, in milliseconds.
inline constexpr uint32_t MIN_RETRY_DELAY_MS = 1000;

/// The longest a redial ever waits, in milliseconds.
/// Matches sendspin-cpp's NURSERY_ESTABLISH_TIMEOUT_S; re-check on a SENDSPIN_GIT_TAG bump.
inline constexpr uint32_t MAX_RETRY_DELAY_MS = 30000;

/// Wait after `attempt` dials: doubling from MIN_RETRY_DELAY_MS, capped at MAX_RETRY_DELAY_MS.
uint32_t next_retry_delay_ms(uint32_t attempt);

/// Paces outbound dials from the last dial, since redialling while one is in flight cancels it.
class RetryPacer {
public:
    /// Feeds in the connection state; a handshake resets the backoff, a loss restarts it.
    /// @return true when this call observed a connection being lost.
    bool note_connection_state(bool connected, int64_t now_ms);

    /// True once the backoff since the last dial has elapsed. False while connected.
    bool should_dial(int64_t now_ms) const;

    /// Records that connect_to() has just been called, and advances the backoff.
    void note_dial(int64_t now_ms);

    /// How long the next dial will wait, in milliseconds.
    uint32_t delay_ms() const {
        return next_retry_delay_ms(this->dials_ > 0 ? this->dials_ - 1 : 0);
    }

private:
    int64_t last_dial_ms_{0};
    uint32_t dials_{0};  ///< dials made since the last completed handshake
    bool dialled_{false};
    bool connected_{false};
};

/// The last dial, and what SENDSPIN_SERVER_URL may honestly claim of it.
class LastDial {
public:
    /// Records that connect_to() has just been called with `url` for `server_id`.
    void note_dial(const std::string& url, const std::string& server_id);

    /// Forgets the dial: the connection it could have described is gone.
    void note_lost();

    /// The dialled URL, or empty unless a live dial named `connected_server_id`.
    std::string url_for(const std::string& connected_server_id) const;

private:
    std::string url_;
    std::string server_id_;  ///< who url_ was expected to reach
};

}  // namespace sendspin_cli
