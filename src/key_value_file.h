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

/// @file key_value_file.h
/// @brief The flat `key = value` format the state store and the config file share
///
/// One format, read by one function, so the file an operator writes by hand and the file the
/// daemon writes for itself cannot drift apart -- and so `--help` can describe both at once.
/// What differs between them is only what a caller does with the result: the config file
/// refuses a line it cannot read, and the state store ignores it.
///
/// Deliberately not INI: there are no sections, because nothing here has two scopes, and a
/// `[section]` line would then be a silent no-op rather than an error.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief One `key = value` line, with where it came from.
struct KeyValueEntry {
    std::string key;
    std::string value;

    /// The 1-based line it was read from, for a diagnostic that has to name it. A config
    /// file's error is only actionable if it says which line to go and look at.
    size_t line{0};
};

/// @brief What reading a flat `key = value` file produced.
enum class KeyValueStatus {
    Ok,          ///< every line was blank, a comment, or a `key = value` pair
    Unreadable,  ///< the file could not be opened -- missing, or not ours to read
    Malformed,   ///< a line was none of those three
};

/// @brief The longest line either file may contain, as a sanity bound rather than a format rule.
///
/// A line past this reads as Malformed. Both files live in directories other things can write to,
/// so a reader that grew its buffer to whatever it was handed would be the wrong shape.
inline constexpr size_t MAX_KEY_VALUE_LINE_BYTES = 4096;

/// @brief Reads `path` as a flat `key = value` file.
///
/// One pair per line, split on the **first** `=`, with the key and the value each stripped of
/// surrounding spaces and tabs. Blank lines are skipped, and so is a line whose first non-blank
/// character is `#`.
///
/// A `#` anywhere else is an ordinary character, so there are no trailing comments. That is the
/// price of keeping a value byte-for-byte what the equivalent command line would have passed --
/// a device name, a path or a friendly name is free to contain a `#`, and a reader that ate
/// everything after one would silently truncate it.
///
/// An empty value (`name =`) is returned as an empty string rather than refused here: whether
/// that means anything is the option's own business, and refusing it in the reader would put the
/// rule in a second place.
///
/// @param entries Every pair in file order, including repeats -- resolving those is the
/// caller's, since "last wins" and "that is a mistake" are both reasonable and they differ per
/// file.
/// @param malformed_line Set to the offending 1-based line number when Malformed is returned,
/// and left alone otherwise.
KeyValueStatus read_key_value_file(const std::string& path, std::vector<KeyValueEntry>& entries,
                                   size_t& malformed_line);

}  // namespace sendspin_cli
