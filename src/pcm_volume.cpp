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

#include "pcm_volume.h"

#include <cmath>

namespace sendspin_cli {

namespace {

/// Fractional bits in the Q32 scale, and the matching round-to-nearest term.
constexpr int FRAC_BITS = 32;
constexpr int64_t ROUND_TERM = INT64_C(1) << (FRAC_BITS - 1);

constexpr uint64_t MS_PER_SECOND = 1000;

}  // namespace

uint64_t q32_gain_for(uint8_t volume, bool muted) {
    if (muted || volume == 0) {
        return 0;
    }
    if (volume >= 100) {
        return Q32_ONE;
    }
    const double amplitude = std::pow(static_cast<double>(volume) / 100.0, 1.5);
    // Safe to cast: the largest amplitude reached here (volume 99) is below unity.
    return static_cast<uint64_t>(std::llround(amplitude * static_cast<double>(Q32_ONE)));
}

void apply_volume(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint64_t scale) {
    const int64_t s_scale = static_cast<int64_t>(scale);

    switch (bytes_per_sample) {
        case 1: {
            auto* samples = reinterpret_cast<int8_t*>(data);
            for (size_t i = 0; i < len; ++i) {
                const int64_t s = static_cast<int64_t>(samples[i]) * s_scale + ROUND_TERM;
                samples[i] = static_cast<int8_t>(s >> FRAC_BITS);
            }
            break;
        }
        case 2: {
            const size_t count = len / 2;
            auto* samples = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < count; ++i) {
                const int64_t s = static_cast<int64_t>(samples[i]) * s_scale + ROUND_TERM;
                samples[i] = static_cast<int16_t>(s >> FRAC_BITS);
            }
            break;
        }
        case 3: {
            const size_t count = len / 3;
            for (size_t i = 0; i < count; ++i) {
                uint8_t* p = data + (i * 3);
                int32_t sample = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
                if ((sample & 0x800000) != 0) {
                    sample |= static_cast<int32_t>(0xFF000000);
                }
                const int32_t out = static_cast<int32_t>(
                    ((static_cast<int64_t>(sample) * s_scale) + ROUND_TERM) >> FRAC_BITS);
                p[0] = static_cast<uint8_t>(out & 0xFF);
                p[1] = static_cast<uint8_t>((out >> 8) & 0xFF);
                p[2] = static_cast<uint8_t>((out >> 16) & 0xFF);
            }
            break;
        }
        case 4: {
            const size_t count = len / 4;
            auto* samples = reinterpret_cast<int32_t*>(data);
            for (size_t i = 0; i < count; ++i) {
                const int64_t s = static_cast<int64_t>(samples[i]) * s_scale + ROUND_TERM;
                samples[i] = static_cast<int32_t>(s >> FRAC_BITS);
            }
            break;
        }
        default:
            break;
    }
}

uint64_t volume_ramp_step(uint32_t sample_rate) {
    const uint64_t frames = (static_cast<uint64_t>(sample_rate) * VOLUME_RAMP_MS) / MS_PER_SECOND;
    if (frames == 0) {
        // No format, or the whole ramp is under one frame: 0 tells callers to snap.
        return 0;
    }
    // Rounded up so the ramp finishes within VOLUME_RAMP_MS; ramped_gain() clamps the overshoot.
    return (Q32_ONE + frames - 1) / frames;
}

uint64_t ramped_gain(uint64_t current, uint64_t target, uint64_t step, size_t frames) {
    if (step == 0) {
        return target;
    }
    if (current == target) {
        return target;
    }

    const bool rising = current < target;
    const uint64_t distance = rising ? target - current : current - target;
    // Compared by division so `frames * step` cannot overflow.
    const uint64_t travelled = (static_cast<uint64_t>(frames) > distance / step)
                                   ? distance
                                   : static_cast<uint64_t>(frames) * step;
    return rising ? current + travelled : current - travelled;
}

uint64_t apply_volume_ramp(uint8_t* data, size_t len, uint8_t bytes_per_sample, uint8_t channels,
                           uint64_t current, uint64_t target, uint64_t step) {
    if (bytes_per_sample == 0 || channels == 0) {
        // No frame width: report no movement rather than snapping, so the ramp is still owed.
        return current;
    }

    const size_t bytes_per_frame = static_cast<size_t>(bytes_per_sample) * channels;
    const size_t frames = len / bytes_per_frame;

    // Step through ramped_gain() so this agrees with the advance a caller commits.
    size_t frame = 0;
    for (; frame < frames && current != target; ++frame) {
        current = ramped_gain(current, target, step, 1);
        apply_volume(data + (frame * bytes_per_frame), bytes_per_frame, bytes_per_sample, current);
    }

    // Unity is the identity, so skip it.
    if (frame < frames && target != Q32_ONE) {
        apply_volume(data + (frame * bytes_per_frame), (frames - frame) * bytes_per_frame,
                     bytes_per_sample, target);
    }
    return current;
}

}  // namespace sendspin_cli
