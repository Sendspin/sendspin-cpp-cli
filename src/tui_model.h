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

#include <array>
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

    /// Feed the reply to request `id`, from note_sent(); failures become a notice.
    void on_command_reply(uint64_t id, const ControlExchange& reply, int64_t now_ms);

    void on_token(const ControlExchange& reply);  ///< the `pair-token` reply

    TuiScreen screen(int64_t now_ms) const;

    /// The request a key sends, or nullopt (Quit, Token, and keys that make no sense now).
    std::optional<ControlRequest> request_for(TuiKey key) const;

    /// Tell the model a request went out: later keys build on its target until a poll shows it.
    /// @return The id to feed its reply back with.
    uint64_t note_sent(const ControlRequest& request);

    /// `p`: show or hide; the view asks for it once. Only the main screen has the footer that
    /// shows it, so elsewhere this does nothing.
    /// @return whether it toggled.
    bool toggle_token(int64_t now_ms);
    bool wants_token() const;  ///< shown and not yet fetched
    const std::optional<std::string>& token() const;

    const PlayerView& view() const;  ///< the last poll, with pending targets laid over it

    std::optional<uint32_t> position_ms(int64_t now_ms) const;  ///< interpolated while playing
    std::string notice(int64_t now_ms) const;                   ///< empty when none is current
    std::string outcome_text() const;                           ///< for PairingOutcome
    std::string confirm_text() const;                           ///< for PairingConfirm
    std::string no_player_reason() const;                       ///< for NoPlayer and Denied

private:
    /// What the last status poll found on the socket.
    enum class Link { NoPlayer, Denied, TooOld, Attached };

    /// The controls a key sets to an absolute value.
    enum Control {
        PlayerVolume,
        PlayerMute,
        Delay,
        GroupVolume,
        GroupMute,
        Repeat,
        Shuffle,
        COUNT
    };

    /// A target sent but not yet seen in a poll.
    struct Pending {
        int value{0};
        uint64_t id{0};                     ///< the latest request that set it
        std::optional<int64_t> acked_ms{};  ///< when that request was answered ok
    };

    /// Where a `y` or `n` stands for the attempt on screen.
    enum class Consent { Ask, Sending, Allowed, Retry };

    static int value_of_(const PlayerView& view, Control control);
    static void set_value_(PlayerView& view, Control control, int value);

    void detach_(Link link, const std::string& reason);
    void overlay_();

    Link link_{Link::NoPlayer};
    std::string no_player_reason_;
    PlayerView reported_;  ///< the last poll as it came
    PlayerView view_;      ///< reported_ with pending_ laid over it
    std::array<std::optional<Pending>, COUNT> pending_{};
    uint64_t last_id_{0};
    int64_t received_ms_{0};  ///< when `view_` arrived, for the position

    /// Attempts at or below this ended before we attached, so are never shown.
    uint32_t pairing_baseline_{0};
    /// The last attached player's client id, kept across a detach to recognise it coming back.
    std::string attached_client_id_;
    /// The server a pairing was last under way with, and its name then: right after success the
    /// re-handshake can leave the connected server unknown for a poll.
    std::string pairing_id_seen_;
    std::string pairing_name_seen_;
    Consent consent_{Consent::Ask};
    bool consent_allows_{false};  ///< the request in flight is `y`
    uint64_t consent_id_{0};
    uint32_t consent_attempt_{0};  ///< the attempt counter when it went out; reset when it ends
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
