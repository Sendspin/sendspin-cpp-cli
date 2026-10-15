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

/// PortAudio setup and device lookup shared by the output sink and the capture source.

#pragma once

#include "audio_sink.h"

#include <portaudio.h>

#include <array>
#include <cstdint>
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

/// Serialises PortAudio's init, open, start, abort, close and format queries across threads; the
/// sink takes it inside its own mutex_. Never take it in a stream callback.
std::mutex& pa_api_mutex();

/// Which side of a device a lookup is about.
enum class PaDirection { Output, Input };

/// PortAudio format per PROBE_BIT_DEPTHS entry; 24-bit is packed paInt24.
inline constexpr std::array<PaSampleFormat, PROBE_BIT_DEPTHS.size()> PA_FORMATS{paInt8, paInt16,
                                                                                paInt24, paInt32};

/// Maps a bit depth onto the interleaved little-endian PCM format PortAudio takes.
bool pa_format_for(uint8_t bits_per_sample, PaSampleFormat& format);

/// True if the device has channels on that side.
bool pa_has_channels(const PaDeviceInfo* info, PaDirection direction);

/// Resolves a PortAudio device spec: empty is the default, digits an index, else a unique name.
/// PortAudio must be initialized.
bool resolve_pa_device(const std::string& device, PaDirection direction, PaDeviceIndex& out,
                       std::string& error);

}  // namespace sendspin_cli
