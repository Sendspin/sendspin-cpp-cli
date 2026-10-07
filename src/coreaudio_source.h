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

/// AudioSource over CoreAudio's AUHAL input, with host-time capture timestamps.

#pragma once

#include "audio_source.h"
#include "pcm_ring.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// An AudioSource that captures through CoreAudio: `--input coreaudio:` an index, a device name,
/// or empty for the system default input.
/// The input callback reads the format fields unlocked: change them only while no unit runs.
class CoreAudioSource final : public AudioSource {
public:
    explicit CoreAudioSource(std::string device);
    ~CoreAudioSource() override;

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

    /// Prints this host's CoreAudio input devices.
    static void list_devices(std::FILE* out);

private:
    static OSStatus input_callback(void* user_data, AudioUnitRenderActionFlags* flags,
                                   const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                   AudioBufferList* data);

    /// Runs off a HAL thread and only ever sets flags for read().
    static OSStatus property_listener(AudioObjectID object, UInt32 count,
                                      const AudioObjectPropertyAddress* addresses, void* user_data);

    /// Removal does not wait an in-flight notification out; close() runs long before destruction.
    void add_listeners_(AudioDeviceID device);
    void remove_listeners_();

    /// The device as --input spelled it, resolved per open.
    std::string device_;

    AudioUnit unit_{nullptr};
    AudioDeviceID device_id_{kAudioObjectUnknown};
    StreamFormat format_{};
    size_t bytes_per_frame_{0};
    /// Mach ticks per microsecond, from mach_timebase_info().
    double host_ticks_per_us_{0.0};
    /// Input path latency the callback's timestamp does not carry, in microseconds.
    int64_t input_latency_us_{0};

    /// The callback is the producer, read() the consumer.
    PcmRingBuffer ring_;
    /// What AudioUnitRender() fills; sized at open so the callback never allocates.
    std::vector<uint8_t> render_buffer_;
    /// Frames put in the ring since open. Callback only.
    uint64_t frames_captured_{0};
    /// Frames taken out of the ring since open. Capture thread only.
    uint64_t frames_read_{0};
    /// Steady-clock time of this open's frame 0; every callback republishes it.
    std::atomic<int64_t> epoch_us_{0};
    /// Frames the callback had no ring space for; read() reports and clears it.
    std::atomic<uint64_t> frames_dropped_{0};

    /// Only parks read(); the callback notifies without taking it.
    std::mutex wait_mutex_;
    std::condition_variable data_ready_;

    /// Set by the property listener when the open device dies or changes its rate.
    std::atomic<bool> device_lost_{false};
    /// Set by the property listener when the system default input moves.
    std::atomic<bool> default_moved_{false};
    /// Whether each listener is registered, so each is removed exactly once.
    bool listening_alive_{false};
    bool listening_rate_{false};
    bool listening_default_{false};
    AudioDeviceID listening_device_{kAudioObjectUnknown};
};

}  // namespace sendspin_cli
