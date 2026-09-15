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

/// Read-only operator config: long flag names as keys, values exactly as getopt would see them.

#pragma once

#include "key_value_file.h"

#include <string>
#include <vector>

namespace sendspin_cli {

/// The system-wide config, the last place looked.
inline constexpr const char* SYSTEM_CONFIG_PATH = "/etc/sendspin-cli.conf";

/// A config file that was found, and what was in it.
struct ConfigFile {
    /// Empty when there was no file to read.
    std::string path;

    /// Entries in file order.
    std::vector<KeyValueEntry> entries;
};

/// Where a config file is looked for, in order; empty variables contribute nothing.
std::vector<std::string> config_search_paths();

/// Finds and reads the first config file, used whole; finding none is not an error.
/// @param explicit_path `--config`'s value, or empty; fatal when it cannot be read.
/// @param search_paths Where to look when `explicit_path` is empty.
/// @return false on an unreadable explicit file or a line that does not parse.
bool load_config_file(const std::string& explicit_path,
                      const std::vector<std::string>& search_paths, ConfigFile& out,
                      std::string& error);

}  // namespace sendspin_cli
