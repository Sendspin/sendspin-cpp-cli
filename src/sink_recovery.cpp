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

namespace sendspin_cli {

bool SinkRecovery::reopen_due() {
    if (this->reopen_spent_) {
        // A stream that was reopened once and has died again. Reopening it a second time would
        // be the same call against the same cached device list, so hand what is left of the
        // outage to the rescan instead -- that is the attempt that can still find something new.
        this->escalate_();
        return false;
    }
    this->reopen_spent_ = true;
    return true;
}

void SinkRecovery::reopen_done(bool recovered) {
    if (recovered) {
        // Playing again. The rescan stays in hand rather than being spent here: if this stream
        // dies too, that is the attempt that has not been tried.
        return;
    }
    this->escalate_();
}

bool SinkRecovery::rescan_due(int64_t now_ms) {
    if (!this->rescan_owed_.load(std::memory_order_relaxed)) {
        return false;
    }
    if (this->rescan_at_ms_ == NOT_STAMPED) {
        // Stamped on the first tick after the escalation -- or, for a retry, after the report
        // that asked for it -- rather than by either, because neither runs on a thread with any
        // business reading this clock. The delay grows with the attempts already made, so a retry
        // asks less often than the first attempt did.
        this->rescan_at_ms_ = now_ms + delay_for_(this->rescan_attempts_);
        return false;
    }
    if (now_ms < this->rescan_at_ms_) {
        return false;
    }
    ++this->rescan_attempts_;
    this->rescan_in_flight_ = true;
    // Nothing is owed while an attempt is in flight. What happens next is rescan_done()'s to say
    // -- and a caller that never says gets exactly one attempt, because the flag above is what
    // stops escalate_() from re-arming an attempt nobody has reported on.
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    return true;
}

void SinkRecovery::rescan_done(bool recovered) {
    if (!this->rescan_in_flight_) {
        // No attempt is outstanding, so there is nothing to report on: a second call for the same
        // attempt, or one that a reset() has already answered more completely than any recovery
        // could. Either would otherwise re-arm an attempt against a stream that is fine.
        return;
    }
    this->rescan_in_flight_ = false;
    if (recovered || this->rescan_attempts_ >= SINK_RESCAN_ATTEMPTS) {
        // Playing again, or out of attempts. Either way nothing further is owed until a
        // configure() that really got a stream running calls reset().
        this->rescan_spent_ = true;
        this->rescan_owed_.store(false, std::memory_order_relaxed);
        return;
    }
    // Failed with attempts left. Owe another, and clear the deadline so the next tick stamps a
    // fresh one -- longer than the last, per delay_for_().
    this->rescan_at_ms_ = NOT_STAMPED;
    this->rescan_owed_.store(true, std::memory_order_relaxed);
}

bool SinkRecovery::pending() const {
    return this->rescan_owed_.load(std::memory_order_relaxed);
}

void SinkRecovery::reset() {
    this->reopen_spent_ = false;
    this->rescan_spent_ = false;
    this->rescan_in_flight_ = false;
    this->rescan_attempts_ = 0;
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    this->rescan_at_ms_ = NOT_STAMPED;
}

void SinkRecovery::escalate_() {
    // Not while one is already in flight. What that attempt leaves owed is rescan_done()'s answer
    // rather than this one's -- and rescan_at_ms_ still holds the deadline the attempt was
    // released on, which is now in the past, so re-arming from here would have the next tick fire
    // the following attempt with no delay at all.
    if (!this->rescan_spent_ && !this->rescan_in_flight_) {
        this->rescan_owed_.store(true, std::memory_order_relaxed);
    }
}

int64_t SinkRecovery::delay_for_(int attempts_made) {
    int64_t delay = SINK_RESCAN_DELAY_MS;
    // Shifted by repeated doubling rather than `<< attempts_made`, so a caller that ever raises
    // SINK_RESCAN_ATTEMPTS cannot shift past the width of the type.
    for (int i = 0; i < attempts_made && delay < SINK_RESCAN_MAX_DELAY_MS; ++i) {
        delay *= 2;
    }
    return (delay < SINK_RESCAN_MAX_DELAY_MS) ? delay : SINK_RESCAN_MAX_DELAY_MS;
}

}  // namespace sendspin_cli
