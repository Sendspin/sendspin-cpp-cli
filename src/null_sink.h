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

/// Device-less AudioSink: discards PCM, or forwards it raw to stdout.

#pragma once

#include "audio_sink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sendspin_cli {

/// Where NullAudioSink puts the PCM it is handed.
enum class NullSinkOutput {
    Discard,  ///< Count the bytes and drop them (-o null)
    Stdout,   ///< Write raw interleaved PCM to stdout (-o stdout), e.g. for `| aplay`
};

/// An AudioSink that needs no device; consumes instantly, and honours mute but not volume.
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

    /// Total bytes consumed since construction.
    size_t total_bytes() const;

private:
    NullSinkOutput output_;
    std::atomic<size_t> total_bytes_{0};
    /// Frame size, so a short stdout write rounds down to a frame boundary.
    std::atomic<size_t> bytes_per_frame_{0};
    /// Latches once stdout goes bad; the sink then discards.
    std::atomic<bool> stdout_failed_{false};
    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
};

}  // namespace sendspin_cli
