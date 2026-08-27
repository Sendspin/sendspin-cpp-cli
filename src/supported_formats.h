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

/// @brief Reads an --audio-format value: `codec:rate:depth:channels`, e.g. `flac:48000:24:2`.
///
/// The grammar is the Python CLI's, extended with `opus` because this player decodes it.
/// Strictly numeric fields for the reason cli.cpp's parse_port() gives, and the bit depth is
/// checked against the four the sinks can emit -- any other depth could never match an
/// advertised entry, so refusing it here names the real problem instead of "the device does
/// not advertise it". An `opus` spec is held to 48 kHz / 16-bit / at most two channels for the
/// same reason and a stronger one: that is the only shape supported_formats() ever emits it in,
/// whatever the device would take, so any other is unreachable rather than merely unadvertised.
///
/// Only the *shape* is settled here. Whether the device actually takes the format is a
/// property of the host, answered at startup by pin_preferred_format() against the derived
/// advertisement -- the split that lets a config file be validated without opening a device.
/// @param error Set to the reason when false comes back, without the flag's name -- the
/// caller prefixes it, because only it knows what the value was typed as.
/// @return true when `spec` parsed into `out`.
bool parse_format_spec(const std::string& spec, sendspin::AudioSupportedFormatObject& out,
                       std::string& error);

/// @brief Moves `preferred` to the front of `formats`, or reports that it is not there.
///
/// Mirrors the Python CLI's `--audio-format`: the pin *reorders* the advertisement rather
/// than narrowing it, so a server that cannot encode the preferred entry still has the rest
/// of the list to fall back on. The protocol has `supported_formats` in priority order, so
/// the front is the whole of what "preferred" means on the wire.
///
/// Absence is the caller's to act on, and the intended action is to refuse to start: an
/// operator who pinned a format their device cannot take asked for something this player
/// cannot do, and playing something else instead is the failure --audio-format exists to
/// prevent.
/// @return true if `preferred` was found (and is now first).
bool pin_preferred_format(std::vector<sendspin::AudioSupportedFormatObject>& formats,
                          const sendspin::AudioSupportedFormatObject& preferred);

/// @brief One-line digest of an advertisement, for the startup log.
///
/// Grouped per codec -- `FLAC 2ch 16/24-bit @ 48000/44100 Hz; OPUS 2ch 16-bit @ 48000 Hz` --
/// because a device-derived list reaches dozens of entries, and a field report needs to say
/// what actually went out without a screenful. Each axis keeps the order it was advertised
/// in, so the digest also shows what the front of the list is.
std::string describe_formats(const std::vector<sendspin::AudioSupportedFormatObject>& formats);

}  // namespace sendspin_cli
