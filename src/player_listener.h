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

/// @file player_listener.h
/// @brief Adapter from the sendspin player role to an AudioSink

#pragma once

#include "audio_sink.h"

#include <sendspin/player_role.h>

#include <cstddef>
#include <cstdint>

namespace sendspin_cli {

/// @brief Connects sendspin's player role to whichever AudioSink -o selected.
///
/// This is the only place that knows about both sides, which is what lets an audio
/// backend be written against AudioSink alone, with no sendspin headers in sight.
class PlayerListener final : public sendspin::PlayerRoleListener {
public:
    /// Also wires sink.on_frames_played to player.notify_audio_played(), so a backend
    /// that can report real playout timing feeds the sync loop through the same seam.
    /// Both references must outlive this listener.
    PlayerListener(sendspin::PlayerRole& player, AudioSink& sink);

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;

private:
    sendspin::PlayerRole& player_;
    AudioSink& sink_;
};

}  // namespace sendspin_cli
