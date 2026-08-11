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

/// @file pcm_volume_test.cpp
/// @brief The Q32 software volume both real backends share
///
/// Arithmetic on byte buffers, so nothing here opens a device: the taper and -- the part
/// worth pinning down -- the 24-bit unpack/sign-extend/repack round trip.

#include "pcm_volume.h"

#include <gtest/gtest.h>

#include <cmath>

#include <cstdint>
#include <vector>

namespace sendspin_cli {
namespace {

/// Packs a signed 24-bit sample into the three little-endian bytes the player emits.
std::vector<uint8_t> packed24(int32_t sample) {
    return {static_cast<uint8_t>(sample & 0xFF), static_cast<uint8_t>((sample >> 8) & 0xFF),
            static_cast<uint8_t>((sample >> 16) & 0xFF)};
}

/// Unpacks three little-endian bytes back into a signed 24-bit sample.
int32_t unpacked24(const std::vector<uint8_t>& bytes) {
    int32_t sample = static_cast<int32_t>(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16));
    if ((sample & 0x800000) != 0) {
        sample |= static_cast<int32_t>(0xFF000000);
    }
    return sample;
}

// ---------------------------------------------------------------------------
// q32_gain_for(): the taper
// ---------------------------------------------------------------------------

TEST(Q32GainFor, FullVolumeIsUnitySoCallersCanSkipScalingEntirely) {
    EXPECT_EQ(q32_gain_for(100, false), Q32_ONE);
    // Above 100 is still unity rather than amplification, which would clip.
    EXPECT_EQ(q32_gain_for(255, false), Q32_ONE);
}

TEST(Q32GainFor, MuteAndZeroAreBothSilence) {
    EXPECT_EQ(q32_gain_for(0, false), 0U);
    EXPECT_EQ(q32_gain_for(100, true), 0U);
    EXPECT_EQ(q32_gain_for(50, true), 0U) << "mute has to win over the volume, not blend with it";
}

TEST(Q32GainFor, TheTaperIsTheSpecCurve) {
    // The spec's `amplitude = (volume / 100)^1.5`, pinned on the two volumes where that has an
    // exact answer rather than on a rounded decimal: (1/4)^1.5 is exactly 1/8, and (1/25)^1.5 is
    // exactly 1/125. If either of these drifts, the curve has changed.
    EXPECT_EQ(q32_gain_for(25, false), Q32_ONE / 8);
    EXPECT_EQ(q32_gain_for(4, false), Q32_ONE / 125);

    // And at half the slider, where the answer is irrational: 0.5^1.5 = 0.35355..., so within a
    // count or two of rounding.
    EXPECT_NEAR(static_cast<double>(q32_gain_for(50, false)),
                0.3535533905932738 * static_cast<double>(Q32_ONE), 2.0);
}

TEST(Q32GainFor, TheTaperIsNotUpstreamsQuadraticOne) {
    // Guarding a deliberate divergence, so it cannot be "tidied" back. Upstream's
    // update_volume_multiplier_() uses (volume/100)^2, which the spec's own note rules out by
    // defining volume as perceived loudness. Quadratic would put volume 50 at a quarter of full
    // scale; the spec puts it at 0.354, about 3 dB louder.
    EXPECT_NE(q32_gain_for(50, false), Q32_ONE / 4);
    EXPECT_GT(q32_gain_for(50, false), Q32_ONE / 4);

    // The divergence widens as the slider drops -- 6 dB at volume 25, where quadratic gives 1/16
    // and the spec gives 1/8.
    EXPECT_EQ(q32_gain_for(25, false), 2 * (Q32_ONE / 16));
}

TEST(Q32GainFor, TheTaperIsMonotonicAndNeverAmplifies) {
    uint64_t previous = 0;
    for (uint8_t volume = 1; volume <= 100; ++volume) {
        const uint64_t gain = q32_gain_for(volume, false);
        EXPECT_GT(gain, previous) << "volume " << static_cast<int>(volume) << " did not rise";
        EXPECT_LE(gain, Q32_ONE) << "volume " << static_cast<int>(volume) << " exceeded unity";
        previous = gain;
    }
}

TEST(Q32GainFor, AVolumeIsPerceivedLoudnessRatherThanAmplitude) {
    // The property the exponent exists to give, stated as the spec states it: "volume 50 should
    // be perceived as half as loud as volume 100". Perceived loudness goes as amplitude^(1/1.5),
    // so inverting the curve must recover the volume ratio -- which is what makes the number on a
    // controller's slider mean something to a listener.
    const auto perceived = [](uint8_t volume) {
        return std::pow(static_cast<double>(q32_gain_for(volume, false)) /
                            static_cast<double>(Q32_ONE),
                        1.0 / 1.5);
    };
    EXPECT_NEAR(perceived(50) / perceived(100), 0.5, 0.001);
    EXPECT_NEAR(perceived(25) / perceived(100), 0.25, 0.001);
    EXPECT_NEAR(perceived(75) / perceived(100), 0.75, 0.001);
}

// ---------------------------------------------------------------------------
// apply_volume(): per bit depth
// ---------------------------------------------------------------------------

TEST(ApplyVolume, UnityLeavesEverySampleAlone) {
    for (const uint8_t bytes_per_sample : {1, 2, 3, 4}) {
        std::vector<uint8_t> data = {0x01, 0x80, 0xFF, 0x7F, 0x00, 0x23, 0xAB, 0xCD};
        const std::vector<uint8_t> before = data;
        apply_volume(data.data(), data.size(), bytes_per_sample, Q32_ONE);
        EXPECT_EQ(data, before) << "bytes_per_sample " << static_cast<int>(bytes_per_sample);
    }
}

TEST(ApplyVolume, SilenceZeroesEveryDepth) {
    for (const uint8_t bytes_per_sample : {1, 2, 3, 4}) {
        std::vector<uint8_t> data(12, 0x7F);
        apply_volume(data.data(), data.size(), bytes_per_sample, 0);
        EXPECT_EQ(data, std::vector<uint8_t>(12, 0x00))
            << "bytes_per_sample " << static_cast<int>(bytes_per_sample);
    }
}

TEST(ApplyVolume, SixteenBitScalesSymmetrically) {
    std::vector<int16_t> samples = {32767, 1000, 0, -1000, -32768};
    apply_volume(reinterpret_cast<uint8_t*>(samples.data()), samples.size() * sizeof(int16_t), 2,
                 Q32_ONE / 4);

    EXPECT_EQ(samples[1], 250);
    EXPECT_EQ(samples[2], 0);
    EXPECT_EQ(samples[3], -250);
    // Full scale scaled by a quarter, with the round-to-nearest term applied.
    EXPECT_EQ(samples[0], 8192);
    EXPECT_EQ(samples[4], -8192);
}

TEST(ApplyVolume, TwentyFourBitSignExtendsBeforeScaling) {
    // The trap in packed 24-bit: without sign-extending bit 23 first, a negative sample reads
    // as a large positive one and scaling flips it to a loud, wrong value.
    for (const int32_t sample : {8388607, 1000, 0, -1000, -8388608, -1}) {
        std::vector<uint8_t> data = packed24(sample);
        apply_volume(data.data(), data.size(), 3, Q32_ONE / 4);

        const int32_t scaled = unpacked24(data);
        // A sample may round to zero, but it must never come back the other side of it.
        if (sample >= 0) {
            EXPECT_GE(scaled, 0) << "sign flipped for " << sample;
        } else {
            EXPECT_LE(scaled, 0) << "sign flipped for " << sample;
        }
        // Round-to-nearest, so allow the one-LSB slack it can introduce.
        EXPECT_NEAR(scaled, sample / 4, 1) << "wrong magnitude for " << sample;
    }
}

TEST(ApplyVolume, ThirtyTwoBitQuartersWithoutOverflowing) {
    std::vector<int32_t> samples = {INT32_MAX, INT32_MIN, 4000, -4000};
    apply_volume(reinterpret_cast<uint8_t*>(samples.data()), samples.size() * sizeof(int32_t), 4,
                 Q32_ONE / 4);

    EXPECT_EQ(samples[2], 1000);
    EXPECT_EQ(samples[3], -1000);
    EXPECT_GT(samples[0], 0) << "the widest positive sample must not wrap negative";
    EXPECT_LT(samples[1], 0) << "the widest negative sample must not wrap positive";
}

TEST(ApplyVolume, ATrailingPartialSampleIsLeftAlone) {
    // Five bytes at 16-bit is two whole samples and a stray byte. Scaling the stray would
    // corrupt the sample it belongs to, whose other half arrives in the next buffer.
    std::vector<uint8_t> data = {0x00, 0x40, 0x00, 0x40, 0xAB};
    apply_volume(data.data(), data.size(), 2, Q32_ONE / 4);
    EXPECT_EQ(data[4], 0xAB);
}

TEST(ApplyVolume, AnUnsupportedDepthChangesNothing) {
    // Better to play the stream at full volume than to shred it: no backend asks for a depth
    // that is not 1, 2, 3 or 4 bytes, and corrupting the buffer would be the louder failure.
    std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55};
    const std::vector<uint8_t> before = data;
    apply_volume(data.data(), data.size(), 5, Q32_ONE / 2);
    EXPECT_EQ(data, before);
}

}  // namespace
}  // namespace sendspin_cli
