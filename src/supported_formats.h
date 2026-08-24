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

/// @file supported_formats.h
/// @brief Crosses a sink's capabilities with what each codec can carry, for `client/hello`

#pragma once

#include "audio_sink.h"

#include <sendspin/config.h>

#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief The formats to advertise in the hello handshake, for a sink with these capabilities.
///
/// Built per codec rather than as one cross product, because the two are not the same set: an
/// entry the decoder cannot produce is a route to a stream that arrives and cannot be played.
///  - **OPUS** at 48 kHz / 16-bit, stereo or mono only. The decoder writes `int16_t`, 48 kHz
///    is the rate Opus is carried at, and `opus_decoder_init()` takes no more than two
///    channels -- so nothing else is reachable however wide the device is.
///  - **FLAC** and **PCM** at every rate and depth the device takes, which is what the
///    decoders pack and both sinks map onto their backend's formats.
///
/// Channels are stereo where the device has two, and its narrowest count where it has not --
/// advertising a width the device cannot take would be a promise this player cannot keep.
///
/// **Ordered, not just collected.** The protocol has `supported_formats` in priority order,
/// first preferred, and servers read it that way -- Music Assistant picks the first entry it
/// can encode and never looks further. So the entries come out ranked by the preference
/// ladders in supported_formats.cpp rather than in the order the device was probed in, which
/// is ascending and would nominate the worst format the device will take. The ranking only
/// permutes: every entry the device can reach is still advertised, so a server, and a user
/// choosing by hand, keep the whole list.
///
/// Pure on purpose: crossing capabilities with codecs is the part worth testing, and a
/// function that probed a device inside itself could not be tested without one.
/// @param caps What the sink's device will take. SinkCapabilities::permissive() for a sink
/// with no device to ask.
/// @return The advertisement in priority order: grouped by codec (FLAC, then OPUS, then PCM),
/// and within FLAC and PCM by preferred rate then preferred depth. Empty when `caps` has an
/// empty axis -- the caller decides what to do about a device that takes nothing, since it is
/// the layer that can name which device that was.
std::vector<sendspin::AudioSupportedFormatObject> supported_formats(const SinkCapabilities& caps);

/// @brief One-line digest of an advertisement, for the startup log.
///
/// Grouped per codec -- `FLAC 2ch 16/24-bit @ 48000/44100 Hz; OPUS 2ch 16-bit @ 48000 Hz` --
/// because a device-derived list reaches dozens of entries, and a field report needs to say
/// what actually went out without a screenful. Each axis keeps the order it was advertised
/// in, so the digest also shows what the front of the list is.
std::string describe_formats(const std::vector<sendspin::AudioSupportedFormatObject>& formats);

}  // namespace sendspin_cli
