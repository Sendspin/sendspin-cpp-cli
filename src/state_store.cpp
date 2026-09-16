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

#include "state_store.h"

#include "key_value_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

namespace {

constexpr const char* STATE_SUBDIR = "sendspin-cli";
constexpr const char* STATE_FILE = "state";

constexpr const char* KEY_LAST_SERVER = "last-server";
constexpr const char* KEY_LAST_SERVER_HASH = "last-server-hash";
constexpr const char* KEY_STATIC_DELAY_MS = "static-delay-ms";
constexpr const char* KEY_VOLUME = "volume";
constexpr const char* KEY_MUTED = "muted";

/// An environment variable's value, or empty when it is unset or set to nothing.
std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

/// Everything up to the last '/', or empty when there is none.
std::string parent_directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace

std::string state_store_path(const std::string& state_dir) {
    if (!state_dir.empty()) {
        return state_dir + "/" + STATE_FILE;
    }
    std::string base = env_or_empty("XDG_STATE_HOME");
    if (base.empty()) {
        const std::string home = env_or_empty("HOME");
        if (home.empty()) {
            return {};
        }
        base = home + "/.local/state";
    }
    return base + "/" + STATE_SUBDIR + "/" + STATE_FILE;
}

StateStore::StateStore(std::string path) : path_(std::move(path)) {}

StateLoadResult StateStore::load(size_t& malformed_line) {
    this->values_.clear();
    std::vector<KeyValueEntry> entries;
    switch (read_key_value_file(this->path_, entries, malformed_line)) {
        case KeyValueStatus::Unreadable:
            return StateLoadResult::Absent;
        case KeyValueStatus::Malformed:
            return StateLoadResult::Corrupt;
        case KeyValueStatus::Ok:
            break;
    }
    // Last wins over entries in file order.
    for (KeyValueEntry& entry : entries) {
        this->values_[entry.key] = std::move(entry.value);
    }
    return StateLoadResult::Loaded;
}

std::string StateStore::last_server() const {
    return this->get(KEY_LAST_SERVER).value_or(std::string());
}

bool StateStore::set_last_server(const std::string& server_id) {
    if (server_id.empty()) {
        return false;
    }
    return this->set_all({{KEY_LAST_SERVER, server_id}});
}

std::optional<uint32_t> StateStore::last_server_hash() const {
    const std::optional<uint64_t> value = this->get_number(KEY_LAST_SERVER_HASH, UINT32_MAX);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

bool StateStore::set_last_server_hash(uint32_t hash) {
    return this->set_all({{KEY_LAST_SERVER_HASH, std::to_string(hash)}});
}

std::optional<uint16_t> StateStore::static_delay_ms() const {
    const std::optional<uint64_t> value = this->get_number(KEY_STATIC_DELAY_MS, UINT16_MAX);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(*value);
}

bool StateStore::set_static_delay_ms(uint16_t delay_ms) {
    return this->set_all({{KEY_STATIC_DELAY_MS, std::to_string(delay_ms)}});
}

std::optional<uint8_t> StateStore::volume() const {
    const std::optional<uint64_t> value = this->get_number(KEY_VOLUME, 100);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<uint8_t>(*value);
}

std::optional<bool> StateStore::muted() const {
    const std::optional<std::string> value = this->get(KEY_MUTED);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value == "true") {
        return true;
    }
    if (*value == "false") {
        return false;
    }
    return std::nullopt;
}

bool StateStore::set_volume_and_muted(uint8_t volume, bool muted) {
    return this->set_all({{KEY_VOLUME, std::to_string(static_cast<unsigned>(volume))},
                          {KEY_MUTED, muted ? "true" : "false"}});
}

std::optional<std::string> StateStore::get(const std::string& key) const {
    const auto found = this->values_.find(key);
    if (found == this->values_.end() || found->second.empty()) {
        return std::nullopt;
    }
    return found->second;
}

bool StateStore::set_all(const std::vector<std::pair<std::string, std::string>>& pairs) {
    // Previous values, absent kept distinct from empty, so a failed write can roll back.
    std::vector<std::pair<std::string, std::optional<std::string>>> previous;
    bool changed = false;
    for (const auto& [key, value] : pairs) {
        const auto found = this->values_.find(key);
        previous.emplace_back(key, found == this->values_.end()
                                       ? std::nullopt
                                       : std::optional<std::string>(found->second));
        changed = changed || found == this->values_.end() || found->second != value;
    }
    if (!changed) {
        return true;
    }

    for (const auto& [key, value] : pairs) {
        this->values_[key] = value;
    }
    if (this->write()) {
        return true;
    }
    // Roll back so the short-circuit above cannot report a write that never happened.
    for (const auto& [key, value] : previous) {
        if (value.has_value()) {
            this->values_[key] = *value;
        } else {
            this->values_.erase(key);
        }
    }
    return false;
}

std::optional<uint64_t> StateStore::get_number(const std::string& key, uint64_t limit) const {
    const std::optional<std::string> value = this->get(key);
    if (!value.has_value() || value->find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    const unsigned long long parsed = std::strtoull(value->c_str(), nullptr, 10);
    if (parsed > limit) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(parsed);
}

bool StateStore::write() const {
    if (this->path_.empty()) {
        return false;
    }

    // Only the leaf directory is created; a mistyped parent should fail visibly.
    const std::string directory = parent_directory(this->path_);
    if (!directory.empty() && mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }

    // Pid in the name, so two players sharing a state dir cannot rename each other's temp file.
    const std::string temporary = this->path_ + ".tmp." + std::to_string(getpid());
    // Remove a stale temp so O_EXCL creates the file with our mode.
    std::remove(temporary.c_str());

    // 0600 at creation, so the file is never readable more widely.
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return false;
    }
    std::FILE* file = ::fdopen(fd, "w");
    if (file == nullptr) {
        ::close(fd);
        std::remove(temporary.c_str());
        return false;
    }

    bool ok = std::fprintf(file, "# Written by sendspin-cli. Edits are overwritten.\n") > 0;
    for (const auto& [key, value] : this->values_) {
        if (!ok) {
            break;
        }
        ok = std::fprintf(file, "%s = %s\n", key.c_str(), value.c_str()) > 0;
    }
    // fsync before rename, or a power cut can leave an empty file in place.
    if (ok) {
        ok = std::fflush(file) == 0 && ::fsync(fd) == 0;
    }
    // fclose also flushes, so a full disk can surface here.
    ok = std::fclose(file) == 0 && ok;

    if (!ok || std::rename(temporary.c_str(), this->path_.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace sendspin_cli
