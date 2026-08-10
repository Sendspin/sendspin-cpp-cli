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

}  // namespace

std::vector<AudioSupportedFormatObject> supported_formats(const SinkCapabilities& caps) {
    std::vector<AudioSupportedFormatObject> formats;

    const uint8_t channels = advertised_channels(caps.channels);
    if (channels == 0) {
        return formats;
    }

    // FLAC and PCM carry whatever the device takes: micro_flac packs at the stream's own
    // depth, and PCM arrives already packed, so both reach every depth the sinks can map.
    for (const SendspinCodecFormat codec : {SendspinCodecFormat::FLAC, SendspinCodecFormat::PCM}) {
        for (const uint32_t rate : caps.rates) {
            for (const uint8_t depth : caps.bit_depths) {
                formats.push_back({codec, channels, rate, depth});
            }
        }
    }

    if (channels <= OPUS_MAX_CHANNELS && contains(caps.rates, OPUS_RATE) &&
        contains(caps.bit_depths, OPUS_BIT_DEPTH)) {
        formats.push_back({SendspinCodecFormat::OPUS, channels, OPUS_RATE, OPUS_BIT_DEPTH});
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
