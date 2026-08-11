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
/// skip the scaling entirely). In between the curve is the one the Sendspin spec names:
///
///     amplitude = (volume / 100)^1.5
///
/// That exponent is not a taste call. The spec defines a volume as *perceived loudness* rather
/// than amplitude -- "volume 50 should be perceived as half as loud as volume 100" -- and ^1.5
/// is the mapping that makes the number mean that. Upstream's
/// `PortAudioSink::update_volume_multiplier_()`, which the rest of this file is lifted from,
/// uses `^2` instead; that is about 3 dB quiet at volume 50 and 6 dB at 25, so this is a
/// deliberate divergence from it rather than an oversight.
///
/// Two exact anchors worth knowing when reading the tests: `(1/4)^1.5` is exactly `1/8`, and
/// `(1/25)^1.5` is exactly `1/125`, so volume 25 and volume 4 land on round fractions of unity.
///
/// Shared by every backend that scales samples itself, so a stream sounds the same however it is
/// played out.
///
/// `volume` and `muted` are deliberately separate parameters rather than one pre-combined gain:
/// the spec makes them independent -- "a volume change MUST NOT clear the mute state" -- so each
/// sink stores both and recomputes this, and a volume command arriving while muted stays muted.
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

/// @brief How long a **full-scale** gain change takes to complete, in milliseconds, at most.
///
/// The spec asks for one -- "to avoid audible clicks, clients SHOULD apply volume changes over a
/// short ramp" -- and a jump from unity to silence is a step discontinuity in the waveform, which
/// is what a click is.
///
/// This is a slew *rate*, not a duration: it is the time silence-to-unity takes, and a smaller
/// change is proportionally quicker. That is deliberate rather than a simplification -- a
/// constant-duration ramp would have to divide by the distance still to travel, which means reading
/// the current gain, and the current gain belongs to the audio thread. See volume_ramp_step().
///
/// Around 20 ms because that is long enough to put the discontinuity below the audible band and
/// short enough that a volume command still feels immediate: at 44.1 kHz it is 882 frames, under a
/// single buffer at the default --buffer-ms.
inline constexpr uint32_t VOLUME_RAMP_MS = 20;

/// @brief The per-frame gain increment a ramp at this stream's rate moves by.
///
/// `Q32_ONE / (sample_rate * VOLUME_RAMP_MS / 1000)`, rounded up so the window is a bound rather
/// than an approximation. It depends on the stream and nothing else -- which is the point: a caller
/// can derive it without reading the gain a ramp is currently at, so the audio thread stays the
/// only owner of that value and neither sink needs a lock to change its volume.
///
/// @return 0 for a rate too low to ramp across at all (including 0). Both functions below read a
/// step of 0 as "do not ramp": they snap to the target instead, which is the right answer for a
/// sink with no format rather than a division to guard at every call site.
uint64_t volume_ramp_step(uint32_t sample_rate);

/// @brief Where a ramp from `current` toward `target` has reached after `frames` frames.
///
/// The single definition of the ramp's arithmetic, and the reason it is exposed rather than kept
/// inside apply_volume_ramp(): `AlsaAudioSink` scales a whole buffer into scratch but may write
/// only part of it, so it has to commit the advance for the frames it *wrote* rather than the ones
/// it scaled. Recomputing that at the commit site is how the two would drift apart.
///
/// Saturates at `target` and never passes it, so stepping `n` frames at once gives exactly what
/// stepping one frame `n` times gives.
uint64_t ramped_gain(uint64_t current, uint64_t target, uint64_t step, size_t frames);

/// @brief Scales `len` bytes of PCM in place while ramping the gain from `current` to `target`.
///
/// Steps **per frame**, not per sample: every channel of one frame must be scaled by the same gain,
/// or the ramp introduces amplitude skew between channels -- a moving image shift on a stereo
/// stream, which is worse than the click it is there to remove.
///
/// The first frame is scaled at `current` **plus one step**, not at `current` itself, and that is
/// deliberate: it is what makes this agree frame-for-frame with ramped_gain(), which a caller
/// committing a partial advance depends on. The offset is one step out of a whole ramp -- under
/// 1/800th of full scale at 44.1 kHz -- so it is inaudible, and it must not be "fixed".
///
/// Once the ramp reaches `target` the rest of the buffer is scaled in one pass, so a steady-state
/// buffer costs exactly what apply_volume() costs. A trailing partial frame is left alone, for the
/// reason apply_volume() leaves a partial sample alone.
///
/// Pure arithmetic on a caller-owned buffer, like everything else in this file: it holds no state
/// and touches no atomic. Each sink owns its own `current`/`target` pair, because which thread may
/// write which is a property of that sink's threading model rather than of this maths.
///
/// @param channels Frames are `bytes_per_sample * channels` wide. 0 leaves the data alone.
/// @return The gain after the last whole frame -- what the caller must store back as its new
/// `current`, unless it consumed fewer frames than it scaled (see ramped_gain()).
uint64_t apply_volume_ramp(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint8_t channels,
                           uint64_t current, uint64_t target, uint64_t step);

}  // namespace sendspin_cli
