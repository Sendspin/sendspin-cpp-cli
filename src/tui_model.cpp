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

#include "tui_model.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace sendspin_cli {
namespace {

/// How long a pairing outcome or a notice stays on screen.
constexpr int64_t MESSAGE_MS = 3000;

constexpr int VOLUME_STEP = 5;
constexpr int DELAY_STEP_MS = 10;

/// Characters of an unnamed server's id that "Paired with" shows.
constexpr size_t SHOWN_ID_CHARS = 12;

/// Rows of a block digit; each row is 5 columns, '#' for ink.
constexpr int DIGIT_ROWS = 5;
constexpr const char* DIGIT_GLYPHS[10][DIGIT_ROWS] = {
    {"#####", "#   #", "#   #", "#   #", "#####"},  // 0
    {"  #  ", " ##  ", "  #  ", "  #  ", " ### "},  // 1
    {"#####", "    #", "#####", "#    ", "#####"},  // 2
    {"#####", "    #", " ####", "    #", "#####"},  // 3
    {"#   #", "#   #", "#####", "    #", "    #"},  // 4
    {"#####", "#    ", "#####", "    #", "#####"},  // 5
    {"#####", "#    ", "#####", "#   #", "#####"},  // 6
    {"#####", "    #", "   # ", "  #  ", "  #  "},  // 7
    {"#####", "#   #", "#####", "#   #", "#####"},  // 8
    {"#####", "#   #", "#####", "    #", "#####"},  // 9
};

/// U+2588 FULL BLOCK, as UTF-8.
constexpr const char* FULL_BLOCK = "\xE2\x96\x88";

uint8_t step_volume(uint8_t volume, int step) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(volume) + step, 0, 100));
}

uint16_t step_delay(uint16_t delay_ms, int step) {
    return static_cast<uint16_t>(
        std::clamp(static_cast<int>(delay_ms) + step, 0, static_cast<int>(MAX_OUTPUT_DELAY_MS)));
}

/// Cycles as the Python TUI does: off, all, one.
sendspin::SendspinRepeatMode next_repeat(const std::string& repeat) {
    if (repeat == "all") {
        return sendspin::SendspinRepeatMode::ONE;
    }
    if (repeat == "one") {
        return sendspin::SendspinRepeatMode::OFF;
    }
    return sendspin::SendspinRepeatMode::ALL;
}

/// `status_state` says `playing (speed N/1000)` away from 1x; that is playing too.
bool is_playing(const PlayerView& view) {
    return view.state.rfind("playing", 0) == 0;
}

ControlRequest make_request(ControlCommand command) {
    ControlRequest request;
    request.command = command;
    return request;
}

std::string outcome_text_for(const PlayerView& view) {
    if (view.last_pairing_outcome == "paired") {
        if (view.last_pairing_server_id == view.server_id) {
            return "Paired with " + view.server_name;
        }
        return "Paired with " + view.last_pairing_server_id.substr(0, SHOWN_ID_CHARS) + "...";
    }
    if (view.last_pairing_outcome == "failed") {
        return "Pairing failed: " + view.last_pairing_reason;
    }
    return "The server stopped pairing";
}

}  // namespace

