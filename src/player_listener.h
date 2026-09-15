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

/// Adapter from the sendspin player role to an AudioSink.

#pragma once

#include "audio_sink.h"
#include "state_store.h"

#include <sendspin/player_role.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace sendspin_cli {

/// Connects sendspin's player role to whichever AudioSink -o selected.
/// on_audio_write() runs on the sync task's thread, every other callback on the main loop.
class PlayerListener final : public sendspin::PlayerRoleListener {
public:
    /// Wires sink.on_frames_played to player.notify_audio_played(); arguments must outlive this.
    /// @param store Where server-set volume and mute persist; null opts out.
    PlayerListener(sendspin::PlayerRole& player, AudioSink& sink, StateStore* store = nullptr);

    /// Fired on the main loop at stream start (true) and end (false), refused formats included.
    std::function<void(bool started)> on_stream_event;

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;

    /// Logs a server-set static delay; the library already applies and persists it.
    void on_static_delay_changed(uint16_t delay_ms) override;

    /// True between a stream start and its end, even when the device refused the format.
    bool streaming() const {
        return this->streaming_;
    }

    /// The gain the sink is really applying, 0-100. Main loop only.
    uint8_t applied_volume() const {
        return this->applied_volume_;
    }

    bool applied_muted() const {
        return this->applied_muted_;
    }

    /// Where the applied volume and mute came from, for `status`.
    VolumeSource volume_source() const {
        return this->volume_source_;
    }

    /// Seeds the sink from the state store without marking it server-set. Call before connecting.
    void restore_volume(uint8_t volume, bool muted);

    /// The format the sink was configured for; absent between streams and for a refused one.
    const std::optional<StreamFormat>& stream_format() const {
        return this->stream_format_;
    }

private:
    /// Writes the applied pair to the store, if any, reporting nothing.
    void persist_volume() const;

    sendspin::PlayerRole& player_;
    AudioSink& sink_;
    StateStore* store_;

    /// Main loop only, so not atomic.
    bool streaming_{false};

    /// What the sink was last told; starts at the sink's own default.
    uint8_t applied_volume_{DEFAULT_SINK_VOLUME};
    bool applied_muted_{false};
    VolumeSource volume_source_{VolumeSource::SinkDefault};

    /// Set at stream start once the sink accepts the format, cleared at stream end.
    std::optional<StreamFormat> stream_format_;

    /// Set when configure() refuses this stream's format, until the next stream start.
    std::atomic<bool> stream_refused_{false};
    /// Bytes handed to a sink while stream_refused_ was set.
    std::atomic<uint64_t> refused_bytes_{0};
};

}  // namespace sendspin_cli
