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

/// @file pcm_ring_test.cpp
/// @brief PcmRingBuffer: the SPSC ring the pull-model backends bridge write() through
///
/// Device-free, like the ring itself, so this runs on a host with no audio backend at all --
/// which is the whole reason the ring lives outside the two sinks that use it.

#include "pcm_ring.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

namespace sendspin_cli {
namespace {

/// Sizes `ring` to hold exactly `bytes` of audio.
///
/// The capacity asked for is one larger, which is the spare byte the ring keeps to tell a full one
/// from an empty one -- so `bytes` is what a caller really gets, and what each sink's
/// ring_capacity_() adds the same byte to.
///
/// By reference rather than by return value because the ring holds atomics and so is neither
/// copyable nor movable -- which is correct for a structure two threads share.
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

    // The producer's contract: a full ring takes what fits and says so, so write() can round the
    // remainder down to a frame boundary and come back for the rest.
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
    // Zeroed rather than left alone: zero is silence for the signed PCM the player emits, so a
    // starved callback outputs a gap instead of whatever the device buffer last held.
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
    // Still there as far as the producer can see: the consumer owns the read position, so the
    // drain happens on its side, on its next read.
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
    // What a sink's ring looks like between streams: close_stream_() resets it to zero, and a
    // write() or a callback arriving in that window must not touch memory that is not there.
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
