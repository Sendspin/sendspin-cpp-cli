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

#include "player_listener.h"

#include "log.h"

#include <sendspin/client.h>

namespace sendspin_cli {

using sendspin::LogLevel;

PlayerListener::PlayerListener(sendspin::PlayerRole& player, AudioSink& sink)
    : player_(player), sink_(sink) {
    // Capture the address by value: capturing the `player` reference parameter itself
    // would leave the callback holding a reference into this constructor's frame.
    this->sink_.on_frames_played = [target = &player](uint32_t frames, int64_t timestamp) {
        target->notify_audio_played(frames, timestamp);
    };
}

size_t PlayerListener::on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) {
    return this->sink_.write(data, length, timeout_ms);
}

void PlayerListener::on_stream_start() {
    const sendspin::ServerPlayerStreamObject& params = this->player_.get_current_stream_params();
    if (!params.is_complete()) {
        // The sink cannot open a device without a format. The player keeps the stream
        // alive, so a later start with complete params still works.
        cli_log(LogLevel::WARN, "Stream started without complete parameters -- sink not configured");
        return;
    }

    cli_log(LogLevel::INFO, "Stream started");
    if (!this->sink_.configure(*params.sample_rate, *params.channels, *params.bit_depth)) {
        cli_log(LogLevel::ERROR, "Output device rejected %u Hz / %u ch / %u-bit",
                *params.sample_rate, *params.channels, *params.bit_depth);
    }
}

void PlayerListener::on_stream_end() {
    cli_log(LogLevel::INFO, "Stream ended");
    this->sink_.clear();
}

void PlayerListener::on_volume_changed(uint8_t volume) {
    this->sink_.set_volume(volume);
}

void PlayerListener::on_mute_changed(bool muted) {
    this->sink_.set_muted(muted);
}

}  // namespace sendspin_cli
