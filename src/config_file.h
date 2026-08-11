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

/// @file config_file.h
/// @brief The operator's config file, so a daemon does not need a long flag line
///
/// Read-only: the daemon never writes here. What it remembers for itself goes in
/// `src/state_store.h` instead, and the split is deliberate -- a daemon that rewrote its own
/// config would destroy the comments and the ordering someone put there.
///
/// Keys are the long flag names minus the dashes, and a value is byte-for-byte what getopt would
/// have handed that flag, so `--help` is the config reference rather than a second document to
/// keep in step. Turning an entry into an option, and layering it under the command line, is
/// `parse_options()`'s (see src/cli.cpp) -- this file only finds the file and reads it.

#pragma once

#include "key_value_file.h"

#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief The system-wide config, the last place looked.
///
/// Named here so `--help`, README.md and the search below cannot drift apart.
inline constexpr const char* SYSTEM_CONFIG_PATH = "/etc/sendspin-cli.conf";

/// @brief A config file that was found, and what was in it.
struct ConfigFile {
    /// The file that was read, empty when there was none to read.
    std::string path;

    /// Its entries in file order, empty when no file was found.
    std::vector<KeyValueEntry> entries;
};

/// @brief Where a config file is looked for, in order, for --help to name and load to walk.
///
/// `$XDG_CONFIG_HOME/sendspin-cli/config`, then `$HOME/.config/sendspin-cli/config`, then
/// SYSTEM_CONFIG_PATH. A variable that is unset or empty contributes nothing -- the XDG spec
/// treats an empty variable as unset, and so does this.
///
/// Deliberately no `$XDG_CONFIG_DIRS` traversal: it is exactly where this surface's scope would
/// inflate, and a player has no use for a config assembled out of several directories.
std::vector<std::string> config_search_paths();

/// @brief Finds and reads the config file, or explains why it cannot be used.
///
/// The first file found is used **whole**. There is no merging across layers: a `/etc` config and
/// a user one do not combine, because a half-overridden config is far harder to reason about than
/// one file you can read top to bottom.
///
/// Finding nothing is silent and normal, and leaves `out.path` empty. There is deliberately no
/// `--no-config` flag: the asymmetry below already gives the same effect, since a run that must
/// not read one can name `/dev/null`.
///
/// @param explicit_path `--config`'s value, empty when it was not given. **Fatal when it cannot be
/// read**, because the operator named that file and silently falling back to the search order
/// would run a player on options nobody chose.
/// @param search_paths Where to look when `explicit_path` is empty, normally
/// config_search_paths(). Injected so tests can walk a list that does not include a real
/// `/etc/sendspin-cli.conf` on the machine running them.
/// @param error Set to a diagnostic naming the file, and the line where a line is at fault.
/// @return false when there is an error to report. A file that exists and does not parse is
/// refused rather than skipped -- an operator's config that is quietly ignored is the failure mode
/// this whole surface has to avoid.
bool load_config_file(const std::string& explicit_path,
                      const std::vector<std::string>& search_paths, ConfigFile& out,
                      std::string& error);

}  // namespace sendspin_cli
