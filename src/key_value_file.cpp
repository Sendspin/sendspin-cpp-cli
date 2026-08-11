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

#include "key_value_file.h"

#include <cstdio>
#include <string>
#include <vector>

namespace sendspin_cli {

namespace {

constexpr const char* BLANKS = " \t";

/// `text` without leading or trailing spaces and tabs.
std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(BLANKS);
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(BLANKS) - first + 1);
}

}  // namespace

KeyValueStatus read_key_value_file(const std::string& path, std::vector<KeyValueEntry>& entries,
                                   size_t& malformed_line) {
    entries.clear();
    if (path.empty()) {
        return KeyValueStatus::Unreadable;
    }

    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return KeyValueStatus::Unreadable;
    }

    // One byte spare so a line exactly at the bound is read whole rather than looking over-long.
    char buffer[MAX_KEY_VALUE_LINE_BYTES + 2] = {};
    size_t number = 0;
    KeyValueStatus status = KeyValueStatus::Ok;

    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), file) != nullptr) {
        ++number;
        std::string line(buffer);

        // A line that filled the buffer with no '\n' is either over-long or the last line of a
        // file with no trailing newline. Only the first is a problem, and the missing '\n' is
        // what tells them apart -- so the length is tested rather than the terminator.
        const bool complete = !line.empty() && line.back() == '\n';
        if (!complete && line.size() > MAX_KEY_VALUE_LINE_BYTES) {
            malformed_line = number;
            status = KeyValueStatus::Malformed;
            break;
        }
        if (complete) {
            line.pop_back();
        }
        // A file written on Windows, or copied through something that rewrote its line endings.
        // Stripped rather than refused: the content is unambiguous, and a value with a stray
        // '\r' on the end fails much further away, as an unopenable path or an unknown device.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string content = trimmed(line);
        if (content.empty() || content.front() == '#') {
            continue;
        }

        const size_t equals = content.find('=');
        if (equals == std::string::npos) {
            malformed_line = number;
            status = KeyValueStatus::Malformed;
            break;
        }
        KeyValueEntry entry;
        entry.key = trimmed(content.substr(0, equals));
        entry.value = trimmed(content.substr(equals + 1));
        entry.line = number;
        if (entry.key.empty()) {
            malformed_line = number;
            status = KeyValueStatus::Malformed;
            break;
        }
        entries.push_back(std::move(entry));
    }

    std::fclose(file);
    if (status != KeyValueStatus::Ok) {
        // Nothing partial is handed back: a caller that refuses the file must not also be able
        // to act on the half of it that parsed.
        entries.clear();
    }
    return status;
}

}  // namespace sendspin_cli
