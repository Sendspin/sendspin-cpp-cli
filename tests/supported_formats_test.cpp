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
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
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

/// One entry reduced to something comparable -- AudioSupportedFormatObject is a plain
/// aggregate with no operator==.
using FormatKey = std::tuple<int, uint8_t, uint32_t, uint8_t>;

/// The advertisement as a *set*: sorted keys, so two orderings of the same entries compare
/// equal and a difference is a difference in what was advertised, not in what order.
std::vector<FormatKey> as_set(const std::vector<AudioSupportedFormatObject>& formats) {
    std::vector<FormatKey> keys;
    keys.reserve(formats.size());
    for (const AudioSupportedFormatObject& format : formats) {
        keys.emplace_back(static_cast<int>(format.codec), format.channels, format.sample_rate,
                          format.bit_depth);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

/// What supported_formats() advertised before the preference ladders: the same crossing, in
/// the ascending order a probe reports its axes in. Spelled out here so the ranking can be
/// held to permuting that list and nothing else -- an ordering change that quietly dropped
/// an entry would take a format away from every server and every user picking one by hand.
std::vector<AudioSupportedFormatObject> ascending_advertisement(const SinkCapabilities& caps) {
    std::vector<AudioSupportedFormatObject> formats;
    if (caps.channels.empty()) {
        return formats;
    }
    const uint8_t channels =
        std::find(caps.channels.begin(), caps.channels.end(), 2) != caps.channels.end()
            ? uint8_t{2}
            : caps.channels.front();

    for (const SendspinCodecFormat codec : {SendspinCodecFormat::FLAC, SendspinCodecFormat::PCM}) {
        for (const uint32_t rate : caps.rates) {
            for (const uint8_t depth : caps.bit_depths) {
                formats.push_back({codec, channels, rate, depth});
            }
        }
    }
    if (channels <= 2 &&
        std::find(caps.rates.begin(), caps.rates.end(), 48000U) != caps.rates.end() &&
        std::find(caps.bit_depths.begin(), caps.bit_depths.end(), 16) != caps.bit_depths.end()) {
        formats.push_back({SendspinCodecFormat::OPUS, channels, 48000, 16});
    }
    return formats;
}

/// The distinct rates `codec`'s entries carry, in the order they were advertised in.
std::vector<uint32_t> rates_of(const std::vector<AudioSupportedFormatObject>& formats,
                               SendspinCodecFormat codec) {
    std::vector<uint32_t> rates;
    for (const AudioSupportedFormatObject& format : formats) {
        if (format.codec == codec &&
            std::find(rates.begin(), rates.end(), format.sample_rate) == rates.end()) {
            rates.push_back(format.sample_rate);
        }
    }
    return rates;
}

/// The distinct depths `codec`'s entries carry, in the order they were advertised in.
std::vector<uint8_t> depths_of(const std::vector<AudioSupportedFormatObject>& formats,
                               SendspinCodecFormat codec) {
    std::vector<uint8_t> depths;
    for (const AudioSupportedFormatObject& format : formats) {
        if (format.codec == codec &&
            std::find(depths.begin(), depths.end(), format.bit_depth) == depths.end()) {
            depths.push_back(format.bit_depth);
        }
    }
    return depths;
}

/// Position of the first / last entry carrying `codec`, or formats.size() where there is none.
size_t first_index(const std::vector<AudioSupportedFormatObject>& formats,
                   SendspinCodecFormat codec) {
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].codec == codec) {
            return i;
        }
    }
    return formats.size();
}