void TuiModel::on_status(const ControlExchange& reply, int64_t now_ms) {
    if (reply.status == ControlStatus::NoDaemon) {
        this->detach_(reply.denied ? Link::Denied : Link::NoPlayer, reply.reason);
        return;
    }
    if (reply.status == ControlStatus::Usage) {
        // A player without `status-kv` refuses it as an unknown subcommand.
        this->detach_(Link::TooOld, reply.reason);
        return;
    }
    PlayerView view;
    const bool parsed = reply.status == ControlStatus::Ok && parse_status_kv(reply.payload, view);
    if (!parsed) {
        const std::string reason = reply.status != ControlStatus::Ok
                                       ? reply.reason
                                       : "the player's status-kv reply is not a version this reads";
        if (this->link_ == Link::Attached) {
            // One poll that went wrong is not a player gone: keep the screen and the baseline,
            // so an attempt that ends meanwhile is still shown.
            this->notice_ = reply.status != ControlStatus::Ok
                                ? "The player did not answer -- retrying"
                                : reason;
            this->notice_until_ms_ = now_ms + MESSAGE_MS;
            return;
        }
        this->detach_(Link::NoPlayer, reason);
        return;
    }

    // Back on the same player, whose counter went on: what ended while away is still news.
    const bool resumed =
        this->link_ == Link::Attached ||
        (!this->attached_client_id_.empty() && view.client_id == this->attached_client_id_ &&
         view.last_pairing_attempt >= this->pairing_baseline_);
    if (!resumed) {
        // Whatever ended before this attach is history, not news.
        this->pairing_baseline_ = view.last_pairing_attempt;
    } else if (view.last_pairing_attempt > this->pairing_baseline_) {
        this->pairing_baseline_ = view.last_pairing_attempt;
        this->outcome_since_ms_ = now_ms;
        this->outcome_text_ = outcome_text_for(view);
    }
    // The window stays open until the attempt concludes, so the allow holds until then.
    if (this->allowed_after_.has_value() &&
        (!view.pairing_window || view.pairing_server_id.empty() ||
         view.last_pairing_attempt != *this->allowed_after_)) {
        this->allowed_after_.reset();
    }
    this->link_ = Link::Attached;
    this->attached_client_id_ = view.client_id;
    this->no_player_reason_.clear();
    this->view_ = std::move(view);
    this->received_ms_ = now_ms;
}

void TuiModel::on_command_reply(const ControlExchange& reply, int64_t now_ms) {
    switch (reply.status) {
        case ControlStatus::Ok:
            return;
        case ControlStatus::Unsupported:
            this->notice_ = "The server doesn't offer that";
            break;
        case ControlStatus::NotConnected:
            this->notice_ = "No server is connected";
            break;
        default:
            this->notice_ = reply.reason;
            break;
    }
    this->notice_until_ms_ = now_ms + MESSAGE_MS;
}

void TuiModel::on_token(const ControlExchange& reply) {
    this->token_fetched_ = true;
    if (reply.status != ControlStatus::Ok) {
        return;
    }
    std::string token = reply.payload;
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    this->token_ = std::move(token);
}

TuiScreen TuiModel::screen(int64_t now_ms) const {
    switch (this->link_) {
        case Link::NoPlayer:
            return TuiScreen::NoPlayer;
        case Link::Denied:
            return TuiScreen::Denied;
        case Link::TooOld:
            return TuiScreen::PlayerTooOld;
        case Link::Attached:
            break;
    }
    // A code first: an attempt let past the window shows its code while the window stays open.
    if (!this->view_.pairing_code.empty()) {
        return TuiScreen::PairingCode;
    }
    if (this->view_.pairing_window) {
        return TuiScreen::PairingConfirm;
    }
    if (!this->view_.pairing_server_id.empty()) {
        return TuiScreen::PairingCode;
    }
    if (this->outcome_since_ms_.has_value() && now_ms - *this->outcome_since_ms_ < MESSAGE_MS) {
        return TuiScreen::PairingOutcome;
    }
    return TuiScreen::Main;
}

std::optional<ControlRequest> TuiModel::request_for(TuiKey key) const {
    if (this->link_ != Link::Attached) {
        return std::nullopt;
    }
    const PlayerView& view = this->view_;
    ControlRequest request;
    switch (key) {
        case TuiKey::Space:
            return make_request(is_playing(view) ? ControlCommand::Pause : ControlCommand::Play);
        case TuiKey::Left:
            return make_request(ControlCommand::Previous);
        case TuiKey::Right:
            return make_request(ControlCommand::Next);
        case TuiKey::Switch:
            return make_request(ControlCommand::Switch);
        case TuiKey::Up:
        case TuiKey::Down:
            request.command = ControlCommand::PlayerVolume;
            request.volume =
                step_volume(view.player_volume, key == TuiKey::Up ? VOLUME_STEP : -VOLUME_STEP);
            return request;
        case TuiKey::MuteSelf:
            request.command = ControlCommand::PlayerMute;
            request.flag = !view.player_muted;
            return request;
        case TuiKey::DelayDown:
        case TuiKey::DelayUp:
            request.command = ControlCommand::Delay;
            request.delay_ms = step_delay(view.output_delay_ms,
                                          key == TuiKey::DelayUp ? DELAY_STEP_MS : -DELAY_STEP_MS);
            return request;
        case TuiKey::Yes:
        case TuiKey::No:
            // Asked only on PairingConfirm, and only until the user has allowed the attempt.
            if (!view.pairing_window || !view.pairing_code.empty() ||
                this->allowed_after_.has_value()) {
                return std::nullopt;
            }
            request.command = ControlCommand::Pair;
            request.confirm = key == TuiKey::Yes;
            return request;
        case TuiKey::Token:
        case TuiKey::Quit:
            return std::nullopt;
        default:
            break;
    }

    // The group keys. Unknown group state parses as defaults, so acting on it would be a guess.
    if (!view.group_state_known) {
        return std::nullopt;
    }
    switch (key) {
        case TuiKey::LeftBracket:
        case TuiKey::RightBracket:
            request.command = ControlCommand::Volume;
            request.volume = step_volume(view.group_volume,
                                         key == TuiKey::RightBracket ? VOLUME_STEP : -VOLUME_STEP);
            return request;
        case TuiKey::MuteGroup:
            request.command = ControlCommand::Mute;
            request.flag = !view.group_muted;
            return request;
        case TuiKey::Repeat:
            request.command = ControlCommand::Repeat;
            request.repeat = next_repeat(view.repeat);
            return request;
        case TuiKey::Shuffle:
            request.command = ControlCommand::Shuffle;
            request.flag = !view.shuffle;
            return request;
        default:
            return std::nullopt;
    }
}

