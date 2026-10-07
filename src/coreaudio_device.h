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

/// CoreAudio device lookup shared by the output sink and the capture source.

#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Which side of a device a lookup is about.
enum class CaDirection { Output, Input };

AudioObjectPropertyAddress ca_address_of(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal);

/// Renders an OSStatus the way CoreAudio headers write one: a four-character code where it is.
std::string ca_status_text(OSStatus status);

/// The name macOS shows for a device, or "(unknown device)" when it cannot be read.
std::string ca_device_name(AudioDeviceID device);

/// A UInt32 property, or 0 when it cannot be read.
uint32_t ca_u32_property(AudioObjectID object, AudioObjectPropertySelector selector,
                         AudioObjectPropertyScope scope);

/// Total channels across the device's streams on that side; 0 means it has none.
uint32_t ca_channels(AudioDeviceID device, CaDirection direction);

/// The rate the device itself is running at; 0 when it cannot be read.
double ca_nominal_sample_rate(AudioDeviceID device);

/// Latency of the device's first stream on that side, in frames.
uint32_t ca_first_stream_latency(AudioDeviceID device, CaDirection direction);

/// The system default device for that side, or kAudioObjectUnknown.
AudioDeviceID ca_default_device(CaDirection direction);

/// This host's devices with channels on that side, in HAL order; the position is the index -o,
/// --input and -l use.
std::vector<AudioDeviceID> ca_enumerate_devices(CaDirection direction);

/// Resolves a CoreAudio device spec: empty is the default, digits an index, else a unique name.
bool resolve_ca_device(const std::string& device, CaDirection direction, AudioDeviceID& out,
                       std::string& error);

/// Maps a stream format onto the interleaved signed little-endian PCM an AUHAL unit takes.
bool ca_asbd_for(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                 AudioStreamBasicDescription& out);

/// A fresh, uninitialized AUHAL unit, or nullptr.
AudioUnit ca_new_hal_unit();

/// Mach absolute-time ticks per microsecond; 0 when the timebase cannot be read.
double ca_host_ticks_per_us();

}  // namespace sendspin_cli
