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

#include "sink_recovery.h"

#include <limits>

namespace sendspin_cli {

bool SinkRecovery::reopen_due() {
    if (this->reopen_spent_) {
        // Already reopened once this stream: escalate to the rescan instead.
        this->escalate_();
        return false;
    }
    this->reopen_spent_ = true;
    return true;
}

void SinkRecovery::reopen_done(bool recovered) {
    if (recovered) {
        // The rescan stays in hand for the next outage.
        return;
    }
    this->escalate_();
}

bool SinkRecovery::rescan_due(int64_t now_ms) {
    if (!this->rescan_owed_.load(std::memory_order_relaxed)) {
        return false;
    }
    if (this->rescan_at_ms_ == NOT_STAMPED) {
        // Stamped on the first tick after escalation; the delay grows with attempts made.
        this->rescan_at_ms_ = now_ms + delay_for_(this->rescan_attempts_);
        return false;
    }
    if (now_ms < this->rescan_at_ms_) {
        return false;
    }
    ++this->rescan_attempts_;
    this->rescan_in_flight_ = true;
    // In flight: nothing more is owed until rescan_done() reports.
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    return true;
}

void SinkRecovery::rescan_done(bool recovered) {
    if (!this->rescan_in_flight_) {
        // No attempt outstanding: a duplicate report, or one a reset() already answered.
        return;
    }
    this->rescan_in_flight_ = false;
    if (recovered || this->rescan_attempts_ >= SINK_RESCAN_ATTEMPTS) {
        this->rescan_spent_ = true;
        this->rescan_owed_.store(false, std::memory_order_relaxed);
        return;
    }
    // Owe another; the next tick stamps a longer deadline.
    this->rescan_at_ms_ = NOT_STAMPED;
    this->rescan_owed_.store(true, std::memory_order_relaxed);
}

bool SinkRecovery::pending() const {
    return this->rescan_owed_.load(std::memory_order_relaxed);
}

void SinkRecovery::discard_frames(uint32_t frames) {
    const uint32_t room = std::numeric_limits<uint32_t>::max() - this->discarded_frames_;
    this->discarded_frames_ += (frames < room) ? frames : room;
}

uint32_t SinkRecovery::take_discarded_frames() {
    const uint32_t frames = this->discarded_frames_;
    this->discarded_frames_ = 0;
    return frames;
}

void SinkRecovery::forget_discarded_frames() {
    this->discarded_frames_ = 0;
}

void SinkRecovery::reset() {
    this->reopen_spent_ = false;
    this->rescan_spent_ = false;
    this->rescan_in_flight_ = false;
    this->rescan_attempts_ = 0;
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    this->rescan_at_ms_ = NOT_STAMPED;
    this->discarded_frames_ = 0;
}

void SinkRecovery::escalate_() {
    // Not while in flight: rescan_at_ms_ is already past, so re-arming would skip the delay.
    if (!this->rescan_spent_ && !this->rescan_in_flight_) {
        this->rescan_owed_.store(true, std::memory_order_relaxed);
    }
}

void OutageGapHandoff::add(uint32_t frames) {
    if (frames == 0) {
        return;
    }
    uint32_t current = this->frames_.load();
    // A compare-exchange rather than load-then-store: the callback can take() between the two, and
    // a plain store would hand back the gap it just retired.
    while (true) {
        const uint32_t room = std::numeric_limits<uint32_t>::max() - current;
        const uint32_t next = current + ((frames < room) ? frames : room);
        if (this->frames_.compare_exchange_weak(current, next)) {
            return;
        }
    }
}

uint32_t OutageGapHandoff::take() {
    return this->frames_.exchange(0);
}

void OutageGapHandoff::forget() {
    this->frames_.store(0);
}

int64_t SinkRecovery::delay_for_(int attempts_made) {
    int64_t delay = SINK_RESCAN_DELAY_MS;
    // Doubled in a loop rather than shifted, so raising SINK_RESCAN_ATTEMPTS cannot overflow.
    for (int i = 0; i < attempts_made && delay < SINK_RESCAN_MAX_DELAY_MS; ++i) {
        delay *= 2;
    }
    return (delay < SINK_RESCAN_MAX_DELAY_MS) ? delay : SINK_RESCAN_MAX_DELAY_MS;
}

}  // namespace sendspin_cli