size_t last_index(const std::vector<AudioSupportedFormatObject>& formats,
                  SendspinCodecFormat codec) {
    for (size_t i = formats.size(); i > 0; --i) {
        if (formats[i - 1].codec == codec) {
            return i - 1;
        }
    }
    return formats.size();
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
// The advertisement is a ranking, not just a set
//
// The protocol has supported_formats in priority order, first preferred, and Music
// Assistant's aiosendspin takes filter_encodable_formats(...)[0] without looking further.
// So the order these come out in *is* the format a server will pick.
// ---------------------------------------------------------------------------

TEST(SupportedFormats, RankingOnlyPermutesWhatIsAdvertised) {
    // The one thing the ordering must never do: change which formats go out. Every entry the
    // ascending crossing produced is still there, for degenerate capability sets too.
    const std::vector<SinkCapabilities> cases{
        SinkCapabilities::permissive(),
        {{44100, 48000}, {16, 24}, {2}},
        {{22050, 96000}, {8, 16, 24, 32}, {1, 2}},
        {{48000}, {16}, {4, 6, 8}},
        {{44100, 88200}, {16}, {2}},
        {{48000}, {24, 32}, {2}},
        {{48000}, {16}, {1}},
        {{}, {16}, {2}},
        {{48000}, {}, {2}},
        {{48000}, {16}, {}},
    };

    for (const SinkCapabilities& caps : cases) {
        const std::vector<AudioSupportedFormatObject> ranked = supported_formats(caps);
        EXPECT_EQ(as_set(ranked), as_set(ascending_advertisement(caps)))
            << describe_formats(ranked);
    }
}

TEST(SupportedFormats, ThePermissiveSetLeadsWithFlacStereo48kHz16Bit) {
    // The default a device that refuses nothing lands on -- and the whole point of the
    // ladders: ascending probe order used to put FLAC at 22050 Hz here.
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats(SinkCapabilities::permissive());

    ASSERT_FALSE(formats.empty());
    EXPECT_EQ(formats.front().codec, SendspinCodecFormat::FLAC);
    EXPECT_EQ(formats.front().channels, 2);
    EXPECT_EQ(formats.front().sample_rate, 48000U);
    EXPECT_EQ(formats.front().bit_depth, 16);
}

TEST(SupportedFormats, ADeviceWithNeitherPreferredRateLeadsWithItsBestRemaining) {
    // Neither 48 nor 44.1 kHz on offer: the fallback is the best rate the device does take,
    // not its lowest.
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{22050, 96000}, {16}, {2}});

    ASSERT_FALSE(formats.empty());
    EXPECT_EQ(formats.front().sample_rate, 96000U);
    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::FLAC), (std::vector<uint32_t>{96000, 22050}));
}

TEST(SupportedFormats, RatesAndDepthsGoOutRanked) {
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats(SinkCapabilities::permissive());

    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::FLAC),
              (std::vector<uint32_t>{48000, 44100, 96000, 88200, 192000, 176400, 32000, 22050}));
    EXPECT_EQ(depths_of(formats, SendspinCodecFormat::FLAC), (std::vector<uint8_t>{16, 24, 32, 8}));
    // PCM carries the same grid, so it is ranked the same way.
    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::PCM),
              rates_of(formats, SendspinCodecFormat::FLAC));
    EXPECT_EQ(depths_of(formats, SendspinCodecFormat::PCM),
              depths_of(formats, SendspinCodecFormat::FLAC));
}

TEST(SupportedFormats, OpusSitsAfterEveryFlacEntryAndBeforeEveryPcmEntry) {
    // Lossless is preferred outright; Opus is the fallback worth having where bandwidth is
    // the constraint; PCM costs the most bytes for no gain over FLAC.
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{44100, 48000}, {16, 24}, {2}});

    ASSERT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 1U);
    EXPECT_LT(last_index(formats, SendspinCodecFormat::FLAC),
              first_index(formats, SendspinCodecFormat::OPUS));
    EXPECT_LT(last_index(formats, SendspinCodecFormat::OPUS),
              first_index(formats, SendspinCodecFormat::PCM));
}

TEST(SupportedFormats, ARateTheLaddersDoNotNameIsAdvertisedLastRatherThanDropped) {
    // A backend that grows a rate outside PROBE_RATES must not lose it to the ranking: the
    // ordering is a permutation, and an unranked value simply sorts to the back.
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{8000, 48000}, {16}, {2}});

    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 8000, 16));
    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::FLAC), (std::vector<uint32_t>{48000, 8000}));
}

// ---------------------------------------------------------------------------
// The startup digest
// ---------------------------------------------------------------------------

TEST(DescribeFormats, GroupsTheAxesPerCodec) {
    // Each axis is spelled in advertised order, and the groups follow first-seen codec order,
    // so the digest reads as the ranking it describes -- 48000 before 44100, FLAC before PCM.
    const std::string text = describe_formats(supported_formats({{44100, 48000}, {16, 24}, {2}}));

    const size_t flac = text.find("FLAC 2ch 16/24-bit @ 48000/44100 Hz");
    const size_t opus = text.find("OPUS 2ch 16-bit @ 48000 Hz");
    const size_t pcm = text.find("PCM 2ch 16/24-bit @ 48000/44100 Hz");

    ASSERT_NE(flac, std::string::npos) << text;
    ASSERT_NE(opus, std::string::npos) << text;
    ASSERT_NE(pcm, std::string::npos) << text;
    EXPECT_LT(flac, opus) << text;
    EXPECT_LT(opus, pcm) << text;
}

TEST(DescribeFormats, SaysSoWhenThereIsNothingToSay) {
    EXPECT_EQ(describe_formats({}), "(nothing)");
}

}  // namespace
}  // namespace sendspin_cli
