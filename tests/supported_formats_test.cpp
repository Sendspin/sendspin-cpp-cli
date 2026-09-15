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

/// supported_formats() and the --audio-format grammar, without an audio device.

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

/// One entry as a comparable tuple; the struct has no operator==.
using FormatKey = std::tuple<int, uint8_t, uint32_t, uint8_t>;

/// The advertisement as a sorted set, ignoring order.
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

/// The unranked ascending crossing, so tests can hold the ranking to a pure permutation.
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

// The device's own limits reach the advertisement

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
    // A 7.1 card still advertises stereo.
    const SinkCapabilities caps{{48000}, {16}, {1, 2, 6, 8}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    ASSERT_FALSE(formats.empty());
    for (const AudioSupportedFormatObject& format : formats) {
        EXPECT_EQ(format.channels, 2);
    }
}

// Per codec, not one cross product

TEST(SupportedFormats, OpusIsOnly48kHz16Bit) {
    const SinkCapabilities caps{{44100, 48000, 96000}, {16, 24}, {2}};

    const std::vector<AudioSupportedFormatObject> formats = supported_formats(caps);

    EXPECT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 1U);
    EXPECT_TRUE(has(formats, SendspinCodecFormat::OPUS, 2, 48000, 16));
    EXPECT_FALSE(has(formats, SendspinCodecFormat::OPUS, 2, 44100, 16));
    EXPECT_FALSE(has(formats, SendspinCodecFormat::OPUS, 2, 48000, 24));
}

TEST(SupportedFormats, OpusIsDroppedWhereTheDeviceCannotTakeIt) {
    EXPECT_EQ(
        count_codec(supported_formats({{44100, 88200}, {16}, {2}}), SendspinCodecFormat::OPUS), 0U);
    EXPECT_EQ(count_codec(supported_formats({{48000}, {24, 32}, {2}}), SendspinCodecFormat::OPUS),
              0U);
}

TEST(SupportedFormats, OpusIsDroppedOnADeviceNarrowerThanNothingBelowFourChannels) {
    // opus_decoder_init() takes mono or stereo only.
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

// Degenerate capability sets

TEST(SupportedFormats, ADeviceWithNoUsableRateAdvertisesNothing) {
    // Empty, so the caller can report it.
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

// The advertisement is a ranking: servers take the first entry they can encode

TEST(SupportedFormats, RankingOnlyPermutesWhatIsAdvertised) {
    // Ranking must never change which formats go out.
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
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats(SinkCapabilities::permissive());

    ASSERT_FALSE(formats.empty());
    EXPECT_EQ(formats.front().codec, SendspinCodecFormat::FLAC);
    EXPECT_EQ(formats.front().channels, 2);
    EXPECT_EQ(formats.front().sample_rate, 48000U);
    EXPECT_EQ(formats.front().bit_depth, 16);
}

TEST(SupportedFormats, ADeviceWithNeitherPreferredRateLeadsWithItsBestRemaining) {
    // No 48 or 44.1 kHz: the best rate on offer leads, not the lowest.
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
    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::PCM),
              rates_of(formats, SendspinCodecFormat::FLAC));
    EXPECT_EQ(depths_of(formats, SendspinCodecFormat::PCM),
              depths_of(formats, SendspinCodecFormat::FLAC));
}

TEST(SupportedFormats, OpusSitsAfterEveryFlacEntryAndBeforeEveryPcmEntry) {
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{44100, 48000}, {16, 24}, {2}});

    ASSERT_EQ(count_codec(formats, SendspinCodecFormat::OPUS), 1U);
    EXPECT_LT(last_index(formats, SendspinCodecFormat::FLAC),
              first_index(formats, SendspinCodecFormat::OPUS));
    EXPECT_LT(last_index(formats, SendspinCodecFormat::OPUS),
              first_index(formats, SendspinCodecFormat::PCM));
}

