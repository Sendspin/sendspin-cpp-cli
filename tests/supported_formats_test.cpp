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

/// @file supported_formats_test.cpp
/// @brief supported_formats(): what a device's capabilities become in the hello handshake
///
/// Nothing here opens an audio device -- the crossing is a pure function precisely so the
/// suite stays runnable on a sound-card-less CI runner.

#include "supported_formats.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

using sendspin::AudioSupportedFormatObject;
using sendspin::SendspinCodecFormat;

/// True if the advertisement carries exactly this entry.
bool has(const std::vector<AudioSupportedFormatObject>& formats, SendspinCodecFormat codec,
         uint8_t channels, uint32_t rate, uint8_t depth) {
    return std::any_of(formats.begin(), formats.end(),
                       [&](const AudioSupportedFormatObject& format) {
                           return format.codec == codec && format.channels == channels &&
                                  format.sample_rate == rate && format.bit_depth == depth;
                       });
}

size_t count_codec(const std::vector<AudioSupportedFormatObject>& formats,
                   SendspinCodecFormat codec) {
    return static_cast<size_t>(std::count_if(
        formats.begin(), formats.end(),
        [&](const AudioSupportedFormatObject& format) { return format.codec == codec; }));
}

// ---------------------------------------------------------------------------
// The device's own limits reach the advertisement
// ---------------------------------------------------------------------------

TEST(SupportedFormats, A16BitOnlyDeviceAdvertisesOnly16Bit) {
    const SinkCapabilities caps{{44100, 48000}, {16}, {1, 2}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    ASSERT_FALSE(formats.empty());
    for (const AudioSupportedFormatObject& format : formats) {
        EXPECT_EQ(format.bit_depth, 16);
        EXPECT_EQ(format.channels, 2);
    }
    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 44100, 16));
    EXPECT_TRUE(has(formats, SendspinCodecFormat::PCM, 2, 48000, 16));
}

TEST(SupportedFormats, A24And32BitDeviceReachesTheDeeperPaths) {
    // The whole point of deriving the list: before it, 8/24/32-bit were unreachable because
    // the advertisement was hardcoded to 16.
    const SinkCapabilities caps{{44100, 96000}, {24, 32}, {2}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 96000, 24));
    EXPECT_TRUE(has(formats, SendspinCodecFormat::PCM, 2, 44100, 32));
    EXPECT_FALSE(has(formats, SendspinCodecFormat::FLAC, 2, 44100, 16));
}

TEST(SupportedFormats, AMonoOnlyDeviceIsNotAdvertisedAsStereo) {
    const SinkCapabilities caps{{48000}, {16}, {1}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    ASSERT_FALSE(formats.empty());
    for (const AudioSupportedFormatObject& format : formats) {
        EXPECT_EQ(format.channels, 1);
    }
}

TEST(SupportedFormats, StereoWinsOverAWiderCount) {
    // A 7.1 card still gets a stereo advertisement: the player has no reason to ask for more
    // than the stream carries, and every extra count multiplies the list.
    const SinkCapabilities caps{{48000}, {16}, {1, 2, 6, 8}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    ASSERT_FALSE(formats.empty());
    for (const AudioSupportedFormatObject& format : formats) {
        EXPECT_EQ(format.channels, 2);
    }
}

// ---------------------------------------------------------------------------
// Per codec, not one cross product
// ---------------------------------------------------------------------------

TEST(SupportedFormats, OpusIsOnly48kHz16Bit) {
    const SinkCapabilities caps{{44100, 48000, 96000}, {16, 24}, {2}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 1U);
    EXPECT_TRUE(has(formats, SendspinCodecFormat::OPUS, 2, 48000, 16));
    EXPECT_FALSE(has(formats, SendspinCodecFormat::OPUS, 2, 44100, 16));
    EXPECT_FALSE(has(formats, SendspinCodecFormat::OPUS, 2, 48000, 24));
}

TEST(SupportedFormats, OpusIsDroppedWhereTheDeviceCannotTakeIt) {
    // A device with no 48 kHz, and one with no 16-bit: advertising OPUS on either would be a
    // route to a stream that arrives and cannot be played.
    EXPECT_EQ(
        count_codec(supported_formats({{44100, 88200}, {16}, {2}}), SendspinCodecFormat::OPUS), 0U);
    EXPECT_EQ(count_codec(supported_formats({{48000}, {24, 32}, {2}}), SendspinCodecFormat::OPUS),
              0U);
}

TEST(SupportedFormats, OpusIsDroppedOnADeviceNarrowerThanNothingBelowFourChannels) {
    // opus_decoder_init() takes mono or stereo only, so a device whose narrowest count is 4
    // can be sent FLAC and PCM but never Opus -- advertising it would be a nonsense entry.
    const SinkCapabilities caps{{48000}, {16}, {4, 6, 8}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    ASSERT_FALSE(formats.empty());
    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 0U);
    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 4, 48000, 16));
}

TEST(SupportedFormats, OpusSurvivesOnAMonoDevice) {
    const SinkCapabilities caps{{48000}, {16}, {1}};

    EXPECT_TRUE(has(supported_formats(caps), SendspinCodecFormat::OPUS, 1, 48000, 16));
}

TEST(SupportedFormats, FlacAndPcmCoverTheWholeGrid) {
    const SinkCapabilities caps{{44100, 48000}, {16, 24}, {2}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::FLAC), 4U);
    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::PCM), 4U);
}

// ---------------------------------------------------------------------------
// Degenerate capability sets
// ---------------------------------------------------------------------------

TEST(SupportedFormats, ADeviceWithNoUsableRateAdvertisesNothing) {
    // Empty rather than guessed at. Substituting a permissive set here would hide the
    // condition from the caller, which is the one place that can report it.
    EXPECT_TRUE(supported_formats({{}, {16}, {2}}).empty());
    EXPECT_TRUE(supported_formats({{48000}, {}, {2}}).empty());
    EXPECT_TRUE(supported_formats({{48000}, {16}, {}}).empty());
}

TEST(SupportedFormats, ThePermissiveSetCoversEveryDepthAndCodec) {
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats(SinkCapabilities::permissive());

    // 8 rates x 4 depths for each of FLAC and PCM, plus the one OPUS entry.
    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::FLAC), PROBE_RATES.size() * 4U);
    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::PCM), PROBE_RATES.size() * 4U);
    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 1U);
    for (const uint8_t depth : PROBE_BIT_DEPTHS) {
        EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 48000, depth))
            << "no FLAC entry at " << static_cast<unsigned>(depth) << "-bit";
    }
}

// ---------------------------------------------------------------------------
// The startup digest
// ---------------------------------------------------------------------------

TEST(DescribeFormats, GroupsTheAxesPerCodec) {
    const std::string text = describe_formats(supported_formats({{44100, 48000}, {16, 24}, {2}}));

    EXPECT_NE(text.find("FLAC 2ch 16/24-bit @ 44100/48000 Hz"), std::string::npos) << text;
    EXPECT_NE(text.find("PCM 2ch 16/24-bit @ 44100/48000 Hz"), std::string::npos) << text;
    EXPECT_NE(text.find("OPUS 2ch 16-bit @ 48000 Hz"), std::string::npos) << text;
}

TEST(DescribeFormats, SaysSoWhenThereIsNothingToSay) {
    EXPECT_EQ(describe_formats({}), "(nothing)");
}

}  // namespace
}  // namespace sendspin_cli
