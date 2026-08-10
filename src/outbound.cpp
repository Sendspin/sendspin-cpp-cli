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

#include "outbound.h"

#include <cstdint>

namespace sendspin_cli {

uint32_t next_retry_delay_ms(uint32_t attempt) {
    uint32_t delay = MIN_RETRY_DELAY_MS;
    // Counted up rather than shifted, so a large `attempt` cannot overflow the shift.
    for (uint32_t step = 0; step < attempt; ++step) {
        if (delay >= MAX_RETRY_DELAY_MS / 2) {
            return MAX_RETRY_DELAY_MS;
        }
        delay *= 2;
    }
    return delay > MAX_RETRY_DELAY_MS ? MAX_RETRY_DELAY_MS : delay;
}

bool RetryPacer::note_connection_state(bool connected, int64_t now_ms) {
    const bool lost = this->connected_ && !connected;
    this->connected_ = connected;

    if (connected) {
        this->dials_ = 0;
        return false;
    }
    if (lost) {
        // Restart the schedule, and pace the first redial from *now* rather than from the
        // dial that established the link -- which may have been hours ago, and would make
        // the redial immediate.
        this->dials_ = 1;
        this->last_dial_ms_ = now_ms;
        this->dialled_ = true;
    }
    return lost;
}

bool RetryPacer::should_dial(int64_t now_ms) const {
    if (this->connected_) {
        return false;
    }
    // The very first dial waits for nothing: there is no attempt in flight to protect.
    if (!this->dialled_) {
        return true;
    }
    return now_ms - this->last_dial_ms_ >= static_cast<int64_t>(this->delay_ms());
}

void RetryPacer::note_dial(int64_t now_ms) {
    this->last_dial_ms_ = now_ms;
    this->dialled_ = true;
    // Stops counting once the schedule has saturated: the delay would not change, and a
    // daemon that retried for years would otherwise wrap the counter.
    if (this->delay_ms() < MAX_RETRY_DELAY_MS) {
        ++this->dials_;
    }
}

}  // namespace sendspin_cli
