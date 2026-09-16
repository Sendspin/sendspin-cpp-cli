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
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::AudioSupportedFormatObject;
using sendspin::SendspinCodecFormat;

namespace {

/// The only shape the Opus decoder can produce: 48 kHz, 16-bit, mono or stereo.
constexpr uint32_t OPUS_RATE = 48000;
constexpr uint8_t OPUS_BIT_DEPTH = 16;
constexpr uint8_t OPUS_MAX_CHANNELS = 2;

// Advertisement ranking: servers take the first entry they can encode, so order matters.

/// Lossless first, then Opus, then PCM.
constexpr std::array<SendspinCodecFormat, 3> CODEC_PREFERENCE{
    SendspinCodecFormat::FLAC, SendspinCodecFormat::OPUS, SendspinCodecFormat::PCM};

/// 48 kHz and 44.1 kHz first, then the high-resolution rates, then the rates below CD.
constexpr std::array<uint32_t, 8> RATE_PREFERENCE{48000,  44100,  96000, 88200,
                                                  192000, 176400, 32000, 22050};

/// 16-bit first, then deeper, with 8-bit last so it never leads.
constexpr std::array<uint8_t, 4> DEPTH_PREFERENCE{16, 24, 32, 8};

/// Stereo where the device takes it, else its narrowest count; 0 when it takes none.
uint8_t advertised_channels(const std::vector<uint8_t>& channels) {
    if (channels.empty()) {
        return 0;
    }
    if (std::find(channels.begin(), channels.end(), 2) != channels.end()) {
        return 2;
    }
    return channels.front();
}

/// Where `value` sits on `ladder`, or one past its end for a value the ladder does not name.
template <typename T, std::size_t N>
std::size_t preference_rank(const std::array<T, N>& ladder, const T& value) {
    return static_cast<std::size_t>(std::find(ladder.begin(), ladder.end(), value) -
                                    ladder.begin());
}

/// `values` stably sorted into `ladder`'s order; values the ladder does not name go last.
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

/// How the --audio-format grammar names a codec.
const char* spec_codec_name(SendspinCodecFormat codec) {
    switch (codec) {
        case SendspinCodecFormat::FLAC:
            return "flac";
        case SendspinCodecFormat::OPUS:
            return "opus";
        case SendspinCodecFormat::PCM:
            return "pcm";
        case SendspinCodecFormat::UNSUPPORTED:
            break;
    }
    return "unsupported";
}

/// Reads one numeric spec field: digits only, non-empty, within `max`.
bool parse_format_field(const std::string& text, unsigned long max, unsigned long& value) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    value = std::strtoul(text.c_str(), nullptr, 10);
    return value > 0 && value <= max;
}

/// Whether two advertised entries name the same format.
bool same_format(const AudioSupportedFormatObject& left, const AudioSupportedFormatObject& right) {
    return left.codec == right.codec && left.channels == right.channels &&
           left.sample_rate == right.sample_rate && left.bit_depth == right.bit_depth;
}

}  // namespace

bool parse_format_spec(const std::string& spec, AudioSupportedFormatObject& out,
                       std::string& error) {
    // Split on every colon, so a fifth field is refused rather than dropped.
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
        error = "'" + parts[2] + "' is not a bit depth this player can emit (8, 16, 24 or 32)";
        return false;
    }
    unsigned long channels = 0;
    if (!parse_format_field(parts[3], 255UL, channels)) {
        error = "'" + parts[3] + "' is not a channel count";
        return false;
    }

    // Any other Opus shape could never be advertised, so refuse it here.
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

bool parse_format_list(const std::string& list, std::vector<AudioSupportedFormatObject>& out,
                       std::string& error) {
    std::vector<std::string> entries;
    size_t begin = 0;
    while (true) {
        const size_t comma = list.find(',', begin);
        if (comma == std::string::npos) {
            entries.push_back(list.substr(begin));
            break;
        }
        entries.push_back(list.substr(begin, comma - begin));
        begin = comma + 1;
    }

    const bool several = entries.size() > 1;
    std::vector<AudioSupportedFormatObject> formats;
    for (size_t index = 0; index < entries.size(); ++index) {
        const std::string& entry = entries[index];
        if (several && entry.empty()) {
            error = "entry " + std::to_string(index + 1) +
                    " is empty -- separate formats with single commas";
            return false;
        }
        AudioSupportedFormatObject format{};
        std::string reason;
        if (!parse_format_spec(entry, format, reason)) {
            error = several ? "'" + entry + "': " + reason : reason;
            return false;
        }
        const bool repeated = std::any_of(
            formats.begin(), formats.end(),
            [&](const AudioSupportedFormatObject& seen) { return same_format(seen, format); });
        if (repeated) {
            error = "'" + entry + "' is listed more than once";
            return false;
        }
        formats.push_back(format);
    }

    out = std::move(formats);
    return true;
}

std::vector<AudioSupportedFormatObject> pin_preferred_formats(
    std::vector<AudioSupportedFormatObject>& formats,
    const std::vector<AudioSupportedFormatObject>& preferred) {
    std::vector<AudioSupportedFormatObject> missing;
    for (const AudioSupportedFormatObject& pin : preferred) {
        const bool carried = std::any_of(
            formats.begin(), formats.end(),
            [&](const AudioSupportedFormatObject& entry) { return same_format(entry, pin); });
        if (!carried) {
            missing.push_back(pin);
        }
    }
    if (!missing.empty()) {
        return missing;
    }

    // Rotate each pin to the front, last first, so the first pin leads.
    for (auto pin = preferred.rbegin(); pin != preferred.rend(); ++pin) {
        const auto entry =
            std::find_if(formats.begin(), formats.end(),
                         [&](const AudioSupportedFormatObject& f) { return same_format(f, *pin); });
        std::rotate(formats.begin(), entry, entry + 1);
    }
    return missing;
}

std::string format_list_spec(const std::vector<AudioSupportedFormatObject>& formats) {
    std::string text;
    for (const AudioSupportedFormatObject& format : formats) {
        if (!text.empty()) {
            text += ',';
        }
        text += spec_codec_name(format.codec);
        text += ':' + std::to_string(format.sample_rate) + ':' +
                std::to_string(static_cast<unsigned>(format.bit_depth)) + ':' +
                std::to_string(static_cast<unsigned>(format.channels));
    }
    return text;
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
