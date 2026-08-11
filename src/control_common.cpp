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

/// @file control_common.cpp
/// @brief The parts of the control channel that are pure data work
///
/// Separate from control_socket.cpp and control_client.cpp so the decisions that matter --
/// which argument shapes are legal, which requests the server will actually act on, what
/// `status` says, where one line ends -- are testable without binding a socket. The same split
/// mdns_common.cpp makes, and for the same reason: `tests/` deliberately opens nothing.

#include "control.h"

#include <limits.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::ClientCommandControllerObject;
using sendspin::SendspinControllerCommand;
using sendspin::SendspinRepeatMode;

namespace {

/// The `on|off` values `mute` and `shuffle` take.
constexpr const char* ON_OFF = "on|off";

/// The reply status line's machine-readable kinds, one per ControlStatus that crosses the wire.
///
/// A token rather than the numeric status, so a reply read by hand still says what happened.
/// `Ok` and `NoDaemon` are absent on purpose: `ok` has its own line shape, and nothing is
/// listening to say "no daemon" in the first place -- that one is the client's own conclusion.
struct ReplyKind {
    ControlStatus status;
    const char* token;
};
constexpr ReplyKind REPLY_KINDS[] = {
    {ControlStatus::Usage, "usage"},
    {ControlStatus::NotConnected, "not-connected"},
    {ControlStatus::Unsupported, "unsupported"},
    {ControlStatus::Failed, "failed"},
};

/// Parses a non-negative decimal integer, digits only, at most `max`.
///
/// Digits-only for the reason `cli.cpp`'s parse_port() gives: strtoull would accept " 50" and
/// "+50", and read "-1" as a huge unsigned that fails the range check only by accident.
bool parse_unsigned(const std::string& text, uint64_t max, uint64_t& value) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    errno = 0;
    // strtoull's own overflow report is still needed: a 30-digit string is all digits.
    const unsigned long long parsed = std::strtoull(text.c_str(), nullptr, 10);
    if (errno == ERANGE || parsed > max) {
        return false;
    }
    value = parsed;
    return true;
}

/// Parses a signed decimal integer that fits `int32_t`, with an optional leading sign.
bool parse_int32(const std::string& text, int32_t& value) {
    const bool negative = !text.empty() && text.front() == '-';
    const bool signed_form = negative || (!text.empty() && text.front() == '+');
    const std::string digits = signed_form ? text.substr(1) : text;
    uint64_t magnitude = 0;
    // The negative range is one wider than the positive one, and `seek-rel -2147483648` is a
    // legal offset -- so the bound is applied before the sign is, rather than by negating a
    // value that has already had to fit.
    const uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;
    if (!parse_unsigned(digits, limit, magnitude)) {
        return false;
    }
    value = negative ? static_cast<int32_t>(-static_cast<int64_t>(magnitude))
                     : static_cast<int32_t>(magnitude);
    return true;
}

/// Reads `on` or `off`.
bool parse_on_off(const std::string& text, bool& value) {
    if (text == "on") {
        value = true;
        return true;
    }
    if (text == "off") {
        value = false;
        return true;
    }
    return false;
}

/// Reads `off`, `one` or `all`.
bool parse_repeat_mode(const std::string& text, SendspinRepeatMode& mode) {
    if (text == "off") {
        mode = SendspinRepeatMode::OFF;
    } else if (text == "one") {
        mode = SendspinRepeatMode::ONE;
    } else if (text == "all") {
        mode = SendspinRepeatMode::ALL;
    } else {
        return false;
    }
    return true;
}

/// How `repeat` prints its own argument back onto the wire.
const char* repeat_mode_name(SendspinRepeatMode mode) {
    switch (mode) {
        case SendspinRepeatMode::ONE:
            return "one";
        case SendspinRepeatMode::ALL:
            return "all";
        case SendspinRepeatMode::OFF:
            break;
    }
    return "off";
}

/// The name a request's command was typed as, for a message that has to quote it back.
std::string command_name(ControlCommand command) {
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        if (subcommand.command == command) {
            return subcommand.name;
        }
    }
    // Unreachable while the table covers the enum, which ControlSubcommands.
    // EveryCommandInTheEnumHasARow pins down by walking the enum's whole range. Named rather
    // than asserted: a reply is not worth aborting a daemon for.
    return "?";
}

