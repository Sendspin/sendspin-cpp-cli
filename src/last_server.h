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

/// @file last_server.h
/// @brief Remembering which server this player last completed a handshake with
///
/// Deliberately *not* the library's `SendspinPersistenceProvider`: that stores an FNV1 hash
/// of the server id, computed by `ConnectionManager::fnv1_hash()`, which lives in the
/// library's `src/` and is not installed -- so a browsed candidate could not be matched
/// against it without reimplementing a private hash and staying bit-compatible with it
/// across tag bumps. Implementing that provider, and making this path configurable, is
/// docs/ROADMAP.md item 8's; this file is only what discovery's own tie-break needs.

#pragma once

#include <string>

namespace sendspin_cli {

/// @brief Where the remembered server id is kept, or empty if there is nowhere to put it.
///
/// `$XDG_STATE_HOME/sendspin-cli/last-server`, falling back to
/// `$HOME/.local/state/sendspin-cli/last-server`. A process with neither variable set --
/// most system services -- gets an empty path, and simply does not remember.
std::string last_server_path();

/// @brief Reads the remembered server id from `path`.
///
/// @return true if a non-empty id was read. A missing or unreadable file is not an error:
/// having no memory is the normal state on a first run.
bool load_last_server(const std::string& path, std::string& server_id);

/// @brief Writes `server_id` to `path`, creating the parent directory if it is missing.
///
/// @return true if it was written. A failure is reported so the caller can log it once, but
/// it is never fatal: a player that cannot remember its server still works, it just falls
/// back to the first server to resolve.
bool save_last_server(const std::string& path, const std::string& server_id);

}  // namespace sendspin_cli
