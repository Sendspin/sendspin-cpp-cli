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

/// Device-less AudioSource: silence or a test tone, paced by the clock.

#pragma once

#include "audio_source.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sendspin_cli {

/// What NullAudioSource captures.
enum class NullSourceSignal {
    Silence,  ///< --input null
    Tone,     ///< --input tone: a 440 Hz sine at a quarter of full scale
};

/// An AudioSource that needs no device and produces frames at exactly the stream's rate.
class NullAudioSource final : public AudioSource {
public:
    explicit NullAudioSource(NullSourceSignal signal);

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

private:
    NullSourceSignal signal_;
    StreamFormat format_{};
    size_t bytes_per_frame_{0};
    int64_t start_us_{0};  ///< steady-clock time of frame 0
    uint64_t frames_{0};   ///< frames produced since open()
};

}  // namespace sendspin_cli
