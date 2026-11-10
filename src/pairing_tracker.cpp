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

#include "pairing_tracker.h"

namespace sendspin_cli {

void PairingTracker::started(const std::string& server_id) {
    if (!this->server_id_.empty()) {
        this->finish_(this->server_id_, PairingOutcome::Abandoned, "");
    }
    this->server_id_ = server_id;
    this->attempts_++;
}

void PairingTracker::succeeded(const std::string& server_id) {
    this->finish_(server_id, PairingOutcome::Paired, "");
}

void PairingTracker::failed(const std::string& server_id, const std::string& reason) {
    this->finish_(server_id, PairingOutcome::Failed, reason);
}

void PairingTracker::code_shown(const std::string& code) {
    this->code_ = code;
}

void PairingTracker::code_cleared() {
    this->code_.clear();
}

void PairingTracker::window_opened() {
    this->window_open_ = true;
}

void PairingTracker::window_closed() {
    this->window_open_ = false;
}

void PairingTracker::connection_ended() {
    if (!this->server_id_.empty()) {
        this->finish_(this->server_id_, PairingOutcome::Abandoned, "");
    }
}

const std::string& PairingTracker::in_progress_server_id() const {
    return this->server_id_;
}

const std::string& PairingTracker::code() const {
    return this->code_;
}

bool PairingTracker::window_open() const {
    return this->window_open_;
}

const PairingAttemptResult& PairingTracker::last() const {
    return this->last_;
}

void PairingTracker::finish_(const std::string& server_id, PairingOutcome outcome,
                             const std::string& reason) {
    if (this->server_id_.empty()) {
        // A result with no start seen: still an attempt.
        this->attempts_++;
    }
    this->last_ = {this->attempts_, server_id, outcome, reason};
    this->server_id_.clear();
}

}  // namespace sendspin_cli
