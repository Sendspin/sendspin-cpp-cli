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

/// @file alsa_sink.h
/// @brief AudioSink over ALSA snd_pcm, with snd_pcm_delay()-based sync feedback

#pragma once

#include "audio_sink.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief An AudioSink that plays through an ALSA PCM device.
///
/// The device string is an ALSA PCM name exactly as `aplay -L` prints it -- `default`,
/// `pipewire`, `hw:2,0`, `plughw:1,0`, `hdmi:CARD=NVidia,DEV=0`. Anything `-o` does not
/// reserve for the device-less sinks is handed here, which is squeezelite's model.
///
/// THREAD SAFETY: an snd_pcm_t is not thread-safe, so every snd_pcm_* call is made under
/// device_mutex_. write() runs on the sync task's background thread while configure(),
/// clear() and stop() run on the main loop, so the mutex is what keeps a shutdown from
/// closing the handle mid-write.
///
/// The mutex alone would let a blocked write() stall shutdown for as long as the device
/// takes to drain, so write() never blocks unboundedly while holding it: it waits for
/// room in short slices (snd_pcm_wait) and re-checks the abort flag between them. That is
/// the write_mutex_ + abort_write_ pairing upstream's PortAudio sink uses.
///
/// Volume is applied in software (Q32 fixed-point, quadratic taper), mirroring upstream's
/// PortAudioSink::apply_volume_(). The ALSA hardware mixer is deliberately not used: the
/// default device here is usually PipeWire's ALSA plugin, where a hardware mixer element
/// is either absent or controls something other than this stream.
class AlsaAudioSink final : public AudioSink {
public:
    explicit AlsaAudioSink(std::string device);
    ~AlsaAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// @brief Checks that `device` names a PCM this host can open, without keeping it.
    ///
    /// Called from make_audio_sink() so a typo in -o fails at startup rather than at the
    /// first stream. A device that exists but is currently busy (-EBUSY, an exclusive
    /// `hw:` PCM someone else holds) deliberately passes: that is a transient condition
    /// for configure() to report, not a reason to refuse to start.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @return true if the device name resolves to a playback PCM.
    static bool probe(const std::string& device, std::string& error);

    /// @brief Prints the host's playback PCMs, as `aplay -L` does. Backs part of -l.
    static void list_devices(std::FILE* out);

private:
    /// Opens the device for a format. Caller holds device_mutex_.
    bool open_device_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    /// Closes the device if open. Caller holds device_mutex_. Idempotent.
    void close_device_();
    /// Handles -EPIPE/-ESTRPIPE/-EINTR from a PCM call. Caller holds device_mutex_.
    /// @return true if the stream was recovered and the operation can be retried.
    bool recover_(int err);
    /// Recomputes volume_multiplier_ from volume_ and muted_.
    void update_volume_multiplier_();

    std::string device_;

    /// Serialises every snd_pcm_* call and the fields describing the open stream.
    std::mutex device_mutex_;
    snd_pcm_t* pcm_{nullptr};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    /// Scratch for volume-scaled samples, since write()'s input buffer is const. Grown to
    /// the largest write seen and reused, so steady-state playback does not allocate.
    std::vector<uint8_t> scaled_;

    /// Set before stop() takes the mutex, so an in-flight write() bails out promptly
    /// instead of making shutdown wait for the device to drain.
    std::atomic<bool> stopping_{false};
    /// Latches once the device is unusable, so write() discards instead of returning 0
    /// forever and spinning the sync task. Same degrade-don't-stall rule as NullAudioSink.
    std::atomic<bool> failed_{false};

    std::atomic<uint8_t> volume_{100};
    std::atomic<bool> muted_{false};
    /// Q32 fixed-point gain: 1<<32 is unity, 0 is silence. Read on the audio thread.
    std::atomic<uint64_t> volume_multiplier_;
};

}  // namespace sendspin_cli
