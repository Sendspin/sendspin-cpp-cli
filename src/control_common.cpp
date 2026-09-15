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

/// The control channel's pure data work, testable without a socket.

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

/// Reply status tokens for each ControlStatus that crosses the wire.
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
    // The bound is applied before the sign, so INT32_MIN still fits.
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
    // Unreachable while the table covers the enum.
    return "?";
}

/// `<mm>:<ss>`, or `<h>:<mm>:<ss>` past an hour.
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

/// Qualifier naming who chose the player's volume; empty for a server's own figure.
const char* volume_source_note(VolumeSource source) {
    switch (source) {
        case VolumeSource::Restored:
            return " (remembered from an earlier run; no server has set it)";
        case VolumeSource::Server:
            return "";
        case VolumeSource::SinkDefault:
            break;
    }
    return " (default; no server has set it)";
}

}  // namespace

const std::vector<ControlSubcommand>& control_subcommands() {
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
        {"delay", ControlCommand::Delay, 1, "<0-5000>",
         "Tell *this* endpoint how much latency its hardware adds after the audio port -- an "
         "amplifier, an external speaker. The player then hands audio over that much earlier, so "
         "the sound lands in sync rather than late. Answered locally, remembered across restarts, "
         "and not a group setting"},
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
    // A flag or nothing: a daemon run.
    if (argc < 2 || argv[1][0] == '-') {
        return true;
    }

    const ControlSubcommand* subcommand = find_control_subcommand(argv[1]);
    if (subcommand == nullptr) {
        error = "unknown subcommand '" + std::string(argv[1]) +
                "' -- expected one of: " + control_subcommand_list();
        return false;
    }

    // Taken by count, since the argument may look like a flag.
    if (static_cast<unsigned>(argc) < 2U + subcommand->arity) {
        error = std::string("'") + subcommand->name + "' needs an argument: " + subcommand->name +
                " " + subcommand->argument;
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
        error =
            "unknown subcommand '" + name + "' -- expected one of: " + control_subcommand_list();
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
        case ControlCommand::Delay: {
            uint64_t delay = 0;
            // Refused, not clamped: the library would silently clamp to 5000.
            if (!parse_unsigned(value, MAX_STATIC_DELAY_MS, delay)) {
                return reject("a static delay in milliseconds, from 0 to " +
                              std::to_string(MAX_STATIC_DELAY_MS));
            }
            out.delay_ms = static_cast<uint16_t>(delay);
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
            // The server's seek_max_ms is checked by the daemon.
            if (!parse_unsigned(value, 4294967295ULL, position)) {
                return reject("a non-negative position in milliseconds, at most 4294967295 "
                              "(use seek-rel for a relative move)");
            }
            out.position_ms = static_cast<uint32_t>(position);
            return true;
        }
        case ControlCommand::SeekRelative: {
            int32_t offset = 0;
            // Bounded only by int32_t: there is no reliable group position to bound against.
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
    // Only a table bug reaches here.
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
    } else if (request.delay_ms.has_value()) {
        line += " " + std::to_string(static_cast<unsigned>(*request.delay_ms));
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
            return std::nullopt;
        case ControlCommand::Delay:
            // Set on the player role, which republishes client/state itself.
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
            // Each mode is its own protocol command.
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
            // Likewise, each shuffle state is its own command.
            return request.flag.value_or(false) ? SendspinControllerCommand::SHUFFLE
                                                : SendspinControllerCommand::UNSHUFFLE;
        case ControlCommand::Switch:
            return SendspinControllerCommand::SWITCH;
    }
    return std::nullopt;
}

ClientCommandControllerObject to_client_command(const ControlRequest& request) {
    ClientCommandControllerObject command;
    // value_or only guards a table bug; the daemon checks protocol_command() first.
    command.command = protocol_command(request).value_or(SendspinControllerCommand::PLAY);
    // The library ignores fields a command does not use.
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
        // Locally answered: none of the checks below apply.
        return false;
    }

    // Before the supported_commands check, which a disconnect empties.
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
            reason += ". The server has published no commands at all yet";
        }
        return true;
    }

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
        append_line(
            out, "server",
            "connected (" +
                (snapshot.server_id.empty() ? std::string("no identity yet") : snapshot.server_id) +
                ")");
    } else {
        append_line(out, "server", snapshot.server_name + " (connected)");
    }

    // playback_speed is per-mille: 1000 is normal, 0 is paused.
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

    // Whether audio arrives here, separate from the group's transport state.
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
        // A zero duration is a live or unknown-length stream.
        std::string reading =
            duration == 0 ? position + " / unknown" : position + " / " + format_clock(duration);
        // Interpolated while playing, and stale after a seek until the server resends progress.
        if (snapshot.playback_speed.value_or(0) != 0) {
            reading += " (estimated)";
        }
        append_line(out, "position", reading);
    }

    append_line(
        out, "group volume",
        format_volume(snapshot.group_state_known, snapshot.group_volume, snapshot.group_muted));
    append_line(out, "repeat",
                snapshot.group_state_known ? repeat_mode_name(snapshot.group_repeat) : "unknown");
    append_line(out, "shuffle",
                snapshot.group_state_known ? (snapshot.group_shuffle ? "on" : "off") : "unknown");
    append_line(out, "player volume",
                format_volume(true, snapshot.player_volume, snapshot.player_muted) +
                    volume_source_note(snapshot.player_volume_source));
    append_line(out, "static delay",
                std::to_string(static_cast<unsigned>(snapshot.static_delay_ms)) + " ms");

    if (snapshot.connected) {
        append_line(out, "note",
                    "state, position, repeat and shuffle are the server's last report; a server "
                    "that does not resend them after a change will show stale values here");
    }

    if (snapshot.format.has_value()) {
        append_line(
            out, "output",
            snapshot.output + " (" + std::to_string(snapshot.format->sample_rate) + " Hz / " +
                std::to_string(static_cast<unsigned>(snapshot.format->channels)) + " ch / " +
                std::to_string(static_cast<unsigned>(snapshot.format->bit_depth)) + "-bit)");
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

    // A newline in the reason would turn the rest into payload.
    std::string one_line = reason;
    std::replace(one_line.begin(), one_line.end(), '\n', ' ');
    std::replace(one_line.begin(), one_line.end(), '\r', ' ');

    for (const ReplyKind& kind : REPLY_KINDS) {
        if (kind.status == status) {
            return "error " + std::string(kind.token) + ": " + one_line + "\n" + payload;
        }
    }
    // NoDaemon never crosses the wire.
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
    // Unknown kind from a newer daemon: keep the reason, degrade to Failed.
    status = ControlStatus::Failed;
    return true;
}

