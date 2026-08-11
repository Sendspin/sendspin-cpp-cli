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
#include "control.h"

#include <sendspin/player_role.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin_cli {

/// @brief Connects sendspin's player role to whichever AudioSink -o selected.
///
/// This is the only place that knows about both sides, which is what lets an audio
/// backend be written against AudioSink alone, with no sendspin headers in sight.
///
/// THREAD SAFETY: on_audio_write() is called on the sendspin sync task's background thread,
/// every other callback on the main loop -- the same split AudioSink documents, since this
/// listener is what those calls arrive through. The refusal bookkeeping below is therefore
/// atomic: it is written on one thread and read on the other.
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

    /// @brief The format the sink is currently configured for, or nothing between streams.
    ///
    /// What the control channel's `status` reports as this endpoint's stream state: audio
    /// arriving *here*, which is a different fact from the group's transport state. Read on the
    /// main loop, where the two stream callbacks that write it also fire -- so unlike the
    /// refusal counters below this needs no atomic.
    const std::optional<StreamFormat>& stream_format() const {
        return this->stream_format_;
    }

private:
    sendspin::PlayerRole& player_;
    AudioSink& sink_;

    /// Set at stream start once the sink has accepted the format, cleared at stream end.
    std::optional<StreamFormat> stream_format_;

    /// Set when configure() refuses this stream's format, cleared at the next stream start.
    ///
    /// Without it a refusal is one ERROR line and then silence -- the sink discards
    /// internally and write() still reports the bytes as consumed, so nothing downstream
    /// notices that a whole track played to nowhere.
    std::atomic<bool> stream_refused_{false};
    /// Bytes handed over while stream_refused_ was set. Only what this listener passed to a
    /// sink it knows was refused: a sink discarding for its own reasons is its own to report.
    std::atomic<uint64_t> refused_bytes_{0};
};

}  // namespace sendspin_cli
