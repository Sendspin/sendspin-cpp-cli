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

/// Tagged stderr logging gated on the sendspin library's global log level.

#pragma once

#include <sendspin/client.h>

#include <array>
#include <cstdio>
#include <string>

namespace sendspin_cli {

/// The tags sendspin-cli's own log lines carry.
inline constexpr const char* LOG_TAG_CLI = "cli";
inline constexpr const char* LOG_TAG_AUDIO = "audio";
inline constexpr const char* LOG_TAG_MDNS = "mdns";
inline constexpr const char* LOG_TAG_DISCOVERY = "discovery";
inline constexpr const char* LOG_TAG_OUTBOUND = "outbound";
inline constexpr const char* LOG_TAG_PLAYER = "player";
inline constexpr const char* LOG_TAG_METADATA = "metadata";
inline constexpr const char* LOG_TAG_CONTROL = "control";
inline constexpr const char* LOG_TAG_HOOK = "hook";

/// Every tag above, for diagnostics that list them.
inline constexpr auto LOG_TAGS = std::to_array({
    LOG_TAG_CLI,
    LOG_TAG_AUDIO,
    LOG_TAG_MDNS,
    LOG_TAG_DISCOVERY,
    LOG_TAG_OUTBOUND,
    LOG_TAG_PLAYER,
    LOG_TAG_METADATA,
    LOG_TAG_CONTROL,
    LOG_TAG_HOOK,
});

/// Writes `<L> <tag>: <message>` to stderr if `level` passes the library's level.
/// Prefer cli_log(), which supplies the file's tag.
__attribute__((format(printf, 3, 4))) void log_line(sendspin::LogLevel level, const char* tag,
                                                    const char* fmt, ...);

/// Logs a fatal startup failure at ERROR under the failing subsystem's tag, past the level gate.
__attribute__((format(printf, 2, 3))) void log_fatal(const char* tag, const char* fmt, ...);

/// Points stderr at `path`, appending, and timestamps every line. Call before -z forks.
/// @return true if `path` now carries the log; on false stderr is untouched.
bool log_to_file(const std::string& path);

/// SIGHUP handler that flags the -f logfile for reopening. Install only when -f was given.
void log_handle_sighup(int sig);

/// Reopens the -f logfile if a SIGHUP asked for it. Call from the main loop.
void log_reopen_if_requested();

}  // namespace sendspin_cli

/// Logs under the calling file's `static constexpr const char* LOG_TAG`.
#define cli_log(level, ...) ::sendspin_cli::log_line((level), LOG_TAG, __VA_ARGS__)
