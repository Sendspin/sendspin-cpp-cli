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
#include "state_store.h"

#include <sendspin/player_role.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    ///
    /// @param store Where a server-set volume and mute are written through, so the next run can
    /// restore them -- persisting them is the spec's RECOMMENDED for players, and the library has
    /// no provider hook for either. Null accepts that this run does not remember; the store
    /// itself accepts having nowhere to write, so a caller only passes null to opt out entirely.
    /// Must outlive this listener.
    PlayerListener(sendspin::PlayerRole& player, AudioSink& sink, StateStore* store = nullptr);

    /// Fired after each stream lifecycle change: `true` at stream start, `false` at end.
    ///
    /// The seam main() points --hook-start/--hook-stop at, shaped like AudioSink's
    /// on_frames_played. It fires on the *lifecycle*, not on the format being accepted: a
    /// stream the device refused is audio arriving and being discarded, and the amplifier
    /// the hook exists to switch should be on for exactly as long as `streaming()` is true.
    /// Runs on the main loop, after this listener's own bookkeeping has settled. Null is
    /// fine and is the default.
    std::function<void(bool started)> on_stream_event;

    size_t on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;

    /// @brief Logs a server-set static delay. There is deliberately nothing else here to do.
    ///
    /// Both halves of obeying the value are already the library's. `SyncTask::decode_chunk()`
    /// subtracts `get_effective_static_delay_ms()` from every chunk's client timestamp, which is
    /// what feeds the drift correction -- so the delay is applied to the audio path without this
    /// override existing. And `CliPersistenceProvider` has already written it to the state store
    /// by the time this fires, which is the spec's "clients must persist `static_delay_ms`".
    ///
    /// So this is observability, and saying so is the point: a reader who expects to find the
    /// playout shift here should be told it is upstream rather than go looking for a bug.
    ///
    /// Fires only for a server's own `set_static_delay`. `PlayerRole::update_static_delay()` --
    /// which is what `sendspin-cli delay` reaches -- does not invoke it, so this is not the place
    /// to shadow the value for `status`; that reads the role directly.
    void on_static_delay_changed(uint16_t delay_ms) override;

    /// @brief True between a stream start and its end, whatever the device made of it.
    ///
    /// What the control channel's `status` reports as this endpoint's stream state: audio
    /// arriving *here*, which is a different fact from the group's transport state.
    ///
    /// Deliberately **not** derived from stream_format(): a stream whose format the device
    /// refused has no format and is still a stream, and it is the case where knowing that audio
    /// is arriving matters most -- it is arriving and being discarded. Reporting it as idle would
    /// contradict the ERROR on_stream_start() raises about exactly that.
    bool streaming() const {
        return this->streaming_;
    }

    /// @brief The gain the sink is really applying, 0-100, and whether it is muted.
    ///
    /// This listener is the only caller of `AudioSink::set_volume()`, which makes it the only
    /// thing that knows what the sink was actually told — and is why `status` reports this rather
    /// than `PlayerRole::get_volume()`. The role agrees with it from startup, but only because
    /// `main()` pushes this pair into the role before anything can connect; the role is not the
    /// authority on what the device is doing.
    ///
    /// Read on the main loop, where the two callbacks that write it also fire.
    uint8_t applied_volume() const {
        return this->applied_volume_;
    }

    bool applied_muted() const {
        return this->applied_muted_;
    }

    /// @brief Where the pair above came from, so `status` can say who chose it.
    ///
    /// Without this a server that deliberately set the volume to full, a player nothing has ever
    /// spoken to, and one that restored a remembered figure are all indistinguishable in the
    /// output -- and only one of the three is a number a server asserted.
    VolumeSource volume_source() const {
        return this->volume_source_;
    }

    /// @brief Seeds the sink and the pair above from the state store, without claiming a server
    /// set them.
    ///
    /// Call once at startup, before anything can connect. Goes through here rather than straight
    /// at the sink because this listener is the only thing that talks to `set_volume()`, so it is
    /// the only thing that can keep `applied_volume()` honest about what the sink was told.
    void restore_volume(uint8_t volume, bool muted);

    /// @brief The format the sink was configured for, or nothing when it has none.
    ///
    /// Absent between streams, and absent *during* one the device refused or that arrived with
    /// incomplete parameters -- so `streaming() && !stream_format()` is the refused case rather
    /// than an inconsistency.
    const std::optional<StreamFormat>& stream_format() const {
        return this->stream_format_;
    }

private:
    /// Writes the applied pair through to the store, if there is one. Reports nothing: a store
    /// that cannot be written has already said so at load, and a volume change is not the place
    /// to start warning on every message.
    void persist_volume() const;

    sendspin::PlayerRole& player_;
    AudioSink& sink_;
    StateStore* store_;

    /// Set at stream start before anything can refuse it, cleared at stream end. Read on the
    /// main loop, where the two stream callbacks that write it also fire -- so unlike the
    /// refusal counters below, neither this nor stream_format_ needs an atomic.
    bool streaming_{false};

    /// What the sink was last told, mirroring the two setters below it. Seeded from the sink's own
    /// default rather than from 0, because that is what an untouched sink is really applying.
    uint8_t applied_volume_{DEFAULT_SINK_VOLUME};
    bool applied_muted_{false};
    VolumeSource volume_source_{VolumeSource::SinkDefault};

    /// Set at stream start only once the sink has accepted the format, cleared at stream end.
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
