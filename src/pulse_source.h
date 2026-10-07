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

/// AudioSource over a libpulse record stream, timestamped from the server-reported latency.

#pragma once

#include "audio_source.h"
#include "pulse_sink.h"

#include <pulse/pulseaudio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sendspin_cli {

/// An AudioSource that captures from a PulseAudio server; `--input pulse:` takes a source name.
class PulseAudioSource final : public AudioSource {
public:
    /// @param device Source name, or empty for the server's default.
    explicit PulseAudioSource(std::string device);
    ~PulseAudioSource() override;

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

    /// Prints this server's sources, with the default marked.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(pa_stream* stream, void* userdata);
    static void stream_read_cb(pa_stream* stream, size_t nbytes, void* userdata);

    /// Connects if need be and opens the record stream, each wait bounded by `timeout_ms`.
    bool open_(const StreamFormat& format, int timeout_ms, std::string& error);

    /// Checks that the connected server has the named source.
    bool find_source_(std::string& error, int timeout_ms);

    /// Copies out what the server has queued, without waiting.
    /// @return Bytes copied, 0 if nothing is queued, negative once the stream is lost.
    int drain_(uint8_t* data, size_t length, int64_t& capture_time_us);

    /// Declared first so it is destroyed last, after the stream is freed.
    PulseConnection conn_;

    std::string device_;  ///< empty for the server's default

    pa_stream* stream_{nullptr};
    uint32_t rate_{0};
    size_t bytes_per_frame_{0};
    size_t fragment_offset_{0};  ///< bytes of the peeked fragment already returned

    /// Set by the read callback; cleared by read() before it asks the server.
    std::atomic<bool> readable_{false};
    /// Set when the server kills the stream.
    std::atomic<bool> lost_{false};
    std::atomic<int> stream_state_{PA_STREAM_UNCONNECTED};
};

}  // namespace sendspin_cli
