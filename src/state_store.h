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

/// @file state_store.h
/// @brief What the daemon remembers about itself across restarts
///
/// One flat `key = value` file, written only by the daemon and never meant to be hand-edited --
/// which is what keeps it a separate file from the operator's config (`src/config_file.h`). A
/// daemon that rewrote its own config would destroy the comments and the ordering someone put
/// there; a config the daemon could not write would have nowhere to record a volume.
///
/// The two remembered server keys mean different things and are deliberately not reconciled with
/// each other. `last-server` is the server *id*, which mDNS discovery compares against a browsed
/// instance to break a tie between candidates. `last-server-hash` is the opaque `uint32_t` the
/// library hands us through `SendspinPersistenceProvider`, which it uses to prefer the
/// last-played server among *inbound* connections -- an FNV1 hash produced by
/// `ConnectionManager::fnv1_hash()`, which lives in the library's uninstalled `src/`. So we store
/// what we are given and hand it back, and never compute it: reimplementing a private hash and
/// staying bit-compatible with it across `SENDSPIN_GIT_TAG` bumps is not a thing to depend on.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace sendspin_cli {

/// @brief Where the state file goes, or empty when there is nowhere to put it.
///
/// `<state_dir>/state` when `--state-dir` named one, otherwise
/// `$XDG_STATE_HOME/sendspin-cli/state`, falling back to
/// `$HOME/.local/state/sendspin-cli/state`.
///
/// `--state-dir` wins outright rather than being another fallback, and it earns its place the
/// way `--no-control` does: a systemd *system* unit has neither variable, and gets
/// `/var/lib/sendspin-cli` from `StateDirectory=`. A process with none of the three -- most
/// system services as they stand -- gets an empty path and simply does not remember.
///
/// @param state_dir `--state-dir`'s value, empty when it was not given.
std::string state_store_path(const std::string& state_dir);

/// @brief The daemon's memory, backed by one file that is rewritten on every change.
///
/// Every setter writes the whole file through a temporary and `rename()`, so a kill mid-write
/// leaves either the old file or the new one and never half of either. That matters more than
/// the cost of rewriting five keys: the alternative is a later run having to reason about a
/// truncated file, on a player whose usual way of stopping is losing power.
///
/// A store with an empty path, or one whose file cannot be written, is not an error. Every
/// setter reports the failure so a caller can say so once, and the player carries on -- having
/// no memory is the normal state on a first run, and a player that cannot remember its volume
/// is still a player.
///
/// THREAD SAFETY: none. Every caller is on the main loop -- the outbound tick, the player
/// listener's volume callbacks, and the library's persistence provider, which it reaches from
/// `update_static_delay()` on that same thread.
class StateStore {
public:
    /// @param path Where the file lives, from state_store_path(). Empty is accepted and means
    /// this run remembers nothing.
    explicit StateStore(std::string path);

    /// @brief Reads whatever is on disk, replacing anything held.
    ///
    /// A missing, unreadable or malformed file leaves the store empty and says so through the
    /// return value; it is never fatal. Unlike the config file, a line that does not parse is
    /// skipped rather than refused -- nothing but this daemon writes here, so a bad line means
    /// the file was corrupted rather than mistyped, and refusing to start over it would strand
    /// a player on something it wrote itself.
    ///
    /// @return true if a file was read at all, whatever it contained.
    bool load();

    const std::string& path() const {
        return this->path_;
    }

    /// @brief The id of the server this player last completed a handshake with, empty if none.
    ///
    /// What mDNS discovery's tie-break compares a browsed instance against, which is why it is
    /// the id rather than the library's hash of it.
    std::string last_server() const;
    bool set_last_server(const std::string& server_id);

    /// @brief The library's opaque last-played-server hash, as handed to us.
    std::optional<uint32_t> last_server_hash() const;
    bool set_last_server_hash(uint32_t hash);

    /// @brief This player's static delay in milliseconds.
    ///
    /// Persisting it is a spec MUST for players: "Clients must persist `static_delay_ms` locally
    /// across reboots and server reconnections". The library does the reading and writing itself
    /// through the persistence provider; this is only where the number lands.
    std::optional<uint16_t> static_delay_ms() const;
    bool set_static_delay_ms(uint16_t delay_ms);

    /// @brief The gain and mute state the sink was last told to apply, 0-100.
    ///
    /// Persisting these is the spec's RECOMMENDED rather than a MUST, and the library has no
    /// provider hook for them, so this half is the CLI's own.
    std::optional<uint8_t> volume() const;
    std::optional<bool> muted() const;
    bool set_volume(uint8_t volume);
    bool set_muted(bool muted);

private:
    /// The value for `key`, or nothing when it is absent.
    std::optional<std::string> get(const std::string& key) const;

    /// Records `key` and rewrites the file, unless the value is already what is being set.
    ///
    /// The short-circuit is not just an optimisation: `on_volume_changed()` fires on every
    /// server volume message, including the ones that repeat the current value, and rewriting
    /// the file for each of those is write amplification on flash for no gain.
    bool set(const std::string& key, const std::string& value);

    /// Writes every held key to `path_`, atomically and at mode 0600.
    bool write() const;

    /// A whole-number value parsed against `limit`, or nothing when it is absent or not one.
    ///
    /// A value out of range reads as absent rather than being clamped: the file is ours, so an
    /// impossible figure in it means something else wrote there, and honouring part of it would
    /// be worse than starting from the default.
    std::optional<uint64_t> get_number(const std::string& key, uint64_t limit) const;

    std::string path_;
    std::map<std::string, std::string> values_;
};

}  // namespace sendspin_cli