TEST(SupportedFormats, ARateTheLaddersDoNotNameIsAdvertisedLastRatherThanDropped) {
    // An unranked rate sorts to the back rather than being dropped.
    const std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{8000, 48000}, {16}, {2}});

    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 8000, 16));
    EXPECT_EQ(rates_of(formats, SendspinCodecFormat::FLAC), (std::vector<uint32_t>{48000, 8000}));
}

// The startup digest

TEST(DescribeFormats, GroupsTheAxesPerCodec) {
    // Axes in advertised order, codecs in first-seen order.
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

// parse_format_spec(): the --audio-format grammar

TEST(ParseFormatSpec, ReadsEachCodec) {
    const std::pair<const char*, SendspinCodecFormat> specs[] = {
        {"flac:48000:24:2", SendspinCodecFormat::FLAC},
        {"opus:48000:16:2", SendspinCodecFormat::OPUS},
        {"pcm:44100:16:1", SendspinCodecFormat::PCM},
    };
    for (const auto& [spec, codec] : specs) {
        AudioSupportedFormatObject format{};
        std::string error;

        ASSERT_TRUE(parse_format_spec(spec, format, error)) << spec << ": " << error;
        EXPECT_EQ(format.codec, codec) << spec;
    }
}

TEST(ParseFormatSpec, ReadsTheThreeNumericFields) {
    AudioSupportedFormatObject format{};
    std::string error;

    ASSERT_TRUE(parse_format_spec("flac:96000:24:2", format, error)) << error;
    EXPECT_EQ(format.sample_rate, 96000U);
    EXPECT_EQ(format.bit_depth, 24);
    EXPECT_EQ(format.channels, 2);
}

TEST(ParseFormatSpec, RejectsTheWrongShape) {
    for (const char* spec : {"", "flac", "flac:48000", "flac:48000:24", "flac:48000:24:2:extra"}) {
        AudioSupportedFormatObject format{};
        std::string error;

        EXPECT_FALSE(parse_format_spec(spec, format, error)) << spec;
        EXPECT_NE(error.find("codec:rate:depth:channels"), std::string::npos) << spec;
    }
}

TEST(ParseFormatSpec, RejectsACodecThisPlayerCannotPlay) {
    AudioSupportedFormatObject format{};
    std::string error;

    EXPECT_FALSE(parse_format_spec("mp3:48000:16:2", format, error));
    EXPECT_NE(error.find("mp3"), std::string::npos) << error;
    EXPECT_NE(error.find("flac, opus and pcm"), std::string::npos) << error;
}

TEST(ParseFormatSpec, RejectsNumbersThatAreNotPlainNumbers) {
    // Digits only.
    for (const char* spec : {"flac:abc:24:2", "flac: 48000:24:2", "flac:-48000:24:2",
                             "flac:48000:24:", "flac:48000:24:0", "flac:0:24:2"}) {
        AudioSupportedFormatObject format{};
        std::string error;

        EXPECT_FALSE(parse_format_spec(spec, format, error)) << spec;
    }
}

TEST(ParseFormatSpec, RejectsADepthNoSinkCanEmit) {
    AudioSupportedFormatObject format{};
    std::string error;

    EXPECT_FALSE(parse_format_spec("flac:48000:20:2", format, error));
    EXPECT_NE(error.find("8, 16, 24 or 32"), std::string::npos) << error;
}

TEST(ParseFormatSpec, RejectsAnOpusShapeNoDecoderCanReach) {
    // Shapes the Opus decoder can never produce are refused by codec.
    for (const char* spec : {"opus:44100:16:2", "opus:48000:24:2", "opus:48000:16:4"}) {
        AudioSupportedFormatObject format{};
        std::string error;

        EXPECT_FALSE(parse_format_spec(spec, format, error)) << spec;
        EXPECT_NE(error.find("opus"), std::string::npos) << spec << ": " << error;
        EXPECT_NE(error.find("48000"), std::string::npos) << spec << ": " << error;
    }
}

TEST(ParseFormatSpec, KeepsTheOpusShapesTheDecoderReaches) {
    // Mono must pass: the channel check is a ceiling, not an equality.
    for (const char* spec : {"opus:48000:16:2", "opus:48000:16:1"}) {
        AudioSupportedFormatObject format{};
        std::string error;

        EXPECT_TRUE(parse_format_spec(spec, format, error)) << spec << ": " << error;
    }
}

// parse_format_list(): the comma-separated --audio-format value

TEST(ParseFormatList, ReadsASingleSpecExactlyAsParseFormatSpecDoes) {
    std::vector<AudioSupportedFormatObject> formats;
    std::string error;

    ASSERT_TRUE(parse_format_list("flac:48000:24:2", formats, error)) << error;
    ASSERT_EQ(formats.size(), 1U);
    EXPECT_EQ(format_list_spec(formats), "flac:48000:24:2");
}

TEST(ParseFormatList, KeepsSeveralSpecsInTheOrderGiven) {
    std::vector<AudioSupportedFormatObject> formats;
    std::string error;

    ASSERT_TRUE(parse_format_list("pcm:44100:16:2,flac:48000:24:2,opus:48000:16:2", formats, error))
        << error;
    ASSERT_EQ(formats.size(), 3U);
    EXPECT_EQ(formats[0].codec, SendspinCodecFormat::PCM);
    EXPECT_EQ(formats[1].codec, SendspinCodecFormat::FLAC);
    EXPECT_EQ(formats[2].codec, SendspinCodecFormat::OPUS);
    EXPECT_EQ(format_list_spec(formats), "pcm:44100:16:2,flac:48000:24:2,opus:48000:16:2");
}

TEST(ParseFormatList, RefusesAnEmptyEntryAndSaysWhichOne) {
    const std::pair<const char*, const char*> cases[] = {
        {"flac:48000:24:2,", "entry 2"},
        {",flac:48000:24:2", "entry 1"},
        {"flac:48000:24:2,,pcm:48000:16:2", "entry 2"},
    };
    for (const auto& [list, named] : cases) {
        std::vector<AudioSupportedFormatObject> formats;
        std::string error;

        EXPECT_FALSE(parse_format_list(list, formats, error)) << list;
        EXPECT_NE(error.find(named), std::string::npos) << list << ": " << error;
        EXPECT_NE(error.find("empty"), std::string::npos) << list << ": " << error;
        EXPECT_TRUE(formats.empty()) << list;
    }
}

TEST(ParseFormatList, RefusesAFormatListedTwice) {
    std::vector<AudioSupportedFormatObject> formats;
    std::string error;

    EXPECT_FALSE(
        parse_format_list("flac:48000:24:2,pcm:48000:16:2,flac:48000:24:2", formats, error));
    EXPECT_NE(error.find("'flac:48000:24:2'"), std::string::npos) << error;
    EXPECT_NE(error.find("more than once"), std::string::npos) << error;
}

TEST(ParseFormatList, NamesTheBadEntryAnywhereInTheList) {
    const std::pair<const char*, const char*> cases[] = {
        {"mp3:48000:16:2,flac:48000:24:2", "'mp3:48000:16:2'"},
        {"flac:48000:24:2,flac:48000:20:2", "'flac:48000:20:2'"},
        // Not trimmed: the space is part of the entry, and the quote shows it.
        {"flac:48000:24:2, pcm:48000:16:2", "' pcm:48000:16:2'"},
    };
    for (const auto& [list, named] : cases) {
        std::vector<AudioSupportedFormatObject> formats;
        std::string error;

        EXPECT_FALSE(parse_format_list(list, formats, error)) << list;
        EXPECT_NE(error.find(named), std::string::npos) << list << ": " << error;
    }
}

TEST(ParseFormatList, LeavesTheOutputAloneOnFailure) {
    std::vector<AudioSupportedFormatObject> formats = {{SendspinCodecFormat::PCM, 2, 44100, 16}};
    std::string error;

    EXPECT_FALSE(parse_format_list("flac:48000:24:2,bogus", formats, error));
    EXPECT_EQ(format_list_spec(formats), "pcm:44100:16:2");
}

// pin_preferred_formats(): what the pins do to the advertisement

TEST(PinPreferredFormats, MovesOneEntryToTheFrontAndKeepsTheRest) {
    std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{44100, 48000}, {16, 24}, {2}});
    const size_t count = formats.size();
    const AudioSupportedFormatObject pin{SendspinCodecFormat::PCM, 2, 44100, 16};

    ASSERT_TRUE(pin_preferred_formats(formats, {pin}).empty());
    EXPECT_EQ(formats.size(), count);
    EXPECT_EQ(formats.front().codec, SendspinCodecFormat::PCM);
    EXPECT_EQ(formats.front().sample_rate, 44100U);
    EXPECT_EQ(formats.front().bit_depth, 16);
    // A reorder, not a narrowing: the ranked head of the unpinned list is still offered.
    EXPECT_TRUE(has(formats, SendspinCodecFormat::FLAC, 2, 48000, 16));
}

