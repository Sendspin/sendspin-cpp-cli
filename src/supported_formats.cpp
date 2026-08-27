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

#include "supported_formats.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::AudioSupportedFormatObject;
using sendspin::SendspinCodecFormat;

namespace {

/// The only shape an Opus stream can reach here.
///
/// The decoder hands `opus_decode()` an `int16_t` buffer, so 16-bit is the only depth on that
/// path whatever the device would take; 48 kHz is the rate Opus is carried at; and
/// `opus_decoder_init()` takes mono or stereo only, so a device offering nothing narrower
/// than 4 channels cannot be sent Opus at all.
constexpr uint32_t OPUS_RATE = 48000;
constexpr uint8_t OPUS_BIT_DEPTH = 16;
constexpr uint8_t OPUS_MAX_CHANNELS = 2;

// The order the advertisement goes out in. It is a ranking, not a set: the protocol has
// `supported_formats` in priority order, first preferred, and a server that honours that plays
// whatever sits at the front of the list it can encode -- Music Assistant's `aiosendspin`
// takes `filter_encodable_formats(...)[0]` and never looks further. Emitting the probe ladders
// in their own ascending order therefore handed it the worst entry the device would take: on
// the ALSA `default` PCM, which accepts everything, that is FLAC at 22050 Hz.
//
// The three ladders below rank the same entries instead. They only ever reorder -- narrowing
// the list would take away formats a server, and a user picking one by hand, can still ask for.

/// Lossless first; then Opus, which is the one worth having where bandwidth is the
/// constraint; then PCM, which costs the most bytes on the wire for no gain over FLAC.
constexpr std::array<SendspinCodecFormat, 3> CODEC_PREFERENCE{
    SendspinCodecFormat::FLAC, SendspinCodecFormat::OPUS, SendspinCodecFormat::PCM};

/// 48 kHz ahead of 44.1 kHz, so the preferred rate is also the one rate Opus can be carried
/// at; then the high-resolution rates, each ahead of its 44.1 kHz-family sibling to keep the
/// whole ladder in the 48 kHz family the head sits in; then the two rates below CD, which
/// nothing should be defaulted into and which are the reason this ladder exists at all.
constexpr std::array<uint32_t, 8> RATE_PREFERENCE{48000,  44100,  96000, 88200,
                                                  192000, 176400, 32000, 22050};

/// 16-bit first, which is what most streams are mastered at and what every decoder here packs
/// natively; then the deeper paths; then 8-bit last, so it can never lead. A server that
/// cannot encode 8-bit drops those entries itself, but one that can should not be steered
/// into them.
constexpr std::array<uint8_t, 4> DEPTH_PREFERENCE{16, 24, 32, 8};

/// The channel count to advertise: stereo where the device takes it, its narrowest count
/// otherwise. Zero when the device takes no probed count at all.
uint8_t advertised_channels(const std::vector<uint8_t>& channels) {
    if (channels.empty()) {
        return 0;
    }
    if (std::find(channels.begin(), channels.end(), 2) != channels.end()) {
        return 2;
    }
    // Ascending, so the front is the narrowest -- mono on a device that has one output.
    return channels.front();
}

/// Where `value` sits on `ladder`, or one past its end for a value the ladder does not name.
template <typename T, std::size_t N>
std::size_t preference_rank(const std::array<T, N>& ladder, const T& value) {
    return static_cast<std::size_t>(std::find(ladder.begin(), ladder.end(), value) -
                                    ladder.begin());
}

/// `values` reordered to `ladder`'s order, with anything the ladder does not name left at the
/// end in the order it arrived. A stable sort rather than a rebuild from the ladder, because
/// a sort is a permutation by construction: a device reporting a rate or depth outside the
/// probe ladders still gets it advertised, last, rather than silently dropped.
template <typename T, std::size_t N>
std::vector<T> in_preference_order(std::vector<T> values, const std::array<T, N>& ladder) {
    std::stable_sort(values.begin(), values.end(), [&ladder](const T& left, const T& right) {
        return preference_rank(ladder, left) < preference_rank(ladder, right);
    });
    return values;
}

bool contains(const std::vector<uint32_t>& values, uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool contains(const std::vector<uint8_t>& values, uint8_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

/// Adds `value` to `values` if it is not already there, keeping insertion order.
template <typename T>
void add_distinct(std::vector<T>& values, T value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

/// Joins `values` with '/', as the digest spells a set of rates or depths.
template <typename T>
std::string join_slashed(const std::vector<T>& values) {
    std::string text;
    for (const T& value : values) {
        if (!text.empty()) {
            text += '/';
        }
        text += std::to_string(static_cast<unsigned>(value));
    }
    return text;
}

/// How the digest names a codec.
const char* codec_name(SendspinCodecFormat codec) {
    switch (codec) {
        case SendspinCodecFormat::FLAC:
            return "FLAC";
        case SendspinCodecFormat::OPUS:
            return "OPUS";
        case SendspinCodecFormat::PCM:
            return "PCM";
        case SendspinCodecFormat::UNSUPPORTED:
            break;
    }
    return "unsupported";
}

/// Reads one numeric field of a format spec: digits only, non-empty, within `max`.
///
/// Digits-only for the reason cli.cpp's parse_port() gives: strtoul would take " 48000",
/// "+48000" and read "-1" as a huge unsigned, and a spec is four plain numbers or a typo.
bool parse_format_field(const std::string& text, unsigned long max, unsigned long& value) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    value = std::strtoul(text.c_str(), nullptr, 10);
    return value > 0 && value <= max;
}

/// Whether two advertised entries name the same format. The struct has no operator== of its
/// own, and this is the one place that wants one.
bool same_format(const AudioSupportedFormatObject& left, const AudioSupportedFormatObject& right) {
    return left.codec == right.codec && left.channels == right.channels &&
           left.sample_rate == right.sample_rate && left.bit_depth == right.bit_depth;
}

}  // namespace

bool parse_format_spec(const std::string& spec, AudioSupportedFormatObject& out,
                       std::string& error) {
    // Split on every colon and then count, so a fifth field is the same shape error a
    // missing one is rather than being silently dropped.
    std::vector<std::string> parts;
    size_t begin = 0;
    while (true) {
        const size_t colon = spec.find(':', begin);
        if (colon == std::string::npos) {
            parts.push_back(spec.substr(begin));
            break;
        }
        parts.push_back(spec.substr(begin, colon - begin));
        begin = colon + 1;
    }
    if (parts.size() != 4) {
        error = "expected codec:rate:depth:channels, e.g. flac:48000:24:2";
        return false;
    }

    if (parts[0] == "flac") {
        out.codec = SendspinCodecFormat::FLAC;
    } else if (parts[0] == "opus") {
        out.codec = SendspinCodecFormat::OPUS;
    } else if (parts[0] == "pcm") {
        out.codec = SendspinCodecFormat::PCM;
    } else {
        error = "unknown codec '" + parts[0] + "' -- this player plays flac, opus and pcm";
        return false;
    }

    unsigned long rate = 0;
    if (!parse_format_field(parts[1], 999999UL, rate)) {
        error = "'" + parts[1] + "' is not a sample rate in Hz";
        return false;
    }
    unsigned long depth = 0;
    if (!parse_format_field(parts[2], 32UL, depth) ||
        (depth != 8 && depth != 16 && depth != 24 && depth != 32)) {
        // Checked here rather than left to the device match: no sink emits any other depth,
        // so "the device does not advertise it" would blame the device for a typo.
        error = "'" + parts[2] + "' is not a bit depth this player can emit (8, 16, 24 or 32)";
        return false;
    }
    unsigned long channels = 0;
    if (!parse_format_field(parts[3], 255UL, channels)) {
        error = "'" + parts[3] + "' is not a channel count";
        return false;
    }

    // Checked here for the bit depth's reason, and with more force: an Opus entry at any other
    // shape is not merely unadvertised, it is unreachable. supported_formats() emits Opus at
    // OPUS_RATE / OPUS_BIT_DEPTH and no wider than OPUS_MAX_CHANNELS because that is all the
    // decoder can produce, so opus:44100:24:2 would parse, miss every entry in the derived list,
    // and refuse to start, blaming the device for a format no device could have been offered.
    if (out.codec == SendspinCodecFormat::OPUS &&
        (rate != OPUS_RATE || depth != OPUS_BIT_DEPTH || channels > OPUS_MAX_CHANNELS)) {
        error = "opus is decoded at " + std::to_string(OPUS_RATE) + " Hz, " +
                std::to_string(static_cast<unsigned>(OPUS_BIT_DEPTH)) + "-bit, at most " +
                std::to_string(static_cast<unsigned>(OPUS_MAX_CHANNELS)) +
                " channels -- it is advertised in no other shape";
        return false;
    }

    out.sample_rate = static_cast<uint32_t>(rate);
    out.bit_depth = static_cast<uint8_t>(depth);
    out.channels = static_cast<uint8_t>(channels);
    return true;
}

bool pin_preferred_format(std::vector<AudioSupportedFormatObject>& formats,
                          const AudioSupportedFormatObject& preferred) {
    for (size_t index = 0; index < formats.size(); ++index) {
        if (!same_format(formats[index], preferred)) {
            continue;
        }
        // Rotate rather than swap, so everything else keeps its ranked order and only the
        // pinned entry moves.
        std::rotate(formats.begin(), formats.begin() + static_cast<ptrdiff_t>(index),
                    formats.begin() + static_cast<ptrdiff_t>(index) + 1);
        return true;
    }
    return false;
}

std::vector<AudioSupportedFormatObject> supported_formats(const SinkCapabilities& caps) {
    std::vector<AudioSupportedFormatObject> formats;

    const uint8_t channels = advertised_channels(caps.channels);
    if (channels == 0) {
        return formats;
    }

    const std::vector<uint32_t> rates = in_preference_order(caps.rates, RATE_PREFERENCE);
    const std::vector<uint8_t> depths = in_preference_order(caps.bit_depths, DEPTH_PREFERENCE);
    const bool opus_reachable = channels <= OPUS_MAX_CHANNELS && contains(caps.rates, OPUS_RATE) &&
                                contains(caps.bit_depths, OPUS_BIT_DEPTH);

    for (const SendspinCodecFormat codec : CODEC_PREFERENCE) {
        if (codec == SendspinCodecFormat::OPUS) {
            if (opus_reachable) {
                formats.push_back({codec, channels, OPUS_RATE, OPUS_BIT_DEPTH});
            }
            continue;
        }
        // FLAC and PCM carry whatever the device takes: micro_flac packs at the stream's own
        // depth, and PCM arrives already packed, so both reach every depth the sinks can map.
        for (const uint32_t rate : rates) {
            for (const uint8_t depth : depths) {
                formats.push_back({codec, channels, rate, depth});
            }
        }
    }

    return formats;
}

std::string describe_formats(const std::vector<AudioSupportedFormatObject>& formats) {
    if (formats.empty()) {
        return "(nothing)";
    }

    struct Digest {
        SendspinCodecFormat codec;
        std::vector<uint8_t> channels;
        std::vector<uint8_t> depths;
        std::vector<uint32_t> rates;
    };
    std::vector<Digest> digests;

    for (const AudioSupportedFormatObject& format : formats) {
        auto entry = std::find_if(digests.begin(), digests.end(),
                                  [&](const Digest& d) { return d.codec == format.codec; });
        if (entry == digests.end()) {
            entry = digests.insert(digests.end(), Digest{format.codec, {}, {}, {}});
        }
        add_distinct(entry->channels, format.channels);
        add_distinct(entry->depths, format.bit_depth);
        add_distinct(entry->rates, format.sample_rate);
    }

    std::string text;
    for (const Digest& digest : digests) {
        if (!text.empty()) {
            text += "; ";
        }
        text += std::string(codec_name(digest.codec)) + " " + join_slashed(digest.channels) +
                "ch " + join_slashed(digest.depths) + "-bit @ " + join_slashed(digest.rates) +
                " Hz";
    }
    return text;
}

}  // namespace sendspin_cli
