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

#include "config_file.h"

#include "key_value_file.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace sendspin_cli {

namespace {

constexpr const char* CONFIG_SUBDIR = "sendspin-cli";
constexpr const char* CONFIG_FILE = "config";

/// An environment variable's value, or empty when it is unset or set to nothing.
std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

/// How a line that does not parse reads, wherever the file came from.
std::string malformed_message(const std::string& path, size_t line) {
    return path + ":" + std::to_string(line) +
           ": not a 'key = value' line -- keys are the long flag names without the dashes, and "
           "'#' starts a comment only at the start of a line";
}

}  // namespace

std::vector<std::string> config_search_paths() {
    std::vector<std::string> paths;
    const std::string xdg = env_or_empty("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        paths.push_back(xdg + "/" + CONFIG_SUBDIR + "/" + CONFIG_FILE);
    }
    const std::string home = env_or_empty("HOME");
    if (!home.empty()) {
        // Listed even when it duplicates the entry above -- `XDG_CONFIG_HOME=$HOME/.config` is the
        // spec's own default and a common thing to set explicitly. Reading the same file twice is
        // impossible anyway, since the first hit wins.
        paths.push_back(home + "/.config/" + CONFIG_SUBDIR + "/" + CONFIG_FILE);
    }
    paths.push_back(SYSTEM_CONFIG_PATH);
    return paths;
}

bool load_config_file(const std::string& explicit_path,
                      const std::vector<std::string>& search_paths, ConfigFile& out,
                      std::string& error) {
    out.path.clear();
    out.entries.clear();

    size_t malformed_line = 0;
    if (!explicit_path.empty()) {
        switch (read_key_value_file(explicit_path, out.entries, malformed_line)) {
            case KeyValueStatus::Ok:
                out.path = explicit_path;
                return true;
            case KeyValueStatus::Malformed:
                error = malformed_message(explicit_path, malformed_line);
                return false;
            case KeyValueStatus::Unreadable:
                // Fatal, unlike the search below: the operator named this file, so falling back
                // would start a player on options nobody chose.
                error = "--config '" + explicit_path +
                        "': cannot be read -- check the path exists and is readable";
                return false;
        }
    }

    for (const std::string& path : search_paths) {
        switch (read_key_value_file(path, out.entries, malformed_line)) {
            case KeyValueStatus::Ok:
                // The first file found is used whole, and the search stops here: nothing below is
                // merged over it.
                out.path = path;
                return true;
            case KeyValueStatus::Malformed:
                // Refused rather than skipped. A config that exists and is broken is an operator's
                // mistake to see, and carrying on to /etc would run a player on the wrong file.
                error = malformed_message(path, malformed_line);
                return false;
            case KeyValueStatus::Unreadable:
                break;
        }
    }

    // Nothing found, which is silent and normal.
    out.entries.clear();
    return true;
}

}  // namespace sendspin_cli