/// `<mm>:<ss>`, or `<h>:<mm>:<ss>` past an hour. Milliseconds are dropped: a position read by
/// a human wants the same resolution a player's display gives it.
std::string format_clock(uint32_t milliseconds) {
    const uint32_t total_seconds = milliseconds / 1000U;
    const uint32_t hours = total_seconds / 3600U;
    const uint32_t minutes = (total_seconds % 3600U) / 60U;
    const uint32_t seconds = total_seconds % 60U;

    const auto two_digits = [](uint32_t value) {
        return (value < 10 ? std::string("0") : std::string()) + std::to_string(value);
    };
    if (hours > 0) {
        return std::to_string(hours) + ":" + two_digits(minutes) + ":" + two_digits(seconds);
    }
    return std::to_string(minutes) + ":" + two_digits(seconds);
}

/// One `<key>: <value>` line.
void append_line(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += ": ";
    out += value;
    out += "\n";
}

/// How a volume and mute pair reads on one line: `55 (muted)`, or `unknown`.
std::string format_volume(bool known, uint8_t volume, bool muted) {
    if (!known) {
        return "unknown";
    }
    return std::to_string(static_cast<unsigned>(volume)) + (muted ? " (muted)" : "");
}

}  // namespace

const std::vector<ControlSubcommand>& control_subcommands() {
    // Function-local rather than a file-scope constant, so the vector is built on first use
    // rather than during static initialization -- and so the help strings live next to the
    // arity they describe.
    static const std::vector<ControlSubcommand> table = {
        {"status", ControlCommand::Status, 0, nullptr, "What this player and its group are doing"},
        {"play", ControlCommand::Play, 0, nullptr, "Resume or start playback"},
        {"pause", ControlCommand::Pause, 0, nullptr, "Pause playback"},
        {"stop", ControlCommand::Stop, 0, nullptr, "Stop playback"},
        {"next", ControlCommand::Next, 0, nullptr, "Skip to the next track"},
        {"prev", ControlCommand::Previous, 0, nullptr, "Skip to the previous track"},
        {"vol", ControlCommand::Volume, 1, "<0-100>",
         "Set the *group* volume -- the server spreads it across every player in the group and "
         "clamps it per player. Not this endpoint's own output level"},
        {"mute", ControlCommand::Mute, 1, ON_OFF, "Mute or unmute the group"},
        {"seek", ControlCommand::Seek, 1, "<ms>", "Seek to an absolute position, in milliseconds"},
        {"seek-rel", ControlCommand::SeekRelative, 1, "<+/-ms>",
         "Seek by an offset from the current position; negative seeks backwards"},
        {"repeat", ControlCommand::Repeat, 1, "off|one|all", "Set the repeat mode"},
        {"shuffle", ControlCommand::Shuffle, 1, ON_OFF, "Turn shuffle on or off"},
        {"switch", ControlCommand::Switch, 0, nullptr,
         "Move this player through the groups available to it. Not a source selector: per the "
         "spec's switch cycle it re-homes this client between groups"},
    };
    return table;
}

const ControlSubcommand* find_control_subcommand(const std::string& name) {
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        if (name == subcommand.name) {
            return &subcommand;
        }
    }
    return nullptr;
}

std::string control_subcommand_list() {
    std::string list;
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        if (!list.empty()) {
            list += ", ";
        }
        list += subcommand.name;
    }
    return list;
}

bool split_subcommand(int argc, char* const argv[], ControlInvocation& out, std::string& error) {
    out = ControlInvocation{};
    // A flag, or nothing at all: a daemon run, which is what every existing command line is.
    if (argc < 2 || argv[1][0] == '-') {
        return true;
    }

    const ControlSubcommand* subcommand = find_control_subcommand(argv[1]);
    if (subcommand == nullptr) {
        error = "unknown subcommand '" + std::string(argv[1]) + "' -- expected one of: " +
                control_subcommand_list();
        return false;
    }

    // Taken out of argv by count rather than by looking for the next flag, because the
    // argument may itself look exactly like one: `seek-rel -5000`.
    if (static_cast<unsigned>(argc) < 2U + subcommand->arity) {
        error = std::string("'") + subcommand->name + "' needs an argument: " +
                subcommand->name + " " + subcommand->argument;
        return false;
    }

    out.name = subcommand->name;
    for (unsigned index = 0; index < subcommand->arity; ++index) {
        out.args.emplace_back(argv[2 + index]);
    }
    out.consumed = static_cast<int>(2U + subcommand->arity);
    return true;
}

