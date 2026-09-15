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

/// AudioSink over PortAudio, with callback DAC-time sync feedback.

#pragma once

#include "audio_sink.h"
#include "pcm_ring.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace sendspin_cli {

/// Holds one reference-counted Pa_Initialize()/Pa_Terminate() pair.
/// Construct and destroy on the main loop thread only.
class PortAudioGuard {
public:
    PortAudioGuard();
    ~PortAudioGuard();

    PortAudioGuard(const PortAudioGuard&) = delete;
    PortAudioGuard& operator=(const PortAudioGuard&) = delete;

    /// True if PortAudio came up. When false, no other Pa_* call will work.
    bool ok() const;

    /// Terminates and reinitializes PortAudio so a replugged device is enumerated.
    /// Invalidates every PaDeviceIndex and needs any stream closed first. Main loop only.
    /// @return true if PortAudio came back up.
    bool reinitialize();

    /// Why initialization failed. Only meaningful when ok() is false.
    const char* error() const;

private:
    PaError err_;
};

/// An AudioSink that plays through PortAudio: `-o portaudio:` an index, a device name, or empty.
/// The callback reads format fields unlocked: change them only while no stream is running.
class PortAudioSink final : public AudioSink {
public:
    /// @param buffer_ms Ring size request; the device-latency and minimum-ring floors win.
    PortAudioSink(std::string device, uint32_t buffer_ms);
    ~PortAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Makes the delayed device rescan once a dead stream has escalated; blocks while it runs.
    void poll(int64_t now_ms) override;

    /// What the resolved device will take; not free, since probing can open the PCM.
    SinkCapabilities capabilities() const override;

    /// Checks that `device` names exactly one output device on this host; no format is tested.
    static bool probe(const std::string& device, std::string& error);

    /// Prints this host's PortAudio output devices.
    static void list_devices(std::FILE* out);

private:
    static int pa_callback(const void* input, void* output, unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags, void* user_data);

    /// Opens and starts a stream. Caller holds mutex_ and has closed any previous stream.
    bool open_stream_(PaDeviceIndex device, uint32_t sample_rate, uint8_t channels,
                      uint8_t bits_per_sample);
    /// Stops and closes the stream and forgets the format. Caller holds mutex_.
    /// Must not touch stopping_, or a format change would un-latch a shutdown.
    void close_stream_();
    /// Restarts the open stream from an empty ring. Caller holds mutex_; stream_ is not null.
    /// @return true if the stream is running again.
    bool restart_stream_();
    /// The one in-place reopen a dead stream gets, from write(). Caller holds mutex_.
    /// @return true if a stream is running again.
    bool reopen_in_place_();
    /// True while the open stream is still being driven by PortAudio. Caller holds mutex_.
    bool stream_alive_() const;
    /// Ring size in bytes. Caller holds mutex_ and the format fields are set.
    size_t ring_capacity_(double device_latency_s) const;
    void update_target_multiplier_();

    /// Declared first so it is destroyed last, after the stream is closed.
    PortAudioGuard pa_;

    /// The device as -o spelled it, resolved per stream.
    std::string device_;
    /// Ring size to aim for, in milliseconds, before the floors in ring_capacity_() apply.
    uint32_t buffer_ms_;

    /// Serialises stream_, the format fields, and the ring buffer's producer side.
    std::mutex mutex_;
    /// Signalled by the audio callback once it has drained a buffer's worth of the ring.
    std::condition_variable space_available_;
    PcmRingBuffer ring_;
    PaStream* stream_{nullptr};
    PaDeviceIndex device_index_{paNoDevice};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    /// Read by the audio callback unlocked; see the class comment.
    size_t bytes_per_frame_{0};
    /// The rate PortAudio really opened. Read by the audio callback.
    double stream_rate_{0.0};
    /// Per-frame gain increment from volume_ramp_step(); 0 means no ramp. Read by the callback.
    uint64_t ramp_step_{0};

    /// Bumped on open, close, restart or flush; a write() that waited compares it on waking.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex so a blocked write() bails out; latches.
    std::atomic<bool> stopping_{false};
    /// Latches so write() complains once about a missing stream; cleared by a good configure().
    std::atomic<bool> failed_{false};

    /// Format last asked for, even if its open failed; recovery reopens at it. Guarded by mutex_.
    StreamFormat last_format_{};
    /// Guarded by mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written by the main loop, read by the callback.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied; only the callback advances it. clear() must not touch it.
    std::uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
