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

/// @file null_sink.h
/// @brief Device-less AudioSink: discards PCM, or forwards it raw to stdout

#pragma once

#include "audio_sink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sendspin_cli {

/// @brief Where NullAudioSink puts the PCM it is handed.
enum class NullSinkOutput {
    Discard,  ///< Count the bytes and drop them (-o null)
    Stdout,   ///< Write raw interleaved PCM to stdout (-o stdout), e.g. for `| aplay`
};

/// @brief An AudioSink that needs no audio device.
///
/// The point of this sink is that `sendspin-cli` runs, and can be verified end to end,
/// on a host or container with no sound card at all. It is also the reference
/// implementation of the AudioSink contract for the real backends to follow.
///
/// Timing: this sink consumes everything immediately rather than at the stream's real
/// rate, so it does not pace playback and deliberately leaves on_frames_played unset.
///
/// Volume and mute: mute is honoured by emitting zeroed samples, which is silence for
/// the signed-integer PCM the player advertises. Volume is only recorded and logged --
/// scaling samples correctly per bit depth is a real backend's job.
class NullAudioSink final : public AudioSink {
public:
    explicit NullAudioSink(NullSinkOutput output);

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// @brief Total bytes consumed since construction. Reported at shutdown.
    size_t total_bytes() const;

private:
    NullSinkOutput output_;
    std::atomic<size_t> total_bytes_{0};
    /// Frame size for the active stream, so a short stdout write can be rounded down to
    /// a frame boundary as the AudioSink::write() contract requires.
    std::atomic<size_t> bytes_per_frame_{0};
    /// Latches once stdout goes bad (a closed downstream pipe): the sink then behaves
    /// like Discard instead of stalling the sync task on every write.
    std::atomic<bool> stdout_failed_{false};
    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
};

}  // namespace sendspin_cli
