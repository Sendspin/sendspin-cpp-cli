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

#include "tui.h"

#include "control.h"
#include "status_kv.h"
#include "tui_model.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sendspin_cli {
namespace {

constexpr std::chrono::milliseconds POLL_INTERVAL{500};

/// How often the progress bar moves between polls.
constexpr std::chrono::milliseconds TICK_INTERVAL{250};

/// Below this many columns the panels stack.
constexpr int WIDE_COLUMNS = 80;

constexpr int VOLUME_PANEL_WIDTH = 27;

/// Rows the code takeover needs around 5-row block digits: borders, name, blanks, instruction.
constexpr int BLOCK_CODE_ROWS = 12;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Both ends must be a terminal: FTXUI reads keys from one and draws on the other.
bool is_terminal() {
#ifdef _WIN32
    // Not _isatty(), which is true for NUL too; a console is what FTXUI drives.
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0 &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
    return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;
#endif
}

/// Everything the UI thread owns. The worker reaches it only through posted closures.
struct TuiState {
    TuiModel model;
    bool token_shown{false};    ///< mirrors the model's toggle, to tell "failed" from "hidden"
    uint64_t token_presses{0};  ///< counts `p`, so a reply to an earlier press is dropped
};

/// Owns every socket exchange, so a slow player never stalls the keys or the drawing.
class Worker {
public:
    Worker(ftxui::ScreenInteractive& screen, TuiState& state, std::string path,
           std::string absent_reason)
        : screen_(screen),
          state_(state),
          path_(std::move(path)),
          absent_reason_(std::move(absent_reason)),
          thread_([this] { this->run_(); }),
          ticker_([this] { this->tick_(); }) {}

    ~Worker() {
        this->stop();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void send(const ControlRequest& request) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->requests_.push_back(request);
        this->wake_.notify_one();
    }

    /// @param press The `p` press the token is for.
    void fetch_token(uint64_t press) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->token_press_ = press;
        this->wake_.notify_one();
    }

    void stop() {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->stopping_ = true;
            this->wake_.notify_one();
            this->tick_wake_.notify_one();
        }
        if (this->ticker_.joinable()) {
            this->ticker_.join();
        }
        // At worst this waits out the one exchange in flight, bounded at 3 s.
        if (this->thread_.joinable()) {
            this->thread_.join();
        }
    }

private:
    ControlExchange exchange_(ControlCommand command) const {
        ControlRequest request;
        request.command = command;
        return exchange_control_request(request, this->path_, this->absent_reason_);
    }

    /// Runs `update` on the UI thread, then redraws.
    void post_(std::function<void(TuiState&)> update) {
        TuiState& state = this->state_;
        this->screen_.Post([&state, update = std::move(update)] { update(state); });
        this->screen_.PostEvent(ftxui::Event::Custom);
    }

    void run_() {
        using Clock = std::chrono::steady_clock;
        Clock::time_point next_poll = Clock::now();
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (true) {
            this->wake_.wait_until(lock, next_poll, [this] {
                return this->stopping_ || !this->requests_.empty() ||
                       this->token_press_.has_value();
            });
            if (this->stopping_) {
                return;
            }
            std::deque<ControlRequest> requests;
            requests.swap(this->requests_);
            const std::optional<uint64_t> token_press = std::exchange(this->token_press_, {});
            lock.unlock();

            // Checked before every exchange, so a quit waits out at most the one in flight.
            for (const ControlRequest& request : requests) {
                if (this->stopping_) {
                    return;
                }
                const ControlExchange reply =
                    exchange_control_request(request, this->path_, this->absent_reason_);
                const int64_t at = now_ms();
                this->post_(
                    [reply, at](TuiState& state) { state.model.on_command_reply(reply, at); });
                // Show what the key did now, not at the next poll.
                next_poll = Clock::now();
            }

            if (this->stopping_) {
                return;
            }
            if (token_press.has_value()) {
                const ControlExchange reply = this->exchange_(ControlCommand::PairToken);
                this->post_([reply, press = *token_press](TuiState& state) {
                    // An earlier press's reply would show a token the user has since hidden.
                    if (press == state.token_presses) {
                        state.model.on_token(reply);
                    }
                });
            }

            if (this->stopping_) {
                return;
            }
            if (Clock::now() >= next_poll) {
                const ControlExchange reply = this->exchange_(ControlCommand::StatusKv);
                const int64_t at = now_ms();
                this->post_([reply, at](TuiState& state) { state.model.on_status(reply, at); });
                next_poll = Clock::now() + POLL_INTERVAL;
            }
            lock.lock();
        }
    }

    /// Moves the progress bar. Its own thread, so a slow exchange never freezes the bar.
    void tick_() {
        const auto stopping = [this] { return this->stopping_.load(); };
        std::unique_lock<std::mutex> lock(this->mutex_);
        while (!this->tick_wake_.wait_for(lock, TICK_INTERVAL, stopping)) {
            this->screen_.PostEvent(ftxui::Event::Custom);
        }
    }

    ftxui::ScreenInteractive& screen_;
    TuiState& state_;  ///< touched only inside closures posted to the UI thread
    const std::string path_;
    const std::string absent_reason_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable tick_wake_;  ///< apart from wake_, so a key never wakes the ticker
    std::deque<ControlRequest> requests_;
    std::optional<uint64_t> token_press_;
    /// Set under `mutex_` for the waits; atomic so run_() can check it between exchanges.
    std::atomic<bool> stopping_{false};

    std::thread thread_;  ///< last, so it starts after everything it reads
    std::thread ticker_;
};