LineState LineAssembler::feed(const char* data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        const char byte = data[index];
        if (byte == '\0') {
            // NUL means a truncated C string or a binary peer.
            return LineState::Invalid;
        }
        if (byte == '\n') {
            this->line_ = std::move(this->buffer_);
            this->buffer_.clear();
            // Tolerate CRLF.
            if (!this->line_.empty() && this->line_.back() == '\r') {
                this->line_.pop_back();
            }
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
    // A peer that shut its write side without a newline still sent a whole request.
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
    // stat(), not lstat(): judge a symlink by its target, so a linked runtime dir still works.
    if (::stat(path.c_str(), &info) != 0) {
        reason = "cannot stat " + path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        reason = path + " is not a directory";
        return false;
    }
    // The effective uid, whose credentials create the socket.
    if (info.st_uid != ::geteuid()) {
        reason = path + " is owned by uid " + std::to_string(info.st_uid) + ", not by this user";
        return false;
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        reason = path + " is writable by its group or by everyone";
        return false;
    }
    return true;
}

std::string control_platform_runtime_dir(std::string& rejection) {
#ifdef __APPLE__
    // confstr(), not $TMPDIR, which the environment could redirect.
    char buffer[PATH_MAX] = {};
    const size_t length = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof(buffer));
    // 0 is failure; larger than the buffer means truncated.
    if (length == 0 || length > sizeof(buffer)) {
        return {};
    }
    std::string path(buffer);
    // Strip confstr()'s trailing slash so the path has one spelling.
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    if (path.empty()) {
        return {};
    }
    if (!is_private_runtime_dir(path, rejection)) {
        return {};
    }
    return path;
#else
    static_cast<void>(rejection);
    return {};
#endif
}

ControlRuntimeDir control_runtime_dir() {
    ControlRuntimeDir result;

    const char* value = std::getenv("XDG_RUNTIME_DIR");
    if (value != nullptr && value[0] != '\0') {
        // Honoured even if group-writable, but warned about.
        result.path = value;
        std::string reason;
        if (!is_private_runtime_dir(result.path, reason)) {
            result.warning =
                "the control socket's directory is not private to this user (" + reason +
                "). The socket itself is 0600, but on macOS and the BSDs socket permissions are "
                "not enforced on connect(), so the directory is what keeps other local accounts "
                "out. Point --control-socket somewhere private, or fix the directory's mode";
        }
        return result;
    }

    result.path = control_platform_runtime_dir(result.rejection);
    return result;
}

std::string control_socket_path(const std::string& runtime_dir, uint16_t port) {
    if (runtime_dir.empty()) {
        return {};
    }
    return runtime_dir + "/" + CONTROL_SOCKET_PREFIX + std::to_string(port) + CONTROL_SOCKET_SUFFIX;
}

std::string control_socket_absent_reason(const ControlRuntimeDir& runtime,
                                         const std::string& path) {
    if (runtime.path.empty()) {
        if (!runtime.rejection.empty()) {
            return "this host's own per-user directory cannot hold a control socket: " +
                   runtime.rejection +
                   ". Give --control-socket <path> to choose one, or --no-control to stop asking";
        }
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
               " bytes a Unix socket address holds, because the directory it goes in is that "
               "deep. Give --control-socket <a shorter path>, or --no-control";
    }
    return {};
}

bool control_socket_path_fits(const std::string& path) {
    // Room for the terminating NUL too.
    return !path.empty() && path.size() < control_socket_path_limit();
}

size_t control_socket_path_limit() {
    // 104 on macOS and the BSDs, 108 on Linux.
    return sizeof(sockaddr_un::sun_path);
}

}  // namespace sendspin_cli