bool parse_control_request(const std::string& name, const std::vector<std::string>& args,
                          ControlRequest& out, std::string& error) {
    const ControlSubcommand* subcommand = find_control_subcommand(name);
    if (subcommand == nullptr) {
        error = "unknown subcommand '" + name + "' -- expected one of: " +
                control_subcommand_list();
        return false;
    }
    if (args.size() != subcommand->arity) {
        error = std::string("'") + subcommand->name + "' takes " +
                std::to_string(subcommand->arity) + " argument" +
                (subcommand->arity == 1 ? "" : "s") + ", got " + std::to_string(args.size());
        return false;
    }

    out = ControlRequest{};
    out.command = subcommand->command;
    if (subcommand->arity == 0) {
        return true;
    }

    const std::string& value = args.front();
    const auto reject = [&error, subcommand, &value](const std::string& accepted) {
        error = std::string("'") + subcommand->name + " " + value + "': expected " + accepted;
        return false;
    };

    switch (subcommand->command) {
        case ControlCommand::Volume: {
            uint64_t volume = 0;
            if (!parse_unsigned(value, 100, volume)) {
                return reject("a volume from 0 to 100");
            }
            out.volume = static_cast<uint8_t>(volume);
            return true;
        }
        case ControlCommand::Mute:
        case ControlCommand::Shuffle: {
            bool flag = false;
            if (!parse_on_off(value, flag)) {
                return reject(std::string("'on' or 'off'"));
            }
            out.flag = flag;
            return true;
        }
        case ControlCommand::Seek: {
            uint64_t position = 0;
            // Bounded by the field the library carries it in. The server's own `seek_max_ms`
            // is a tighter bound that only the daemon knows -- see control_refusal().
            if (!parse_unsigned(value, 4294967295ULL, position)) {
                return reject("a non-negative position in milliseconds, at most 4294967295 "
                              "(use seek-rel for a relative move)");
            }
            out.position_ms = static_cast<uint32_t>(position);
            return true;
        }
        case ControlCommand::SeekRelative: {
            int32_t offset = 0;
            // Bounded only by int32_t, deliberately. The server does not bound a relative
            // seek, and this client has no reliable view of the *group's* position to bound it
            // against: get_track_progress_ms() is this endpoint's own interpolated progress,
            // so refusing an offset against it would refuse legitimate commands whenever the
            // shadow was stale.
            if (!parse_int32(value, offset)) {
                return reject("an offset in milliseconds from -2147483648 to 2147483647");
            }
            out.offset_ms = offset;
            return true;
        }
        case ControlCommand::Repeat: {
            SendspinRepeatMode mode = SendspinRepeatMode::OFF;
            if (!parse_repeat_mode(value, mode)) {
                return reject(std::string("'off', 'one' or 'all'"));
            }
            out.repeat = mode;
            return true;
        }
        case ControlCommand::Status:
        case ControlCommand::Play:
        case ControlCommand::Pause:
        case ControlCommand::Stop:
        case ControlCommand::Next:
        case ControlCommand::Previous:
        case ControlCommand::Switch:
            break;
    }
    // Only reachable if the table gave an arity of 1 to a command with no argument to read,
    // which is a table bug rather than a user error.
    error = std::string("'") + subcommand->name + "' takes no argument";
    return false;
}

std::string encode_control_request(const ControlRequest& request) {
    std::string line = command_name(request.command);
    if (request.volume.has_value()) {
        line += " " + std::to_string(static_cast<unsigned>(*request.volume));
    } else if (request.flag.has_value()) {
        line += *request.flag ? " on" : " off";
    } else if (request.position_ms.has_value()) {
        line += " " + std::to_string(*request.position_ms);
    } else if (request.offset_ms.has_value()) {
        line += " " + std::to_string(*request.offset_ms);
    } else if (request.repeat.has_value()) {
        line += std::string(" ") + repeat_mode_name(*request.repeat);
    }
    return line;
}

