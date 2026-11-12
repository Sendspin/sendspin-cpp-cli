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

/// Byte order between the player's little-endian PCM and a backend that takes native-endian PCM.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace sendspin_cli {

/// Swaps each sample in place between little-endian and native order; either direction, since
/// the swap is its own inverse. Does nothing on a little-endian host.
/// @param bytes_per_sample 2, 3 (packed) or 4; anything else, 1 included, leaves the data alone.
inline void pcm_le_native_swap(uint8_t* data, size_t len, uint8_t bytes_per_sample) {
    if constexpr (std::endian::native == std::endian::little) {
        return;
    } else {
        if (bytes_per_sample < 2 || bytes_per_sample > 4) {
            return;
        }
        for (size_t i = 0; i + bytes_per_sample <= len; i += bytes_per_sample) {
            std::swap(data[i], data[i + bytes_per_sample - 1]);
            if (bytes_per_sample == 4) {
                std::swap(data[i + 1], data[i + 2]);
            }
        }
    }
}

}  // namespace sendspin_cli
