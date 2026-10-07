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

/// AudioSource over an ALSA capture PCM, with snd_pcm_delay()-based capture timestamps.

#pragma once

#include "audio_source.h"

#include <alsa/asoundlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sendspin_cli {

/// An AudioSource that captures from an ALSA PCM, e.g. `default` or `hw:1,0`.
class AlsaAudioSource final : public AudioSource {
public:
    explicit AlsaAudioSource(std::string device);
    ~AlsaAudioSource() override;

    std::string name() const override;
    bool negotiate(StreamFormat& format, std::string& error) override;
    bool open(const StreamFormat& format) override;
    int read(uint8_t* data, size_t length, uint32_t timeout_ms, int64_t& capture_time_us) override;
    void close() override;

    /// Prints the host's capture PCMs, as `arecord -L` does.
    static void list_devices(std::FILE* out);

private:
    /// Restarts the stream after an overrun or a suspend.
    /// @return false if the device is gone.
    bool recover_(int err);

    std::string device_;
    snd_pcm_t* pcm_{nullptr};
    uint32_t rate_{0};
    size_t bytes_per_frame_{0};
};

}  // namespace sendspin_cli