bool split_control_line(const std::string& line, std::string& name,
                        std::vector<std::string>& args) {
    name.clear();
    args.clear();

    size_t position = 0;
    while (position < line.size()) {
        const size_t start = line.find_first_not_of(" \t", position);
        if (start == std::string::npos) {
            break;
        }
        const size_t end = line.find_first_of(" \t", start);
        std::string word = line.substr(start, end == std::string::npos ? end : end - start);
        if (name.empty()) {
            name = std::move(word);
        } else {
            args.push_back(std::move(word));
        }
        position = end == std::string::npos ? line.size() : end;
    }
    return !name.empty();
}

std::optional<SendspinControllerCommand> protocol_command(const ControlRequest& request) {
    switch (request.command) {
        case ControlCommand::Status:
            // Answered out of the daemon's own shadows; nothing goes to the server.
            return std::nullopt;
        case ControlCommand::Play:
            return SendspinControllerCommand::PLAY;
        case ControlCommand::Pause:
            return SendspinControllerCommand::PAUSE;
        case ControlCommand::Stop:
            return SendspinControllerCommand::STOP;
        case ControlCommand::Next:
            return SendspinControllerCommand::NEXT;
        case ControlCommand::Previous:
            return SendspinControllerCommand::PREVIOUS;
        case ControlCommand::Volume:
            return SendspinControllerCommand::VOLUME;
        case ControlCommand::Mute:
            return SendspinControllerCommand::MUTE;
        case ControlCommand::Seek:
            return SendspinControllerCommand::SEEK;
        case ControlCommand::SeekRelative:
            return SendspinControllerCommand::SEEK_RELATIVE;
        case ControlCommand::Repeat:
            // Three commands rather than one with a parameter, which is why `repeat` cannot be
            // a plain pass-through: the mode *is* the command.
            switch (request.repeat.value_or(SendspinRepeatMode::OFF)) {
                case SendspinRepeatMode::ONE:
                    return SendspinControllerCommand::REPEAT_ONE;
                case SendspinRepeatMode::ALL:
                    return SendspinControllerCommand::REPEAT_ALL;
                case SendspinRepeatMode::OFF:
                    break;
            }
            return SendspinControllerCommand::REPEAT_OFF;
        case ControlCommand::Shuffle:
            // Likewise: `off` is its own command rather than a false-valued parameter.
            return request.flag.value_or(false) ? SendspinControllerCommand::SHUFFLE
                                                : SendspinControllerCommand::UNSHUFFLE;
        case ControlCommand::Switch:
            return SendspinControllerCommand::SWITCH;
    }
    return std::nullopt;
}

ClientCommandControllerObject to_client_command(const ControlRequest& request) {
    ClientCommandControllerObject command;
    // Only reached for a request that has one, since the daemon checks protocol_command()
    // first; the value_or keeps a table bug from being undefined behaviour.
    command.command = protocol_command(request).value_or(SendspinControllerCommand::PLAY);
    // Set unconditionally rather than per command: the library ignores the fields a command
    // does not use, and only the field the parser filled in is ever present.
    command.volume = request.volume;
    command.muted = request.command == ControlCommand::Mute ? request.flag : std::nullopt;
    command.position_ms = request.position_ms;
    command.offset_ms = request.offset_ms;
    return command;
}