TEST(PinPreferredFormats, PutsSeveralPinsFirstInOrderThenTheRestInRankedOrder) {
    const std::vector<AudioSupportedFormatObject> ranked =
        supported_formats({{44100, 48000}, {16, 24}, {2}});
    std::vector<AudioSupportedFormatObject> formats = ranked;
    const std::vector<AudioSupportedFormatObject> pins = {
        {SendspinCodecFormat::PCM, 2, 44100, 16},
        {SendspinCodecFormat::OPUS, 2, 48000, 16},
        {SendspinCodecFormat::FLAC, 2, 44100, 24},
    };

    ASSERT_TRUE(pin_preferred_formats(formats, pins).empty());

    // Built independently: the pins, then every other ranked entry in order.
    std::vector<AudioSupportedFormatObject> expected = pins;
    const std::string pinned = "," + format_list_spec(pins) + ",";
    for (const AudioSupportedFormatObject& entry : ranked) {
        if (pinned.find("," + format_list_spec({entry}) + ",") == std::string::npos) {
            expected.push_back(entry);
        }
    }
    EXPECT_EQ(format_list_spec(formats), format_list_spec(expected));
    EXPECT_EQ(formats.size(), ranked.size());
    EXPECT_EQ(as_set(formats), as_set(ranked)) << "no entry lost or duplicated";
}

