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

/// Crosses a sink's capabilities with what each codec can carry, for `client/hello`.

#pragma once

#include "audio_sink.h"

#include <sendspin/config.h>

#include <string>
#include <vector>

namespace sendspin_cli {

/// The formats to advertise for a sink with these capabilities, in priority order.
/// @return FLAC, then OPUS (48 kHz/16-bit, up to stereo), then PCM, each by preferred rate and
/// depth. Empty when `caps` has an empty axis.
std::vector<sendspin::AudioSupportedFormatObject> supported_formats(const SinkCapabilities& caps);

/// Parses one --audio-format spec, `codec:rate:depth:channels`, e.g. `flac:48000:24:2`.
/// @param error Set to the reason, without the flag's name.
bool parse_format_spec(const std::string& spec, sendspin::AudioSupportedFormatObject& out,
                       std::string& error);

/// Parses a comma-separated --audio-format list, refusing empty and duplicate entries.
/// @return true when every entry parsed; `out` then holds them in the order given.
bool parse_format_list(const std::string& list,
                       std::vector<sendspin::AudioSupportedFormatObject>& out, std::string& error);

/// Moves `preferred` to the front of `formats`, in order, without narrowing the list.
/// @return The entries `formats` does not carry; when non-empty, `formats` is left untouched.
std::vector<sendspin::AudioSupportedFormatObject> pin_preferred_formats(
    std::vector<sendspin::AudioSupportedFormatObject>& formats,
    const std::vector<sendspin::AudioSupportedFormatObject>& preferred);

/// Formats in the --audio-format grammar, comma-joined, keeping their order.
std::string format_list_spec(const std::vector<sendspin::AudioSupportedFormatObject>& formats);

/// One-line per-codec digest of an advertisement, for the startup log.
std::string describe_formats(const std::vector<sendspin::AudioSupportedFormatObject>& formats);

}  // namespace sendspin_cli