bool control_refusal(const ControlRequest& request, const ControllerSnapshot& snapshot,
                     ControlStatus& status, std::string& reason) {
    const std::optional<SendspinControllerCommand> command = protocol_command(request);
    if (!command.has_value()) {
        // `status` is answered locally, so none of the checks below apply to it -- and a
        // daemon with no server connection is exactly when reading it is most useful.
        return false;
    }

    // Ahead of the supported_commands test, and that order is the whole point:
    // on_controller_state_clear() empties supported_commands when the connection drops, so a
    // gate that looked there first would answer "pause is not supported" when the truth is
    // that nothing is connected.
    if (!snapshot.connected) {
        status = ControlStatus::NotConnected;
        reason = "this player is not connected to a Sendspin server, so there is nothing to "
                 "send '" +
                 command_name(request.command) +
                 "' to. Run 'sendspin-cli status' to see what it is doing";
        return true;
    }

    const std::vector<SendspinControllerCommand>& supported = snapshot.supported_commands;
    if (std::find(supported.begin(), supported.end(), *command) == supported.end()) {
        status = ControlStatus::Unsupported;
        reason = "the server does not offer '" + command_name(request.command) +
                 "': it is not in the supported_commands this connection published, so sending "
                 "it would be ignored";
        if (supported.empty()) {
            // Distinguished from a genuine absence, because the two need different actions:
            // this one resolves itself when the first server/state arrives.
            reason += ". The server has published no commands at all yet";
        }
        return true;
    }

    // The only bound the server publishes. A relative seek is deliberately not checked against
    // it -- see the note in parse_control_request().
    if (request.command == ControlCommand::Seek && snapshot.seek_max_ms.has_value() &&
        request.position_ms.value_or(0) > *snapshot.seek_max_ms) {
        status = ControlStatus::Usage;
        reason = "'seek " + std::to_string(request.position_ms.value_or(0)) +
                 "' is past the end of what this server says is seekable (seek_max_ms is " +
                 std::to_string(*snapshot.seek_max_ms) + ")";
        return true;
    }

    return false;
}

std::string format_status(const StatusSnapshot& snapshot) {
    std::string out;

    append_line(out, "name", snapshot.name);

    if (!snapshot.connected) {
        append_line(out, "server", "not connected");
    } else if (snapshot.server_name.empty()) {
        // A completed handshake with no name yet: still connected, and saying so beats a
        // blank value that reads like a missing field.
        append_line(out, "server", "connected (" + (snapshot.server_id.empty()
                                                        ? std::string("no identity yet")
                                                        : snapshot.server_id) +
                                       ")");
    } else {
        append_line(out, "server", snapshot.server_name + " (connected)");
    }

    // The group's transport state, from the metadata progress rather than guessed at. The
    // speed is per-mille, so 1000 is normal playback and 0 is paused.
    if (!snapshot.playback_speed.has_value()) {
        append_line(out, "state", "unknown");
    } else if (*snapshot.playback_speed == 0) {
        append_line(out, "state", "paused");
    } else if (*snapshot.playback_speed == 1000) {
        append_line(out, "state", "playing");
    } else {
        append_line(out, "state",
                    "playing (speed " + std::to_string(*snapshot.playback_speed) + "/1000)");
    }

    // Deliberately its own line rather than folded into `state`: this is whether audio is
    // arriving at *this* endpoint, which a player dropped from the group loses while the
    // group keeps playing.
    append_line(out, "stream", snapshot.streaming ? "receiving" : "idle");

    if (snapshot.title.empty() && snapshot.artist.empty()) {
        append_line(out, "track", "unknown");
    } else if (snapshot.artist.empty()) {
        append_line(out, "track", snapshot.title);
    } else if (snapshot.title.empty()) {
        append_line(out, "track", snapshot.artist);
    } else {
        append_line(out, "track", snapshot.artist + " - " + snapshot.title);
    }

    if (!snapshot.progress_ms.has_value()) {
        append_line(out, "position", "unknown");
    } else {
        const std::string position = format_clock(*snapshot.progress_ms);
        const uint32_t duration = snapshot.duration_ms.value_or(0);
        // A zero duration is a live or unknown-length stream rather than a zero-length track,
        // so it is named instead of printed as 0:00.
        append_line(out, "position",
                    duration == 0 ? position + " / unknown"
                                  : position + " / " + format_clock(duration));
    }

    append_line(out, "group volume",
                format_volume(snapshot.group_state_known, snapshot.group_volume,
                              snapshot.group_muted));
    // Always known: it is this process's own state, whether or not a server is connected.
    append_line(out, "player volume",
                format_volume(true, snapshot.player_volume, snapshot.player_muted));

    if (snapshot.format.has_value()) {
        append_line(out, "output",
                    snapshot.output + " (" + std::to_string(snapshot.format->sample_rate) +
                        " Hz / " +
                        std::to_string(static_cast<unsigned>(snapshot.format->channels)) +
                        " ch / " +
                        std::to_string(static_cast<unsigned>(snapshot.format->bit_depth)) +
                        "-bit)");
    } else {
        append_line(out, "output", snapshot.output);
    }

    return out;
}

