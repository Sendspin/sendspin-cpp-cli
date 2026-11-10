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

#include "status_kv.h"

#include <charconv>
#include <functional>
#include <limits>
#include <unordered_map>

namespace sendspin_cli {
namespace {

/// A \r or \n in a value would start a new key, so both read as a space.
void append_kv(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += '=';
    for (const char c : value) {
        out += (c == '\r' || c == '\n') ? ' ' : c;
    }
    out += '\n';
}

std::string flag(bool value) {
    return value ? "1" : "0";
}

std::string number(uint32_t value) {
    return std::to_string(value);
}

std::string number(std::optional<uint32_t> value) {
    return value.has_value() ? number(*value) : std::string();
}

const char* outcome_name(PairingOutcome outcome) {
    switch (outcome) {
        case PairingOutcome::Paired:
            return "paired";
        case PairingOutcome::Failed:
            return "failed";
        case PairingOutcome::Abandoned:
            return "abandoned";
        case PairingOutcome::None:
            break;
    }
    return "";
}

const char* volume_source_name(VolumeSource source) {
    switch (source) {
        case VolumeSource::Restored:
            return "restored";
        case VolumeSource::Server:
            return "server";
        case VolumeSource::Fixed:
            return "fixed";
        case VolumeSource::Hardware:
            return "hardware";
        case VolumeSource::Local:
            return "local";
        case VolumeSource::SinkDefault:
            break;
    }
    return "default";
}

const char* repeat_name(sendspin::SendspinRepeatMode mode) {
    switch (mode) {
        case sendspin::SendspinRepeatMode::ONE:
            return "one";
        case sendspin::SendspinRepeatMode::ALL:
            return "all";
        case sendspin::SendspinRepeatMode::OFF:
            break;
    }
    return "off";
}

/// Reads an unsigned decimal that fits T; anything else leaves `out` as it was.
template <typename T>
void parse_number(const std::string& value, T& out) {
    uint32_t parsed = 0;
    const char* end = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), end, parsed);
    if (error == std::errc() && stop == end && parsed <= std::numeric_limits<T>::max()) {
        out = static_cast<T>(parsed);
    }
}

}  // namespace

std::string format_status_kv(const StatusSnapshot& snapshot) {
    std::string out;
    const bool has_format = snapshot.format.has_value();
    const PairingAttemptResult& last = snapshot.last_pairing;

    append_kv(out, "version", "1");
    append_kv(out, "name", snapshot.name);
    append_kv(out, "client_id", snapshot.client_id);
    append_kv(out, "connected", flag(snapshot.connected));
    append_kv(out, "server_name", snapshot.server_name);
    append_kv(out, "server_id", snapshot.server_id);
    append_kv(out, "trust",
              !snapshot.connected || !snapshot.trust.has_value()
                  ? ""
                  : (*snapshot.trust == sendspin::ConnectionTrust::USER ? "paired" : "unpaired"));
    append_kv(out, "paired_servers", number(static_cast<uint32_t>(snapshot.paired_servers)));
    append_kv(out, "pairing_server_id", snapshot.pairing_server_id);
    append_kv(out, "pairing_code", snapshot.pairing_code);
    append_kv(out, "pairing_window", flag(snapshot.pairing_window_open));
    append_kv(out, "last_pairing_attempt", number(last.attempt));
    append_kv(out, "last_pairing_outcome", outcome_name(last.outcome));
    append_kv(out, "last_pairing_server_id", last.server_id);
    append_kv(out, "last_pairing_reason", last.reason);
    append_kv(out, "state", status_state(snapshot));
    append_kv(out, "streaming", flag(snapshot.streaming));
    append_kv(out, "codec", snapshot.codec.has_value() ? codec_name(*snapshot.codec) : "");
    append_kv(out, "sample_rate", has_format ? number(snapshot.format->sample_rate) : "");
    append_kv(out, "channels", has_format ? number(snapshot.format->channels) : "");
    append_kv(out, "bit_depth", has_format ? number(snapshot.format->bit_depth) : "");
    append_kv(out, "title", snapshot.title);
    append_kv(out, "artist", snapshot.artist);
    append_kv(out, "album", snapshot.album);
    append_kv(out, "position_ms", number(snapshot.progress_ms));
    append_kv(out, "duration_ms", number(snapshot.duration_ms));
    append_kv(out, "group_state_known", flag(snapshot.group_state_known));
    append_kv(out, "group_volume", snapshot.group_state_known ? number(snapshot.group_volume) : "");
    append_kv(out, "group_muted", snapshot.group_state_known ? flag(snapshot.group_muted) : "");
    append_kv(out, "group_name", snapshot.group_name);
    append_kv(out, "repeat", snapshot.group_state_known ? repeat_name(snapshot.group_repeat) : "");
    append_kv(out, "shuffle", snapshot.group_state_known ? flag(snapshot.group_shuffle) : "");
    append_kv(out, "player_volume", number(snapshot.player_volume));
    append_kv(out, "player_muted", flag(snapshot.player_muted));
    append_kv(out, "player_volume_source", volume_source_name(snapshot.player_volume_source));
    append_kv(out, "output_delay_ms", number(snapshot.output_delay_ms));
    append_kv(out, "output", snapshot.output);
    append_kv(out, "input", snapshot.input);
    append_kv(out, "input_streaming", flag(snapshot.input_streaming));
    return out;
}

