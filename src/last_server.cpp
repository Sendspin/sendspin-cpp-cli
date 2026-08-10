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

#include "last_server.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace sendspin_cli {

namespace {

constexpr const char* STATE_SUBDIR = "sendspin-cli";
constexpr const char* STATE_FILE = "last-server";

/// The longest id this will read back, as a sanity bound rather than a protocol limit: the
/// file is ours, but it lives in a directory anything can write to.
constexpr size_t MAX_SERVER_ID_BYTES = 512;

/// An environment variable's value, or empty when it is unset or set to nothing.
///
/// The XDG spec treats an empty variable as unset, and so does this: `XDG_STATE_HOME=` would
/// otherwise resolve to a path starting at the filesystem root.
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

std::string last_server_path() {
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

bool load_last_server(const std::string& path, std::string& server_id) {
    server_id.clear();
    if (path.empty()) {
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return false;
    }

    char buffer[MAX_SERVER_ID_BYTES + 1] = {};
    const size_t read = std::fread(buffer, 1, MAX_SERVER_ID_BYTES, file);
    std::fclose(file);

    std::string text(buffer, read);
    // One id per file, written with a trailing newline so the file is readable with `cat`.
    const size_t end = text.find_first_of("\r\n");
    if (end == std::string::npos && read >= MAX_SERVER_ID_BYTES) {
        // The whole buffer with no line ending means the id was longer than this will read.
        // Returning the truncated prefix would be worse than returning nothing: it can never
        // match a browsed instance, so the memory would look present and silently never work.
        return false;
    }
    if (end != std::string::npos) {
        text.erase(end);
    }
    if (text.empty()) {
        return false;
    }

    server_id = std::move(text);
    return true;
}

bool save_last_server(const std::string& path, const std::string& server_id) {
    if (path.empty() || server_id.empty()) {
        return false;
    }

    // Only the leaf directory is created: `$XDG_STATE_HOME` and `~/.local/state` are the
    // caller's own to provide, and silently building a whole tree under a mistyped variable
    // would scatter directories rather than fail visibly.
    const std::string directory = parent_directory(path);
    if (!directory.empty() && mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return false;
    }
    const bool written = std::fprintf(file, "%s\n", server_id.c_str()) > 0;
    // fclose is what flushes, so a full disk shows up here rather than in fprintf.
    return std::fclose(file) == 0 && written;
}

}  // namespace sendspin_cli
