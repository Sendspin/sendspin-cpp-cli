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
        this->rescan_at_ms_ = now_ms + SINK_RESCAN_DELAY_MS;
        return false;
    }
    if (now_ms < this->rescan_at_ms_) {
        return false;
    }
    // Marked spent as it is handed out, not when the caller reports back -- there is no reporting
    // call, because a rescan that worked and one that did not both leave nothing else to try.
    this->rescan_spent_ = true;
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    return true;
}

bool SinkRecovery::pending() const {
    return this->rescan_owed_.load(std::memory_order_relaxed);
}

void SinkRecovery::reset() {
    this->reopen_spent_ = false;
    this->rescan_spent_ = false;
    this->rescan_owed_.store(false, std::memory_order_relaxed);
    this->rescan_at_ms_ = NOT_STAMPED;
}

void SinkRecovery::escalate_() {
    if (!this->rescan_spent_) {
        this->rescan_owed_.store(true, std::memory_order_relaxed);
    }
}

}  // namespace sendspin_cli
