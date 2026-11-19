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

/// `status-kv`: the status as key=value lines for programs, and the parser that reads it back.

#pragma once

#include "control.h"

#include <cstdint>
#include <optional>
#include <string>

namespace sendspin_cli {

/// The parsed form the TUI reads. One member per key, named as the key; every field defaults to
/// "unknown" (false, empty, 0, nullopt).
struct PlayerView {
    std::string name;
    std::string client_id;
    bool connected{false};
    std::string server_name;
    std::string server_id;
    std::string trust;  ///< `paired`, `unpaired`, or empty
    uint32_t paired_servers{0};
    std::string pairing_server_id;
    std::string pairing_code;
    bool pairing_window{false};
    uint32_t last_pairing_attempt{0};
    std::string last_pairing_outcome;  ///< empty, `paired`, `failed` or `abandoned`
    std::string last_pairing_server_id;
    std::string last_pairing_reason;
    std::string state;  ///< the word the human `state:` line prints
    bool streaming{false};
    std::string codec;
    uint32_t sample_rate{0};
    uint8_t channels{0};
    uint8_t bit_depth{0};
    std::string title;
    std::string artist;
    std::string album;
    std::optional<uint32_t> position_ms{};
    std::optional<uint32_t> duration_ms{};
    bool group_state_known{false};
    uint8_t group_volume{0};
    bool group_muted{false};
    std::string group_name;
    std::string repeat;  ///< `off`, `one`, `all`, or empty
    bool shuffle{false};
    uint8_t player_volume{0};
    bool player_muted{false};
    std::string
        player_volume_source;  ///< `default`, `restored`, `server`, `fixed`, `hardware`, `local`
    uint16_t output_delay_ms{0};
    std::string output;
    std::string input;
    bool input_streaming{false};
};

/// One `key=value` line per field, in a fixed order, `version=1` first. A \r or \n inside a
/// value becomes a space; an unknown or absent value is empty.
std::string format_status_kv(const StatusSnapshot& snapshot);

/// Unknown keys are ignored; missing keys keep their defaults.
/// @return false only when `version` is missing or not 1.
bool parse_status_kv(const std::string& payload, PlayerView& out);

}  // namespace sendspin_cli
