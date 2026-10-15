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

/// AudioSource over a PortAudio input stream, with ADC-time capture timestamps.

#pragma once

#include "audio_source.h"
#include "pcm_ring.h"
#include "portaudio_device.h"

#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace sendspin_cli {

/// An AudioSource that captures through PortAudio: `--input portaudio:` an index, a device name,
/// or empty for the default input. Construct and destroy on the main loop thread only.
/// The callback reads the format fields unlocked: change them only while no stream runs.
class PortAudioSource final : public AudioSource {
public:
    explicit PortAudioSource(std::string device);
    ~PortAudioSource() override;

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

    /// Prints this host's PortAudio input devices.
    static void list_devices(std::FILE* out);

private:
    static int pa_callback(const void* input, void* output, unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags, void* user_data);

    /// Declared first so it is destroyed last, after the stream is closed.
    PortAudioGuard pa_;

    /// The device as --input spelled it, resolved per open.
    std::string device_;

    PaStream* stream_{nullptr};
    uint32_t rate_{0};
    size_t bytes_per_frame_{0};

    /// The callback is the producer, read() the consumer.
    PcmRingBuffer ring_;
    /// Frames put in the ring since open. Callback only.
    uint64_t frames_captured_{0};
    /// Frames taken out of the ring since open. Capture thread only.
    uint64_t frames_read_{0};
    /// Steady-clock time of this open's frame 0, or 0 when PortAudio gives no ADC time.
    std::atomic<int64_t> epoch_us_{0};
    /// Steady-clock time of the callback's last run; 0 until the first.
    std::atomic<int64_t> last_callback_us_{0};
    /// Frames the callback had no ring space for; read() reports and clears it.
    std::atomic<uint64_t> frames_dropped_{0};
    /// Steady-clock time of the open, for the stall check before the first callback.
    int64_t opened_us_{0};

    /// Only parks read(); the callback notifies without taking it.
    std::mutex wait_mutex_;
    std::condition_variable data_ready_;
};

}  // namespace sendspin_cli
