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

/// Session and pairing progress, for the log and for `status`.

#pragma once

#include <sendspin/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin_cli {

/// The library's 120 s attempt timeout plus margin: an attempt with no prompt is over by then.
inline constexpr int64_t UNPROMPTED_ATTEMPT_LIMIT_MS = 150000;

/// Logs session and pairing progress, and caches pairing progress for `status`. Main loop only.
class PairingListener final : public sendspin::SendspinClientListener {
public:
    /// One main-loop tick, after client.loop(): logs a new or re-keyed session, and forgets an
    /// attempt that ended without a callback. Read is_paired() before is_connected().
    /// @param now_ms A monotonic clock, in milliseconds.
    void tick(bool connected, bool paired, int64_t now_ms);

    void on_pairing_attempt(std::string_view server_id) override;
    void on_paired(std::string_view server_id) override;
    void on_pairing_aborted(std::string_view server_id,
                            sendspin::SendspinPairAbortReason reason) override;
    void on_pairing_code(std::string_view code,
                         sendspin::SendspinPairingCodeFormat format) override;
    void on_clear_pairing_code() override;
    void on_open_pairing_window() override;
    void on_close_pairing_window() override;

    /// The server a pairing is in progress with, or empty.
    const std::string& server_id() const {
        return this->server_id_;
    }

    /// The dynamic code on show, or empty.
    const std::string& code() const {
        return this->code_;
    }

    bool window_open() const {
        return this->window_open_;
    }

private:
    void forget_attempt();

    bool connected_{false};
    bool paired_{false};
    std::string server_id_;
    /// When a tick first saw the attempt; unset until one has.
    std::optional<int64_t> attempt_seen_ms_;
    /// Whether the attempt showed a code or window, whose withdrawal ends it with a callback.
    bool attempt_prompted_{false};
    std::string code_;
    bool window_open_{false};
};

}  // namespace sendspin_cli