bool parse_status_kv(const std::string& payload, PlayerView& out) {
    using Setter = std::function<void(PlayerView&, const std::string&)>;
    const auto text = [](std::string PlayerView::* member) -> Setter {
        return [member](PlayerView& view, const std::string& value) { view.*member = value; };
    };
    const auto boolean = [](bool PlayerView::* member) -> Setter {
        return
            [member](PlayerView& view, const std::string& value) { view.*member = value == "1"; };
    };
    const auto integer = [](auto PlayerView::* member) -> Setter {
        return [member](PlayerView& view, const std::string& value) {
            parse_number(value, view.*member);
        };
    };
    // Empty is "unknown", which is not the same as 0.
    const auto optional_integer = [](std::optional<uint32_t> PlayerView::* member) -> Setter {
        return [member](PlayerView& view, const std::string& value) {
            std::optional<uint32_t> parsed;
            if (!value.empty()) {
                uint32_t number = 0;
                parse_number(value, number);
                parsed = number;
            }
            view.*member = parsed;
        };
    };

    static const std::unordered_map<std::string, Setter> setters = {
        {"name", text(&PlayerView::name)},
        {"client_id", text(&PlayerView::client_id)},
        {"connected", boolean(&PlayerView::connected)},
        {"server_name", text(&PlayerView::server_name)},
        {"server_id", text(&PlayerView::server_id)},
        {"trust", text(&PlayerView::trust)},
        {"paired_servers", integer(&PlayerView::paired_servers)},
        {"pairing_server_id", text(&PlayerView::pairing_server_id)},
        {"pairing_code", text(&PlayerView::pairing_code)},
        {"pairing_window", boolean(&PlayerView::pairing_window)},
        {"last_pairing_attempt", integer(&PlayerView::last_pairing_attempt)},
        {"last_pairing_outcome", text(&PlayerView::last_pairing_outcome)},
        {"last_pairing_server_id", text(&PlayerView::last_pairing_server_id)},
        {"last_pairing_reason", text(&PlayerView::last_pairing_reason)},
        {"state", text(&PlayerView::state)},
        {"streaming", boolean(&PlayerView::streaming)},
        {"codec", text(&PlayerView::codec)},
        {"sample_rate", integer(&PlayerView::sample_rate)},
        {"channels", integer(&PlayerView::channels)},
        {"bit_depth", integer(&PlayerView::bit_depth)},
        {"title", text(&PlayerView::title)},
        {"artist", text(&PlayerView::artist)},
        {"album", text(&PlayerView::album)},
        {"position_ms", optional_integer(&PlayerView::position_ms)},
        {"duration_ms", optional_integer(&PlayerView::duration_ms)},
        {"group_state_known", boolean(&PlayerView::group_state_known)},
        {"group_volume", integer(&PlayerView::group_volume)},
        {"group_muted", boolean(&PlayerView::group_muted)},
        {"group_name", text(&PlayerView::group_name)},
        {"repeat", text(&PlayerView::repeat)},
        {"shuffle", boolean(&PlayerView::shuffle)},
        {"player_volume", integer(&PlayerView::player_volume)},
        {"player_muted", boolean(&PlayerView::player_muted)},
        {"player_volume_source", text(&PlayerView::player_volume_source)},
        {"output_delay_ms", integer(&PlayerView::output_delay_ms)},
        {"output", text(&PlayerView::output)},
        {"input", text(&PlayerView::input)},
        {"input_streaming", boolean(&PlayerView::input_streaming)},
    };

    PlayerView view;
    bool version_ok = false;
    size_t position = 0;
    while (position < payload.size()) {
        size_t line_end = payload.find('\n', position);
        if (line_end == std::string::npos) {
            line_end = payload.size();
        }
        std::string line = payload.substr(position, line_end - position);
        position = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "version") {
            version_ok = value == "1";
        } else if (const auto it = setters.find(key); it != setters.end()) {
            it->second(view, value);
        }
    }

    if (!version_ok) {
        return false;
    }
    out = std::move(view);
    return true;
}

}  // namespace sendspin_cli
