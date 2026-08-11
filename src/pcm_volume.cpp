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

}  // namespace

uint64_t q32_gain_for(uint8_t volume, bool muted) {
    if (muted || volume == 0) {
        return 0;
    }
    if (volume >= 100) {
        return Q32_ONE;
    }
    // `amplitude = (volume / 100)^1.5`, the spec's curve. Computed in floating point rather than
    // fixed: this runs once per volume change -- each sink caches the result and apply_volume()
    // below is the only thing in the hot path -- so there is nothing to win by approximating a
    // fractional power in integers, and a great deal of clarity to lose.
    const double amplitude = std::pow(static_cast<double>(volume) / 100.0, 1.5);
    // Rounded rather than truncated so the curve is symmetric about its true value, and safe to
    // cast: the largest amplitude reachable here is volume 99's, which is below unity.
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

}  // namespace sendspin_cli