std::optional<TuiKey> key_for(const ftxui::Event& event) {
    if (event == ftxui::Event::ArrowLeft) {
        return TuiKey::Left;
    }
    if (event == ftxui::Event::ArrowRight) {
        return TuiKey::Right;
    }
    if (event == ftxui::Event::ArrowUp) {
        return TuiKey::Up;
    }
    if (event == ftxui::Event::ArrowDown) {
        return TuiKey::Down;
    }
    if (event == ftxui::Event::CtrlC) {
        return TuiKey::Quit;
    }
    if (!event.is_character()) {
        return std::nullopt;
    }
    static const std::pair<const char*, TuiKey> KEYS[] = {
        {" ", TuiKey::Space},    {"[", TuiKey::LeftBracket}, {"]", TuiKey::RightBracket},
        {"m", TuiKey::MuteSelf}, {"M", TuiKey::MuteGroup},   {"r", TuiKey::Repeat},
        {"x", TuiKey::Shuffle},  {"g", TuiKey::Switch},      {",", TuiKey::DelayDown},
        {".", TuiKey::DelayUp},  {"p", TuiKey::Token},       {"y", TuiKey::Yes},
        {"Y", TuiKey::Yes},      {"n", TuiKey::No},          {"N", TuiKey::No},
        {"q", TuiKey::Quit},
    };
    const std::string character = event.character();
    for (const auto& [typed, key] : KEYS) {
        if (character == typed) {
            return key;
        }
    }
    return std::nullopt;
}

bool is_takeover(TuiScreen screen) {
    return screen == TuiScreen::PairingCode || screen == TuiScreen::PairingConfirm ||
           screen == TuiScreen::PairingOutcome;
}

/// What the screen shows for a value nobody has reported.
std::string or_unknown(const std::string& value) {
    return value.empty() ? "--" : value;
}

/// `<m>:<ss>`, or `<h>:<mm>:<ss>` past an hour.
std::string format_clock(uint32_t milliseconds) {
    const uint32_t total = milliseconds / 1000U;
    char text[32];
    if (total >= 3600U) {
        std::snprintf(text, sizeof(text), "%u:%02u:%02u", total / 3600U, (total / 60U) % 60U,
                      total % 60U);
    } else {
        std::snprintf(text, sizeof(text), "%u:%02u", total / 60U, total % 60U);
    }
    return text;
}

std::string format_volume(uint8_t volume, bool muted) {
    return std::to_string(static_cast<unsigned>(volume)) + "%" + (muted ? "   [MUTED]" : "");
}

