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

/// PcmRingBuffer: the SPSC ring the pull-model backends bridge write() through.

#include "pcm_ring.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

namespace sendspin_cli {
namespace {

/// Sizes `ring` to hold exactly `bytes`, allowing for the spare byte.
void size_to_hold(PcmRingBuffer& ring, size_t bytes) {
    ring.reset(bytes + 1);
}

std::vector<uint8_t> ramp(size_t len, uint8_t first = 0) {
    std::vector<uint8_t> data(len);
    std::iota(data.begin(), data.end(), first);
    return data;
}

TEST(PcmRingBuffer, RoundTripsWhatWasWritten) {
    PcmRingBuffer ring;
    size_to_hold(ring, 64);
    const std::vector<uint8_t> in = ramp(64);

    EXPECT_EQ(ring.write(in.data(), in.size()), 64u);
    EXPECT_EQ(ring.available(), 64u);
    EXPECT_EQ(ring.free_space(), 0u);

    std::vector<uint8_t> out(64, 0xFF);
    EXPECT_EQ(ring.read(out.data(), out.size()), 64u);
    EXPECT_EQ(out, in);
    EXPECT_EQ(ring.available(), 0u);
}

TEST(PcmRingBuffer, AShortWriteReportsWhatItTook) {
    PcmRingBuffer ring;
    size_to_hold(ring, 8);
    const std::vector<uint8_t> in = ramp(32);

    EXPECT_EQ(ring.write(in.data(), in.size()), 8u);
    EXPECT_EQ(ring.free_space(), 0u);
    EXPECT_EQ(ring.write(in.data(), in.size()), 0u);
}

TEST(PcmRingBuffer, AShortReadIsZeroFilledAndCountsOnlyRealAudio) {
    PcmRingBuffer ring;
    size_to_hold(ring, 64);
    const std::vector<uint8_t> in = ramp(4, 1);
    ASSERT_EQ(ring.write(in.data(), in.size()), 4u);

    std::vector<uint8_t> out(16, 0xFF);
    // Zero-filled: silence for signed PCM.
    EXPECT_EQ(ring.read(out.data(), out.size()), 4u)
        << "the return value is real audio, not the bytes the caller's buffer got";
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[3], 4);
    for (size_t i = 4; i < out.size(); ++i) {
        EXPECT_EQ(out[i], 0) << "byte " << i;
    }
}

TEST(PcmRingBuffer, WrapsAroundTheEndOfTheBuffer) {
    PcmRingBuffer ring;
    size_to_hold(ring, 16);

    // Push the positions most of the way round, then straddle the seam.
    const std::vector<uint8_t> filler = ramp(12);
    ASSERT_EQ(ring.write(filler.data(), filler.size()), 12u);
    std::vector<uint8_t> drain(12, 0);
    ASSERT_EQ(ring.read(drain.data(), drain.size()), 12u);

    const std::vector<uint8_t> in = ramp(16, 100);
    EXPECT_EQ(ring.write(in.data(), in.size()), 16u);
    std::vector<uint8_t> out(16, 0);
    EXPECT_EQ(ring.read(out.data(), out.size()), 16u);
    EXPECT_EQ(out, in);
}

TEST(PcmRingBuffer, ARequestedClearIsCarriedOutByTheReader) {
    PcmRingBuffer ring;
    size_to_hold(ring, 64);
    const std::vector<uint8_t> in = ramp(32, 7);
    ASSERT_EQ(ring.write(in.data(), in.size()), 32u);

    ring.request_clear();
    // The consumer performs the drain, on its next read.
    EXPECT_EQ(ring.available(), 32u);

    std::vector<uint8_t> out(32, 0xFF);
    EXPECT_EQ(ring.read(out.data(), out.size()), 0u);
    for (const uint8_t byte : out) {
        EXPECT_EQ(byte, 0) << "the clearing read hands back silence, not stale audio";
    }
    EXPECT_EQ(ring.available(), 0u);

    // And only once: the next read is an ordinary one again.
    ASSERT_EQ(ring.write(in.data(), in.size()), 32u);
    EXPECT_EQ(ring.read(out.data(), out.size()), 32u);
}

TEST(PcmRingBuffer, DropEmptiesItHereAndNow) {
    PcmRingBuffer ring;
    size_to_hold(ring, 64);
    const std::vector<uint8_t> in = ramp(32);
    ASSERT_EQ(ring.write(in.data(), in.size()), 32u);

    ring.drop();
    EXPECT_EQ(ring.available(), 0u);
    EXPECT_EQ(ring.free_space(), 64u);

    // A pending clear goes with it, or the next stream's first read would be swallowed.
    ring.request_clear();
    ring.drop();
    ASSERT_EQ(ring.write(in.data(), in.size()), 32u);
    std::vector<uint8_t> out(32, 0);
    EXPECT_EQ(ring.read(out.data(), out.size()), 32u);
}

TEST(PcmRingBuffer, AnUnsizedRingTakesNothingAndReadsSilence) {
    // A reset-to-zero ring between streams must tolerate reads and writes.
    PcmRingBuffer ring;
    const std::vector<uint8_t> in = ramp(8);
    EXPECT_EQ(ring.write(in.data(), in.size()), 0u);
    EXPECT_EQ(ring.available(), 0u);
    EXPECT_EQ(ring.free_space(), 0u);

    std::vector<uint8_t> out(8, 0xFF);
    EXPECT_EQ(ring.read(out.data(), out.size()), 0u);
    for (const uint8_t byte : out) {
        EXPECT_EQ(byte, 0);
    }
}

}  // namespace
}  // namespace sendspin_cli
