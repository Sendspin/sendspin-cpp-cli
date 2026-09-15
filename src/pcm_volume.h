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

/// Software volume for interleaved signed little-endian PCM, in Q32 fixed point.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sendspin_cli {

/// Unity gain on the Q32 scale.
inline constexpr uint64_t Q32_ONE = UINT64_C(1) << 32;

/// Q32 gain for a 0-100 volume: (volume / 100)^1.5 per the spec, 0 when muted.
/// The ^1.5 is the spec's perceived-loudness curve; upstream's ^2 is deliberately not used.
uint64_t q32_gain_for(uint8_t volume, bool muted);

/// Scales `len` bytes of signed little-endian PCM in place by a Q32 gain of at most Q32_ONE.
/// @param bytes_per_sample 1, 2, 3 (packed) or 4; anything else leaves the data alone.
void apply_volume(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint64_t scale);

/// Time a full-scale gain change takes; smaller changes are proportionally quicker.
inline constexpr uint32_t VOLUME_RAMP_MS = 20;

/// Per-frame Q32 gain increment for a ramp at `sample_rate`.
/// @return 0 when the rate is too low to ramp, which callers treat as "snap to target".
uint64_t volume_ramp_step(uint32_t sample_rate);

/// Gain a ramp from `current` toward `target` reaches after `frames` frames, saturating.
uint64_t ramped_gain(uint64_t current, uint64_t target, uint64_t step, size_t frames);

/// Scales PCM in place, ramping per frame from `current` toward `target`.
/// The first frame is at `current` plus one step, to match ramped_gain(); do not "fix" it.
/// @return The gain after the last whole frame, for the caller to store back.
uint64_t apply_volume_ramp(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint8_t channels,
                           uint64_t current, uint64_t target, uint64_t step);

}  // namespace sendspin_cli