ftxui::Element hint(const std::string& text) {
    return ftxui::text(text) | ftxui::dim;
}

ftxui::Element panel(const std::string& title, ftxui::Elements lines) {
    return ftxui::window(ftxui::text(" " + title + " "), ftxui::vbox(std::move(lines)));
}

ftxui::Element now_playing_panel(const TuiModel& model) {
    const PlayerView& view = model.view();
    ftxui::Elements lines;
    if (view.title.empty() && view.artist.empty()) {
        lines = {ftxui::text("Press <space> to play"), ftxui::text("Press g to switch group"),
                 ftxui::text("")};
    } else {
        lines = {ftxui::text("Title:  " + or_unknown(view.title)) | ftxui::bold,
                 ftxui::text("Artist: " + or_unknown(view.artist)),
                 ftxui::text("Album:  " + or_unknown(view.album))};
    }
    const std::optional<ControlRequest> space = model.request_for(TuiKey::Space);
    const bool pauses = space.has_value() && space->command == ControlCommand::Pause;
    lines.push_back(ftxui::filler());
    lines.push_back(
        hint(std::string("<- prev   <space> ") + (pauses ? "pause" : "play") + "   next ->") |
        ftxui::hcenter);
    return panel("Now Playing", std::move(lines));
}

ftxui::Element volume_panel(const PlayerView& view) {
    // Unknown group state parses as 0, which would read as a real level.
    const std::string group =
        view.group_state_known ? format_volume(view.group_volume, view.group_muted) : "--";
    return panel("Volume",
                 {ftxui::text("Group:   " + group),
                  ftxui::text("Player:  " + format_volume(view.player_volume, view.player_muted)),
                  ftxui::filler(), hint("up/dn player  m mute"), hint("[/] group     M mute")});
}

ftxui::Element progress_panel(const TuiModel& model, int64_t now) {
    const std::optional<uint32_t> position = model.position_ms(now);
    const std::optional<uint32_t> duration = model.view().duration_ms;
    float fraction = 0.0F;
    std::string label = position.has_value() ? format_clock(*position) : "--:--";
    if (duration.has_value() && *duration > 0) {
        fraction = std::min(
            1.0F, static_cast<float>(position.value_or(0)) / static_cast<float>(*duration));
        label += " / " + format_clock(*duration);
    } else if (duration.has_value()) {
        label += " (live)";
    }
    return panel("Progress",
                 {ftxui::hbox({ftxui::gauge(fraction) | ftxui::flex, ftxui::text("  " + label)})});
}

ftxui::Element playback_panel(const PlayerView& view) {
    const bool known = view.group_state_known;
    return panel("Playback", {ftxui::text("Repeat:  " + (known ? or_unknown(view.repeat) : "--")),
                              ftxui::text(std::string("Shuffle: ") +
                                          (known ? (view.shuffle ? "on" : "off") : "--")),
                              ftxui::filler(), hint("r repeat  x shuffle")});
}

ftxui::Element stream_panel(const PlayerView& view) {
    std::string codec = or_unknown(view.codec);
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string format = "--";
    if (view.sample_rate > 0) {
        char rate[16];
        std::snprintf(rate, sizeof(rate), view.sample_rate % 1000U == 0 ? "%.0f" : "%.1f",
                      view.sample_rate / 1000.0);
        format = std::string(rate) + " kHz / " + std::to_string(view.bit_depth) + "-bit / " +
                 std::to_string(view.channels) + "ch";
    }
    return panel("Stream", {ftxui::text("Codec: " + codec), ftxui::text(format),
                            ftxui::text("Delay: " + std::to_string(view.output_delay_ms) + " ms"),
                            ftxui::filler(), hint(",/. adjust delay")});
}