std::string encode_control_reply(ControlStatus status, const std::string& reason,
                                const std::string& payload) {
    if (status == ControlStatus::Ok) {
        return "ok\n" + payload;
    }

    // The first newline is the whole of this format's framing, so a reason carrying one would
    // turn the rest of it into payload -- and the client would print a diagnostic as though it
    // were part of a status. True by construction today, since every reason is one line and the
    // only interpolated values come from a request line that by definition holds no newline; but
    // that is an invariant held in another file, which is the argument that already earned
    // connect_to_socket() its own bound check.
    std::string one_line = reason;
    std::replace(one_line.begin(), one_line.end(), '\n', ' ');
    std::replace(one_line.begin(), one_line.end(), '\r', ' ');

    for (const ReplyKind& kind : REPLY_KINDS) {
        if (kind.status == status) {
            return "error " + std::string(kind.token) + ": " + one_line + "\n" + payload;
        }
    }
    // NoDaemon never crosses the wire -- there is nothing at the other end to send it -- so a
    // status with no token is a bug here rather than a reply worth shaping.
    return "error failed: " + one_line + "\n" + payload;
}

bool decode_control_reply(const std::string& line, ControlStatus& status, std::string& reason) {
    reason.clear();
    if (line == "ok") {
        status = ControlStatus::Ok;
        return true;
    }

    static constexpr const char* PREFIX = "error ";
    const size_t prefix_length = std::strlen(PREFIX);
    if (line.compare(0, prefix_length, PREFIX) != 0) {
        return false;
    }
    const size_t colon = line.find(':', prefix_length);
    if (colon == std::string::npos) {
        return false;
    }

    const std::string token = line.substr(prefix_length, colon - prefix_length);
    reason = line.substr(colon + 1);
    if (!reason.empty() && reason.front() == ' ') {
        reason.erase(0, 1);
    }
    for (const ReplyKind& kind : REPLY_KINDS) {
        if (token == kind.token) {
            status = kind.status;
            return true;
        }
    }
    // A well-formed reply naming a kind this build does not know: a newer daemon against an
    // older subcommand. The reason is still the useful part, so it is kept and the status
    // degrades to a plain failure rather than the whole reply being discarded.
    status = ControlStatus::Failed;
    return true;
}

LineState LineAssembler::feed(const char* data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        const char byte = data[index];
        if (byte == '\0') {
            // A text protocol, and a NUL is how a truncated C string or a binary peer arrives.
            // Refused rather than carried, so nothing downstream has to be NUL-safe.
            return LineState::Invalid;
        }
        if (byte == '\n') {
            this->line_ = std::move(this->buffer_);
            this->buffer_.clear();
            // A '\r' from a peer with CRLF line endings, which `printf 'status\r\n' | socat`
            // produces on some shells.
            if (!this->line_.empty() && this->line_.back() == '\r') {
                this->line_.pop_back();
            }
            // Anything after the newline is dropped: one command per connection, so a second
            // line is a peer talking out of turn rather than a pipeline to honour.
            return LineState::Ready;
        }
        if (this->buffer_.size() >= MAX_CONTROL_LINE_BYTES) {
            return LineState::TooLong;
        }
        this->buffer_ += byte;
    }
    return LineState::Incomplete;
}

LineState LineAssembler::finish() {
    if (this->buffer_.empty()) {
        return LineState::Incomplete;
    }
    // A peer that wrote its request and shut the write side down without a newline. Taking it
    // is what makes `printf status | socat - UNIX-CONNECT:...` work, and costs nothing: the
    // bytes are already bounded, and end-of-input is as unambiguous a terminator as '\n'.
    this->line_ = std::move(this->buffer_);
    this->buffer_.clear();
    if (!this->line_.empty() && this->line_.back() == '\r') {
        this->line_.pop_back();
    }
    return LineState::Ready;
}

const char* line_state_reason(LineState state) {
    switch (state) {
        case LineState::TooLong:
            return "the request line is too long";
        case LineState::Invalid:
            return "the request contains a NUL byte, and this is a text protocol";
        case LineState::Incomplete:
            return "the connection closed before a whole request arrived";
        case LineState::Ready:
            break;
    }
    return "";
}

