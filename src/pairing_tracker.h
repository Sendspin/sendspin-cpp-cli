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

/// Numbers pairing attempts and records how the last one ended. Main loop only; no library types.
class PairingTracker {
public:
    /// A new attempt; one still open is recorded as abandoned.
    void started(const std::string& server_id);
    void succeeded(const std::string& server_id);
    void failed(const std::string& server_id, const std::string& reason);

    /// The open attempt ended without an outcome callback (prompt withdrawn, connection lost).
    void ended();

    const PairingAttemptResult& last() const {
        return this->last_;
    }

private:
    /// Records an outcome; one that trails ended() for the same server upgrades that record.
    void finish_(const std::string& server_id, PairingOutcome outcome, const std::string& reason);

    std::string server_id_;  ///< empty when no attempt is open
    uint32_t attempts_{0};   ///< attempts begun so far
    PairingAttemptResult last_;
};

}  // namespace sendspin_cli
