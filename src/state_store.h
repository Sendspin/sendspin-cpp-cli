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

/// What the daemon remembers about itself across restarts, in a file only it writes.
/// `last-server-hash` is opaque library data: store and hand it back, never compute it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

/// What reading the state file produced.
enum class StateLoadResult {
    Loaded,   ///< a file was read, and whatever it held is now in the store
    Absent,   ///< there is no file to read, which is the normal state on a first run
    Corrupt,  ///< a file was there and a line did not parse; nothing was taken from it
};

/// `<state_dir>/state`, else under `$XDG_STATE_HOME` or `~/.local/state`; empty when none apply.
/// @param state_dir `--state-dir`'s value, empty when it was not given.
std::string state_store_path(const std::string& state_dir);

/// The daemon's memory, rewritten atomically on every change; an unwritable file is not fatal.
/// Not thread-safe: main loop only.
class StateStore {
public:
    /// @param path From state_store_path(); empty means this run remembers nothing.
    explicit StateStore(std::string path);

    /// Reads the file, replacing anything held. Never fatal, but callers should report `Corrupt`.
    /// @param malformed_line Set to the 1-based line at fault when `Corrupt` comes back.
    StateLoadResult load(size_t& malformed_line);

    const std::string& path() const {
        return this->path_;
    }

    /// The id of the last server a handshake completed with, empty if none.
    std::string last_server() const;
    bool set_last_server(const std::string& server_id);

    /// The library's opaque last-played-server hash, as handed to us.
    std::optional<uint32_t> last_server_hash() const;
    bool set_last_server_hash(uint32_t hash);

    /// This player's static delay in milliseconds.
    std::optional<uint16_t> static_delay_ms() const;
    bool set_static_delay_ms(uint16_t delay_ms);

    /// The last applied gain, 0-100; volume and mute can each be absent independently.
    std::optional<uint8_t> volume() const;
    std::optional<bool> muted() const;

    /// Records volume and mute in one write, so a kill cannot split the pair.
    bool set_volume_and_muted(uint8_t volume, bool muted);

private:
    /// The value for `key`, or nothing when it is absent.
    std::optional<std::string> get(const std::string& key) const;

    /// Records every pair in one write, skipping it when nothing changes; rolls back on failure.
    bool set_all(const std::vector<std::pair<std::string, std::string>>& pairs);

    /// Writes every held key to `path_`, atomically and at mode 0600.
    bool write() const;

    /// A whole number up to `limit`, or nothing when absent or out of range.
    std::optional<uint64_t> get_number(const std::string& key, uint64_t limit) const;

    std::string path_;
    std::map<std::string, std::string> values_;
};

}  // namespace sendspin_cli
