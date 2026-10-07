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

/// AudioSource over a libpipewire capture stream, timestamped from the graph's reported delay.

#pragma once

#include "audio_source.h"
#include "pcm_ring.h"
#include "pipewire_sink.h"

#include <pipewire/pipewire.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace sendspin_cli {

/// An AudioSource that captures from a PipeWire graph; `--input pipewire:` a node name or empty.
/// process() writes the ring and reads the format fields unlocked: change them only while closed.
class PipeWireAudioSource final : public AudioSource {
public:
    /// @param device Source node name, or empty for the graph's default.
    explicit PipeWireAudioSource(std::string device);
    ~PipeWireAudioSource() override;

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

    /// Prints this graph's audio source nodes.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(void* userdata, enum pw_stream_state old,
                                enum pw_stream_state state, const char* error);
    static void stream_process_cb(void* userdata);

    /// Checks that a daemon answers and has the named node.
    bool find_node_(std::string& error) const;

    /// Declared first so it is destroyed last, after the stream and its loop.
    PipeWireGuard pw_;

    std::string device_;  ///< empty for the graph's default

    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};
    PcmRingBuffer ring_;
    uint32_t rate_{0};
    size_t bytes_per_frame_{0};

    uint64_t frames_written_{0};  ///< process() only
    uint64_t frames_read_{0};     ///< read() only
    /// Steady-clock time of this stream's frame 0 as process() last measured it; 0 until it has.
    std::atomic<int64_t> origin_us_{0};

    /// Set when the graph errors or drops the stream.
    std::atomic<bool> lost_{false};
    std::atomic<int> stream_state_{PW_STREAM_STATE_UNCONNECTED};

    std::mutex wait_mutex_;
    /// Signalled by process(), without wait_mutex_, once it has added to the ring.
    std::condition_variable data_ready_;
};

}  // namespace sendspin_cli
