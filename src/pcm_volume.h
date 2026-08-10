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

/// @file pcm_volume.h
/// @brief Software volume for interleaved signed little-endian PCM, in Q32 fixed point

#pragma once

#include <cstddef>
#include <cstdint>

namespace sendspin_cli {

/// @brief Unity gain on the Q32 scale q32_gain_for() produces and apply_volume() consumes.
inline constexpr uint64_t Q32_ONE = UINT64_C(1) << 32;

/// @brief The Q32 gain a 0-100 volume and a mute flag come to.
///
/// Muted, or a volume of 0, is 0 (silence); 100 or above is Q32_ONE (unity, so a caller can
/// skip the scaling entirely). In between the taper is quadratic, `(volume/100)^2`, which is
/// close enough to perceptually uniform to feel linear on a controller's slider.
///
/// Shared by every backend that scales samples itself, so a stream sounds the same however
/// it is played out. Lifted from upstream's `PortAudioSink::update_volume_multiplier_()`.
uint64_t q32_gain_for(uint8_t volume, bool muted);

/// @brief Scales `len` bytes of signed little-endian PCM by a Q32 gain, in place.
///
/// `bytes_per_sample` is the stream's bit depth over 8: 1, 2, 3 (packed 24-bit, not padded
/// into four) or 4. Any other value leaves the data alone rather than corrupting it.
/// A trailing partial sample is left alone for the same reason.
///
/// Only ever scales down -- `scale` above Q32_ONE would clip, and no caller produces one --
/// so the result cannot overflow the sample type and no clamping is needed.
///
/// Lifted from upstream's `PortAudioSink::apply_volume_()`.
void apply_volume(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint64_t scale);

}  // namespace sendspin_cli