void TuiModel::note_sent(const ControlRequest& request) {
    if (request.command == ControlCommand::Pair && request.confirm == std::optional<bool>(true)) {
        this->allowed_after_ = this->view_.last_pairing_attempt;
    }
}

void TuiModel::toggle_token() {
    this->token_shown_ = !this->token_shown_;
    if (!this->token_shown_) {
        // It is a password: forget it once hidden, and fetch it afresh when shown again.
        this->token_fetched_ = false;
        this->token_.reset();
    }
}

bool TuiModel::wants_token() const {
    return this->token_shown_ && !this->token_fetched_;
}

const std::optional<std::string>& TuiModel::token() const {
    return this->token_;
}

const PlayerView& TuiModel::view() const {
    return this->view_;
}

std::optional<uint32_t> TuiModel::position_ms(int64_t now_ms) const {
    const std::optional<uint32_t>& position = this->view_.position_ms;
    if (!position.has_value() || !is_playing(this->view_)) {
        return position;
    }
    const int64_t moved =
        static_cast<int64_t>(*position) + std::max<int64_t>(0, now_ms - this->received_ms_);
    // A duration of 0 is a live stream, with no end to stop at.
    const uint32_t duration = this->view_.duration_ms.value_or(0);
    const int64_t limit = duration != 0 ? duration : std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(moved, limit));
}

std::string TuiModel::notice(int64_t now_ms) const {
    return now_ms < this->notice_until_ms_ ? this->notice_ : std::string();
}

std::string TuiModel::outcome_text() const {
    return this->outcome_text_;
}

std::string TuiModel::confirm_text() const {
    return this->allowed_after_.has_value() ? "Allowed -- waiting for the server..."
                                            : "A server wants to pair. Y to allow, N to refuse";
}

std::string TuiModel::no_player_reason() const {
    return this->no_player_reason_;
}

void TuiModel::detach_(Link link, const std::string& reason) {
    this->link_ = link;
    this->no_player_reason_ = reason;
    this->view_ = PlayerView{};
    this->allowed_after_.reset();
}

std::vector<std::string> pairing_code_lines(const std::string& code, int width) {
    const bool digits_only =
        std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
    // 5 columns a digit and 1 between them.
    const int needed = 6 * static_cast<int>(code.size()) - 1;
    if (!digits_only || needed > width) {
        std::string spaced;
        for (const char c : code) {
            if (!spaced.empty()) {
                spaced += ' ';
            }
            spaced += c;
        }
        return {spaced};
    }

    std::vector<std::string> lines(DIGIT_ROWS);
    for (size_t i = 0; i < code.size(); i++) {
        for (int row = 0; row < DIGIT_ROWS; row++) {
            if (i > 0) {
                lines[row] += ' ';
            }
            for (const char* ink = DIGIT_GLYPHS[code[i] - '0'][row]; *ink != '\0'; ink++) {
                lines[row] += *ink == '#' ? FULL_BLOCK : " ";
            }
        }
    }
    return lines;
}

}  // namespace sendspin_cli
