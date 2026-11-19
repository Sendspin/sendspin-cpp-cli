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
#include <mutex>
#include <optional>
#include <span>

namespace sendspin_cli {

/// Connects sendspin's player role to whichever AudioSink -o selected.
/// on_audio_output() runs on the sync task's thread, every other method on the main loop.
/// The sink opens in each write's format; configure() stays on the main loop, in poll().
class PlayerListener final : public sendspin::PlayerRoleListener {
public:
    /// Wires sink.on_frames_played to player.notify_audio_played(); arguments must outlive this,
    /// and the sink must be stopped before this is destroyed, since its reports reach this.
    /// @param store Where server-set volume and mute persist; null opts out.
    PlayerListener(sendspin::PlayerRole& player, AudioSink& sink, StateStore* store = nullptr);

    /// Fired on the main loop at stream start (true) and end (false), refused formats included.
    std::function<void(bool started)> on_stream_event;

    /// Writes in the open format; a new format returns 0 until poll() has reopened the sink.
    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                           uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;

    /// Logs a server-set output delay; the library already applies and persists it.
    void on_output_delay_changed(uint16_t delay_ms) override;

    /// Reopens the sink in a format a write asked for, once the old format's audio has played.
    /// @param now_us SendspinClient::now_us(), the clock on_frames_played timestamps are in.
    void poll(int64_t now_us);

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

    /// Records the level the sink read from its hardware mixer, marking the pair hardware-sourced.
    void seed_volume(uint8_t volume, bool muted);

    /// Pins the sink at full gain, unmuted, and ignores volume and mute from then on.
    void fix_volume();

    /// Records and publishes a level the sink applied itself, e.g. a mixer knob. Main loop only.
    void on_sink_volume_changed(uint8_t volume, bool muted);

    /// Applies a level asked for on this player, publishes it, and persists it. Main loop only.
    /// @return false under --fixed-volume, changing nothing.
    bool set_local_volume(uint8_t volume);
    bool set_local_muted(bool muted);

    /// Closes the sink's device at each stream end, so other players can open it between streams.
    void release_device_between_streams() {
        this->release_device_ = true;
    }

    /// The format the sink is open in; absent between streams and for a refused one.
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
    bool release_device_{false};

    /// What the sink was last told; starts at the sink's own default.
    uint8_t applied_volume_{DEFAULT_SINK_VOLUME};
    bool applied_muted_{false};
    VolumeSource volume_source_{VolumeSource::SinkDefault};

    /// Configures the sink for `format` and publishes the result to the sync thread.
    void open_sink(const sendspin::AudioFormat& format);

    /// Forgets the open format, at stream end.
    void reset_format();

    /// Mirror of open_ for `status`; main loop only.
    std::optional<StreamFormat> stream_format_;

    /// The format configure() last ran for, and whether the sink refused it.
    struct OpenFormat {
        sendspin::AudioFormat format;
        bool refused{false};
    };

    /// Guards open_ and requested_, the hand-off between the sync thread and poll().
    std::mutex format_mutex_;
    std::optional<OpenFormat> open_;
    /// A format a write is waiting on; poll() opens it.
    std::optional<sendspin::AudioFormat> requested_;
    /// When poll() first saw requested_, in now_us; bounds the wait for the old audio to play.
    std::optional<int64_t> requested_since_us_;

    /// Frames of the open format the sink took, and frames it reported played since the last open.
    std::atomic<uint64_t> written_frames_{0};
    std::atomic<uint64_t> played_frames_{0};
    /// The latest finish time the sink reported, in now_us.
    std::atomic<int64_t> playout_end_us_{0};

    /// Bytes of refused formats handed to the sink this stream.
    std::atomic<uint64_t> refused_bytes_{0};
};

}  // namespace sendspin_cli
