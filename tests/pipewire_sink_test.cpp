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

/// The ring-versus-quantum arithmetic behind -o pipewire; built only with the PipeWire backend.

#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE

#include "pipewire_sink.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace sendspin_cli {
namespace {

// pipewire_ring_frames

TEST(PipeWireRingFrames, ScalesWithRateAndBufferMs) {
    EXPECT_EQ(pipewire_ring_frames(48000, 100), 4800U);
    EXPECT_EQ(pipewire_ring_frames(44100, 100), 4410U);
    EXPECT_EQ(pipewire_ring_frames(96000, 250), 24000U);
}

TEST(PipeWireRingFrames, FloorsAtMinRingFrames) {
    // 10 ms at 44.1 kHz is 441 frames, well under the floor, so the floor is what a caller gets.
    EXPECT_EQ(pipewire_ring_frames(44100, 10), MIN_RING_FRAMES);
    EXPECT_GT(pipewire_ring_frames(44100, 100), MIN_RING_FRAMES);
}

TEST(PipeWireRingFrames, ZeroBufferMsStillYieldsAUsableRing) {
    // Not a configuration anyone asks for, but --buffer-ms 0 must not produce a ring of nothing.
    EXPECT_EQ(pipewire_ring_frames(48000, 0), MIN_RING_FRAMES);
}

// pipewire_quantum_fit

TEST(PipeWireQuantumFit, ComfortableRingReportsNeitherFault) {
    // The default case: 100 ms at 48 kHz against the graph's usual 1024-frame quantum.
    const PipeWireQuantumFit fit = pipewire_quantum_fit(4800, 1024, 48000);
    EXPECT_FALSE(fit.starves);
    EXPECT_FALSE(fit.tight);
}

TEST(PipeWireQuantumFit, RingUnderThreeQuantaIsTightNotStarving) {
    const PipeWireQuantumFit fit = pipewire_quantum_fit(2048, 1024, 48000);
    EXPECT_FALSE(fit.starves);
    EXPECT_TRUE(fit.tight);
}

TEST(PipeWireQuantumFit, RingSmallerThanOneQuantumStarves) {
    // A forced 8192-frame quantum against a 100 ms ring: every cycle zero-fills.
    const PipeWireQuantumFit fit = pipewire_quantum_fit(4800, 8192, 48000);
    EXPECT_TRUE(fit.starves);
    EXPECT_FALSE(fit.tight);  // starving is reported instead of tight, never as well as
}

TEST(PipeWireQuantumFit, BoundariesFallOnTheRightSide) {
    EXPECT_FALSE(pipewire_quantum_fit(1024, 1024, 48000).starves);
    EXPECT_TRUE(pipewire_quantum_fit(1023, 1024, 48000).starves);
    // Exactly three quanta clears the floor; one frame short of three does not.
    EXPECT_FALSE(pipewire_quantum_fit(3072, 1024, 48000).tight);
    EXPECT_TRUE(pipewire_quantum_fit(3071, 1024, 48000).tight);
}

TEST(PipeWireQuantumFit, RecommendedBufferMsActuallyClearsTheFloor) {
    // The advice must fix the fault when passed back in.
    const uint32_t rate = 48000;
    const uint32_t quantum = 8192;
    const PipeWireQuantumFit bad = pipewire_quantum_fit(4800, quantum, rate);
    ASSERT_TRUE(bad.starves);
    ASSERT_GT(bad.recommended_buffer_ms, 0U);

    const PipeWireQuantumFit fixed =
        pipewire_quantum_fit(pipewire_ring_frames(rate, bad.recommended_buffer_ms), quantum, rate);
    EXPECT_FALSE(fixed.starves);
    EXPECT_FALSE(fixed.tight);
}

TEST(PipeWireQuantumFit, RecommendationRoundsUpAtAwkwardRates) {
    // 69.66 ms: rounding down to 69 would fail the check, so it rounds up.
    const PipeWireQuantumFit fit = pipewire_quantum_fit(1024, 1024, 44100);
    EXPECT_EQ(fit.recommended_buffer_ms, 70U);
    EXPECT_FALSE(
        pipewire_quantum_fit(pipewire_ring_frames(44100, fit.recommended_buffer_ms), 1024, 44100)
            .tight);
}

TEST(PipeWireQuantumFit, UnobservedGraphReportsNothing) {
    // No quantum yet means no verdict.
    const PipeWireQuantumFit no_quantum = pipewire_quantum_fit(4800, 0, 48000);
    EXPECT_FALSE(no_quantum.starves);
    EXPECT_FALSE(no_quantum.tight);
    EXPECT_EQ(no_quantum.recommended_buffer_ms, 0U);

    // And a rate of zero must not divide by it.
    const PipeWireQuantumFit no_rate = pipewire_quantum_fit(4800, 1024, 0);
    EXPECT_FALSE(no_rate.starves);
    EXPECT_FALSE(no_rate.tight);
    EXPECT_EQ(no_rate.recommended_buffer_ms, 0U);
}

}  // namespace
}  // namespace sendspin_cli

#endif  // SENDSPIN_CLI_HAVE_PIPEWIRE