bool is_private_runtime_dir(const std::string& path, std::string& reason) {
    struct stat info = {};
    // stat() rather than lstat(), so a symlink is judged by what it *points at*. That is the
    // safe direction here and the useful one: a link to a world-writable directory fails the
    // mode test below and a link into someone else's tree fails the ownership test, while
    // lstat() would instead refuse a legitimately symlinked $XDG_RUNTIME_DIR for being a link.
    if (::stat(path.c_str(), &info) != 0) {
        reason = "cannot stat " + path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        reason = path + " is not a directory";
        return false;
    }
    // The effective uid rather than the real one, because that is whose credentials the socket
    // will be created with and whom the kernel will compare a peer against.
    if (info.st_uid != ::geteuid()) {
        reason = path + " is owned by uid " + std::to_string(info.st_uid) + ", not by this user";
        return false;
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        // The whole point: a directory anyone else can write to is one where the socket can be
        // replaced or unlinked, whatever mode the socket itself carries.
        reason = path + " is writable by its group or by everyone";
        return false;
    }
    return true;
}

std::string control_platform_runtime_dir() {
#ifdef __APPLE__
    // confstr() rather than getenv("TMPDIR"): the two normally name the same per-user directory,
    // but only this one cannot be pointed somewhere else by the environment.
    char buffer[PATH_MAX] = {};
    const size_t length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof(buffer));
    // 0 is failure; a value above the buffer size means the answer was truncated, which is a
    // path this cannot use rather than one to guess at.
    if (length == 0 || length > sizeof(buffer)) {
        return {};
    }
    std::string path(buffer);
    // confstr() returns this one with a trailing slash, and every path built here joins with its
    // own '/'. Left in, the socket path would carry a '//' that is harmless to bind but makes two
    // spellings of one socket -- and the subcommand compares nothing, so it would simply look odd
    // in a log and in a diagnostic.
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    std::string reason;
    if (path.empty() || !is_private_runtime_dir(path, reason)) {
        return {};
    }
    return path;
#else
    // Elsewhere $XDG_RUNTIME_DIR is the convention, and its absence is a real absence rather
    // than a platform difference to paper over.
    return {};
#endif
}

std::string control_runtime_dir() {
    const char* value = std::getenv("XDG_RUNTIME_DIR");
    // Trusted rather than verified: this is the user saying where their runtime files go, and
    // second-guessing it would refuse legitimate setups. The platform fallback below is the one
    // this code chose, so it is the one this code checks.
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return control_platform_runtime_dir();
}

std::string control_socket_path(const std::string& runtime_dir, uint16_t port) {
    if (runtime_dir.empty()) {
        return {};
    }
    return runtime_dir + "/" + CONTROL_SOCKET_PREFIX + std::to_string(port) +
           CONTROL_SOCKET_SUFFIX;
}

std::string control_socket_absent_reason(const std::string& runtime_dir, const std::string& path) {
    if (runtime_dir.empty()) {
        // Names both sources where there are two, so the reader is not sent to check an
        // environment variable their platform never sets in the first place.
#ifdef __APPLE__
        return "$XDG_RUNTIME_DIR is not set and this host's own per-user temporary directory "
               "could not be used, so there is nowhere user-private to put a control socket. "
               "Give --control-socket <path> to choose one, or --no-control to stop asking";
#else
        return "$XDG_RUNTIME_DIR is not set, so there is no user-private directory to put a "
               "control socket in. Give --control-socket <path> to choose one, or "
               "--no-control to stop asking";
#endif
    }
    if (!control_socket_path_fits(path)) {
        return "the default control socket path '" + path + "' is longer than the " +
               std::to_string(control_socket_path_limit()) +
               " bytes a Unix socket address holds, because $XDG_RUNTIME_DIR is that deep. "
               "Give --control-socket <a shorter path>, or --no-control";
    }
    return {};
}

bool control_socket_path_fits(const std::string& path) {
    // The terminating NUL has to fit too: bind() takes sun_path as a C string, and a path that
    // exactly fills the array leaves nowhere for it.
    return !path.empty() && path.size() < control_socket_path_limit();
}

size_t control_socket_path_limit() {
    // 104 on macOS and the BSDs, 108 on Linux. Read off the struct rather than written down,
    // since it is the only number bind() actually honours.
    return sizeof(sockaddr_un::sun_path);
}

}  // namespace sendspin_cli