ftxui::Element server_panel(const PlayerView& view) {
    ftxui::Elements lines;
    if (view.connected) {
        lines.push_back(ftxui::text(or_unknown(view.server_name)) | ftxui::bold);
        lines.push_back(
            ftxui::text("connected" + (view.trust.empty() ? std::string() : ", " + view.trust)));
    } else {
        lines.push_back(ftxui::text("No server connected"));
    }
    lines.push_back(ftxui::text("Group: " + or_unknown(view.group_name)));
    // Main shows only when no attempt is under way, so this is the whole condition.
    if (view.paired_servers == 0) {
        lines.push_back(ftxui::paragraph("Not paired yet. In Music Assistant, add this player and "
                                         "choose to pair with a code.") |
                        ftxui::color(ftxui::Color::Yellow));
    }
    lines.push_back(ftxui::filler());
    lines.push_back(hint("g switch group"));
    return panel("Server", std::move(lines));
}

ftxui::Element footer(const TuiState& state, int64_t now) {
    ftxui::Elements left;
    const std::string notice = state.model.notice(now);
    if (!notice.empty()) {
        left.push_back(ftxui::text(notice) | ftxui::bold | ftxui::color(ftxui::Color::Yellow));
    }
    if (state.token_shown) {
        // An empty token after the fetch means the request failed, not that there is none.
        const std::optional<std::string>& token = state.model.token();
        const std::string shown = token.has_value()           ? *token
                                  : state.model.wants_token() ? "fetching..."
                                                              : "unavailable";
        left.push_back(ftxui::text("Pairing token: " + shown));
    }
    return ftxui::hbox(
        {ftxui::vbox(std::move(left)) | ftxui::flex, hint("p pairing token  q quit")});
}

ftxui::Element main_screen(const TuiState& state, int64_t now, int width) {
    const TuiModel& model = state.model;
    const PlayerView& view = model.view();
    if (width < WIDE_COLUMNS) {
        // Clipped from the top when short, rather than every panel squashed empty.
        return ftxui::vbox({now_playing_panel(model), volume_panel(view),
                            progress_panel(model, now), playback_panel(view), stream_panel(view),
                            server_panel(view), footer(state, now)}) |
               ftxui::yframe;
    }
    return ftxui::vbox({
        ftxui::hbox(
            {now_playing_panel(model) | ftxui::flex,
             volume_panel(view) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, VOLUME_PANEL_WIDTH)}),
        progress_panel(model, now),
        // Fixed thirds: flex would hand the Server panel's paragraph the whole row.
        ftxui::hbox({playback_panel(view) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width / 3),
                     stream_panel(view) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width / 3),
                     server_panel(view) | ftxui::flex}),
        footer(state, now),
    });
}

/// Fills the terminal with `lines`, centred, so a pairing cannot be missed.
ftxui::Element takeover(ftxui::Elements lines, const std::string& notice) {
    ftxui::Elements centred;
    for (ftxui::Element& line : lines) {
        centred.push_back(std::move(line) | ftxui::hcenter);
    }
    if (!notice.empty()) {
        centred.push_back(ftxui::text(""));
        centred.push_back(ftxui::text(notice) | ftxui::color(ftxui::Color::Yellow) |
                          ftxui::hcenter);
    }
    return ftxui::vbox({ftxui::vbox(std::move(centred)) | ftxui::vcenter | ftxui::flex,
                        hint("q quit") | ftxui::align_right}) |
           ftxui::borderHeavy;
}

ftxui::Element pairing_code_screen(const PlayerView& view, int width, int height) {
    if (view.pairing_code.empty()) {
        return takeover({ftxui::text("Confirming with the server...") | ftxui::bold}, "");
    }
    const bool named = view.pairing_server_id == view.server_id && !view.server_name.empty();
    const std::string server = named ? view.server_name : view.pairing_server_id;
    ftxui::Elements lines;
    lines.push_back(ftxui::text(server.empty() ? "A server is pairing" : "Pairing with " + server));
    lines.push_back(ftxui::text(""));
    // Less the border and a column of margin each side; too short for block digits, a width of
    // 0 gets the plain form, so the code is never cut off.
    const int code_width = height >= BLOCK_CODE_ROWS ? width - 4 : 0;
    for (const std::string& line : pairing_code_lines(view.pairing_code, code_width)) {
        lines.push_back(ftxui::text(line) | ftxui::bold | ftxui::color(ftxui::Color::Cyan));
    }
    lines.push_back(ftxui::text(""));
    lines.push_back(ftxui::text("Type this into Music Assistant"));
    return takeover(std::move(lines), "");
}

