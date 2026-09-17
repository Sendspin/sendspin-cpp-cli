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

/// AudioSink over ALSA snd_pcm, with snd_pcm_delay()-based sync feedback.

#pragma once

#include "audio_sink.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// An AudioSink that plays through an ALSA PCM, e.g. `default` or `hw:2,0`.
/// Every snd_pcm_* call holds device_mutex_. write() never reopens a dead device; poll() does.
class AlsaAudioSink final : public AudioSink {
public:
    /// @param buffer_ms Ring size request, already range-checked; ALSA rounds it to the card.
    AlsaAudioSink(std::string device, uint32_t buffer_ms);
    ~AlsaAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Reopens a device that died mid-stream once SinkRecovery's delay is up.
    void poll(int64_t now_ms) override;

    /// What this PCM will take; opens the device briefly.
    SinkCapabilities capabilities() const override;

    /// Checks that `device` names a playback PCM this host can open; a busy device passes.
    static bool probe(const std::string& device, std::string& error);

    /// Prints the host's playback PCMs, as `aplay -L` does.
    static void list_devices(std::FILE* out);

private:
    /// Opens the device for a format. Caller holds device_mutex_.
    bool open_device_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    /// Closes the device if open. Caller holds device_mutex_. Idempotent.
    void close_device_();
    /// Recovers a PCM error in place or retires a lost device. Caller holds device_mutex_.
    /// @return true if the operation can be retried; on false pcm_ may already be closed.
    bool recover_(int err);
    /// Retires a lost device and arms poll() to get it back. Caller holds device_mutex_.
    /// @return false always.
    bool handle_device_loss_();
    void update_target_multiplier_();

    std::string device_;
    /// Ring size to ask ALSA for, in milliseconds; the period is this over PERIODS_PER_BUFFER.
    uint32_t buffer_ms_;

    /// Serialises every snd_pcm_* call and the fields describing the open stream.
    std::mutex device_mutex_;
    snd_pcm_t* pcm_{nullptr};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    /// The format last announced; survives close_device_() so poll() can reopen at it.
    StreamFormat last_format_{};
    /// Scratch for volume-scaled samples, reused across writes.
    std::vector<uint8_t> scaled_;

    /// Set before stop() takes the mutex, so an in-flight write() bails out promptly.
    std::atomic<bool> stopping_{false};
    /// Latches once the device is unusable, so write() discards instead of spinning.
    std::atomic<bool> failed_{false};

    /// Guarded by device_mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written from the main loop without device_mutex_.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied, walked toward target_multiplier_; guarded by device_mutex_.
    std::uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
