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

/// Checks apply_volume() against little-endian bytes; standalone so it cross-compiles for a
/// big-endian target without GoogleTest. Exits non-zero on any mismatch.

#include "pcm_volume.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using sendspin_cli::Q32_ONE;

void put_le(uint8_t* p, uint8_t width, int32_t sample) {
    for (uint8_t byte = 0; byte < width; ++byte) {
        p[byte] = static_cast<uint8_t>(static_cast<uint32_t>(sample) >> (8U * byte));
    }
}

int32_t get_le(const uint8_t* p, uint8_t width) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < width; ++byte) {
        value |= static_cast<uint32_t>(p[byte]) << (8U * byte);
    }
    // Sign-extends from the sample's top bit.
    const uint32_t sign = UINT32_C(1) << ((8U * width) - 1U);
    return static_cast<int32_t>((value ^ sign) - sign);
}

/// @return The number of samples apply_volume() got wrong at this width and gain.
int mismatches(uint8_t width, uint64_t gain) {
    const int64_t full_scale = (INT64_C(1) << ((8 * width) - 1)) - 1;
    std::vector<int32_t> samples;
    for (const int64_t divisor : {1, 2, 3, 7, 100, 1000}) {
        samples.push_back(static_cast<int32_t>(full_scale / divisor));
        samples.push_back(static_cast<int32_t>(-full_scale / divisor));
    }
    samples.push_back(static_cast<int32_t>(-full_scale - 1));
    samples.insert(samples.end(), {0, 1, -1});

    std::vector<uint8_t> data(samples.size() * width);
    for (size_t i = 0; i < samples.size(); ++i) {
        put_le(&data[i * width], width, samples[i]);
    }
    sendspin_cli::apply_volume(data.data(), data.size(), width, gain);

    int bad = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const int64_t want = ((static_cast<int64_t>(samples[i]) * static_cast<int64_t>(gain)) +
                              (INT64_C(1) << 31)) >>
                             32;
        if (get_le(&data[i * width], width) != want) {
            ++bad;
        }
    }
    return bad;
}

}  // namespace

int main() {
    int failed = 0;
    for (const uint8_t width : {2, 3, 4}) {
        for (const uint64_t gain : {Q32_ONE / 2, Q32_ONE / 3, Q32_ONE / 4}) {
            const int bad = mismatches(width, gain);
            std::printf("%u-bit little-endian, gain %.3f: %s (%d wrong)\n", 8U * width,
                        static_cast<double>(gain) / static_cast<double>(Q32_ONE),
                        bad == 0 ? "ok" : "FAIL", bad);
            failed += bad;
        }
    }
    return failed == 0 ? 0 : 1;
}
