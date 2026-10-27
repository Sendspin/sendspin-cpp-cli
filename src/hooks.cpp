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

/// HookRunner's queueing; starting and reaping a hook live in hooks_posix.cpp and hooks_win.cpp.

#include "hooks.h"

#include "log.h"

#include <cstddef>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_HOOK;

void HookRunner::run(const std::string& command, const char* event, const HookContext& context) {
    if (!this->running_.empty()) {
        if (this->pending_.has_value()) {
            cli_log(LogLevel::DEBUG, "The queued %s hook was superseded by the %s event",
                    this->pending_->event.c_str(), event);
        }
        this->pending_ = PendingHook{command, event, context};
        return;
    }
    this->spawn(command, event, context);
}

void HookRunner::poll() {
    for (size_t index = 0; index < this->running_.size();) {
        if (!this->reap(this->running_[index])) {
            ++index;
            continue;
        }
        this->running_.erase(this->running_.begin() + static_cast<ptrdiff_t>(index));
    }

    if (this->running_.empty() && this->pending_.has_value()) {
        const PendingHook next = *this->pending_;
        this->pending_.reset();
        this->spawn(next.command, next.event.c_str(), next.context);
    }
}

void HookRunner::flush() {
    if (!this->pending_.has_value()) {
        return;
    }
    if (!this->running_.empty()) {
        cli_log(LogLevel::WARN, "The %s hook has not finished -- running the %s hook beside it",
                this->running_.front().event.c_str(), this->pending_->event.c_str());
    }
    const PendingHook next = *this->pending_;
    this->pending_.reset();
    this->spawn(next.command, next.event.c_str(), next.context);
}

}  // namespace sendspin_cli