/// The screens with no player to show: a heading, then why, as plain paragraphs.
ftxui::Element plain_screen(const std::string& heading, const std::vector<std::string>& body) {
    ftxui::Elements lines = {ftxui::text(heading) | ftxui::bold, ftxui::text("")};
    for (const std::string& paragraph : body) {
        lines.push_back(ftxui::paragraph(paragraph));
        lines.push_back(ftxui::text(""));
    }
    lines.push_back(ftxui::filler());
    lines.push_back(hint("Checking again every half second.  q quit"));
    return panel("sendspin-cli tui", std::move(lines)) | ftxui::flex;
}

ftxui::Element render(const TuiState& state, const std::string& path) {
    const int64_t now = now_ms();
    const ftxui::Dimensions size = ftxui::Terminal::Size();
    const TuiModel& model = state.model;
    const std::string notice = model.notice(now);
    switch (model.screen(now)) {
        case TuiScreen::NoPlayer:
            // Also a poll that failed or did not parse, so the reason carries the weight.
            return plain_screen("No sendspin-cli player is running here, or it did not answer",
                                {model.no_player_reason()});
        case TuiScreen::Denied:
            return plain_screen(
                "A sendspin-cli player is running, but this user may not connect to it",
                {model.no_player_reason(), "Run the TUI as the player's user, for example:",
                 "sudo sendspin-cli tui --control-socket " + path});
        case TuiScreen::PlayerTooOld:
            return plain_screen(
                "The running sendspin-cli player is too old for the TUI",
                {model.no_player_reason(),
                 "It has no status-kv. Restart it on this version of sendspin-cli."});
        case TuiScreen::Main:
            return main_screen(state, now, size.dimx);
        case TuiScreen::PairingCode:
            return pairing_code_screen(model.view(), size.dimx, size.dimy);
        case TuiScreen::PairingConfirm:
            return takeover({ftxui::text(model.confirm_text()) | ftxui::bold}, notice);
        case TuiScreen::PairingOutcome:
            return takeover({ftxui::text(model.outcome_text()) | ftxui::bold}, notice);
    }
    return ftxui::text("");
}

}  // namespace

int run_tui(const std::string& path, const std::string& absent_reason) {
    if (!is_terminal()) {
        std::fprintf(stderr, "error: sendspin-cli tui needs a terminal\n");
        return 1;
    }

    // Before the screen, so it outlives any reply still queued on it.
    TuiState state;

    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    // Ctrl-C is a key like `q` here; FTXUI would otherwise re-raise SIGINT once it exits.
    screen.ForceHandleCtrlC(false);

    Worker worker(screen, state, path, absent_reason);

    ftxui::Component view = ftxui::Renderer([&state, &path] { return render(state, path); });
    view |= ftxui::CatchEvent([&](const ftxui::Event& event) {
        const std::optional<TuiKey> key = key_for(event);
        if (!key.has_value()) {
            return false;
        }
        if (*key == TuiKey::Quit) {
            screen.Exit();
            return true;
        }
        const int64_t now = now_ms();
        const TuiScreen shown = state.model.screen(now);
        // A takeover answers only y/n (when asked) and q; nothing behind it moves.
        const bool answers = *key == TuiKey::Yes || *key == TuiKey::No;
        if (is_takeover(shown) && !(shown == TuiScreen::PairingConfirm && answers)) {
            return true;
        }
        if (*key == TuiKey::Token) {
            // Only the main screen has the footer the token shows in.
            if (state.model.toggle_token(now)) {
                state.token_shown = !state.token_shown;
                state.token_presses++;
                if (state.model.wants_token()) {
                    worker.fetch_token(state.token_presses);
                }
            }
            return true;
        }
        if (const std::optional<ControlRequest> request = state.model.request_for(*key)) {
            worker.send(*request);
            state.model.note_sent(*request);
        }
        return true;
    });

    screen.Loop(view);
    worker.stop();
    return 0;
}

}  // namespace sendspin_cli