TEST(PinPreferredFormats, EntriesAlreadyAtTheFrontStayPut) {
    std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{44100, 48000}, {16, 24}, {2}});
    const std::vector<AudioSupportedFormatObject> before = formats;

    ASSERT_TRUE(pin_preferred_formats(formats, {before[0], before[1]}).empty());
    EXPECT_EQ(format_list_spec(formats), format_list_spec(before));
}

TEST(PinPreferredFormats, ReportsEveryMissingEntryAndLeavesTheListUntouched) {
    std::vector<AudioSupportedFormatObject> formats =
        supported_formats({{44100, 48000}, {16, 24}, {2}});
    const std::vector<AudioSupportedFormatObject> before = formats;
    const std::vector<AudioSupportedFormatObject> pins = {
        {SendspinCodecFormat::FLAC, 2, 192000, 24},
        {SendspinCodecFormat::PCM, 2, 48000, 16},  // carried, so not reported
        {SendspinCodecFormat::PCM, 2, 96000, 16},
    };

    const std::vector<AudioSupportedFormatObject> missing = pin_preferred_formats(formats, pins);

    EXPECT_EQ(format_list_spec(missing), "flac:192000:24:2,pcm:96000:16:2");
    // Left untouched, so the caller's refusal describes the list that would have gone out.
    EXPECT_EQ(format_list_spec(formats), format_list_spec(before));
}

}  // namespace
}  // namespace sendspin_cli
