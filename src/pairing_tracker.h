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

/// Remembers how the last pairing attempt ended, for `status` and the TUI.

#pragma once

#include <cstdint>
#include <string>

namespace sendspin_cli {

enum class PairingOutcome : uint8_t { None, Paired, Failed, Abandoned };

/// How one pairing attempt ended.
struct PairingAttemptResult {
    uint32_t attempt{0};  ///< 1-based number of the attempt that ended; 0 = none yet
    std::string server_id;
    PairingOutcome outcome{PairingOutcome::None};
    std::string reason;  ///< set for Failed only
};

/// Folds the library's pairing callbacks into the open attempt, the code and window on show,
/// and the result of the last attempt. Main loop only; no logging, no library types.
///
/// The library clears the code before reporting a success, after reporting a failure, and alone
/// when a connection drops, so clearing the code or closing the window never ends an attempt.
/// Only a result, a new handshake or connection loss, or a new attempt does.
class PairingTracker {
public:
    void started(const std::string& server_id);
    void succeeded(const std::string& server_id);
    void failed(const std::string& server_id, const std::string& reason);
    void code_shown(const std::string& code);
    void code_cleared();
    void window_opened();
    void window_closed();

    /// A new handshake, or the connection dropped: an open attempt will never report its end.
    void connection_ended();

    /// The server an attempt is open with, or empty.
    const std::string& in_progress_server_id() const;
    const std::string& code() const;
    bool window_open() const;
    const PairingAttemptResult& last() const;

private:
    /// Records the open attempt (or a result that arrived without a start) as ended.
    void finish_(const std::string& server_id, PairingOutcome outcome, const std::string& reason);

    std::string server_id_;  ///< empty when no attempt is open
    uint32_t attempts_{0};   ///< attempts begun so far
    std::string code_;
    bool window_open_{false};
    PairingAttemptResult last_;
};

}  // namespace sendspin_cli
