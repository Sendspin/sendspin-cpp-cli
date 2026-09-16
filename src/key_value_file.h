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

/// The flat `key = value` format the state store and the config file share.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sendspin_cli {

/// One `key = value` line, with where it came from.
struct KeyValueEntry {
    std::string key;
    std::string value;

    /// 1-based line number, for diagnostics.
    size_t line{0};
};

/// What reading a flat `key = value` file produced.
enum class KeyValueStatus {
    Ok,          ///< every line was blank, a comment, or a `key = value` pair
    Unreadable,  ///< the file could not be opened -- missing, or not ours to read
    Malformed,   ///< a line was none of those three
};

/// Longest line either file may contain; longer reads as Malformed.
inline constexpr size_t MAX_KEY_VALUE_LINE_BYTES = 4096;

/// Reads `path`: one pair per line split on the first `=`, trimmed; `#` starts a comment only
/// as a line's first non-blank character. Empty values are returned as empty strings.
/// @param entries Every pair in file order, repeats included.
/// @param malformed_line Set to the offending 1-based line when Malformed is returned.
KeyValueStatus read_key_value_file(const std::string& path, std::vector<KeyValueEntry>& entries,
                                   size_t& malformed_line);

}  // namespace sendspin_cli
