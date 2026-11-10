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

/// The TUI's behaviour without a UI library: which screen to show, what each key sends, and the
/// pairing takeovers. The view only draws what this decides.

#pragma once

#include "control.h"
#include "status_kv.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sendspin_cli {

enum class TuiScreen {
    NoPlayer,        ///< nothing answers on the control socket
    Denied,          ///< a player answers, but not to this user
    PlayerTooOld,    ///< the player predates `status-kv`
    Main,            ///< now playing
    PairingCode,     ///< a dynamic code, or an attempt awaiting its outcome
    PairingConfirm,  ///< a server waits on `pair confirm` or `pair cancel`
    PairingOutcome,  ///< how the last attempt ended, briefly
};

enum class TuiKey {
    Space,
    Left,
    Right,
    Up,
    Down,
    LeftBracket,
    RightBracket,
    MuteSelf,
    MuteGroup,
    Repeat,
    Shuffle,
    Switch,
    DelayDown,
    DelayUp,
    Token,
    Yes,
    No,
    Quit,
};

class TuiModel {
public:
    /// Feed every status-kv poll. `now_ms` is a monotonic clock.
    void on_status(const ControlExchange& reply, int64_t now_ms);

    /// Feed the reply to a request the TUI sent; failures become a notice.
    void on_command_reply(const ControlExchange& reply, int64_t now_ms);

    void on_token(const ControlExchange& reply);  ///< the `pair-token` reply

    TuiScreen screen(int64_t now_ms) const;

    /// The request a key sends, or nullopt (Quit, Token, and keys that make no sense now).
    std::optional<ControlRequest> request_for(TuiKey key) const;

    /// Tell the model a request went out, so an allowed attempt is not offered again.
    void note_sent(const ControlRequest& request);

    void toggle_token();       ///< `p`: show or hide; the view asks for it once
    bool wants_token() const;  ///< shown and not yet fetched
    const std::optional<std::string>& token() const;

    const PlayerView& view() const;

    std::optional<uint32_t> position_ms(int64_t now_ms) const;  ///< interpolated while playing
    std::string notice(int64_t now_ms) const;                   ///< empty when none is current
    std::string outcome_text() const;                           ///< for PairingOutcome
    std::string confirm_text() const;                           ///< for PairingConfirm
    std::string no_player_reason() const;                       ///< for NoPlayer and Denied

private:
    /// What the last status poll found on the socket.
    enum class Link { NoPlayer, Denied, TooOld, Attached };

    void detach_(Link link, const std::string& reason);

    Link link_{Link::NoPlayer};
    std::string no_player_reason_;
    PlayerView view_;
    int64_t received_ms_{0};  ///< when `view_` arrived, for the position

    /// Attempts at or below this ended before we attached, so are never shown.
    uint32_t pairing_baseline_{0};
    /// Set when `y` went out, to the attempt counter then; cleared when that attempt ends.
    std::optional<uint32_t> allowed_after_{};
    std::optional<int64_t> outcome_since_ms_{};
    std::string outcome_text_;

    std::string notice_;
    int64_t notice_until_ms_{0};

    bool token_shown_{false};
    bool token_fetched_{false};
    std::optional<std::string> token_{};
};

/// The code in 5-row block digits, or the code spaced out on one row when 6*n-1 > width.
std::vector<std::string> pairing_code_lines(const std::string& code, int width);

}  // namespace sendspin_cli
