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

/// @file log.h
/// @brief Tagged stderr logging gated on the sendspin library's global log level

#pragma once

#include <sendspin/client.h>

#include <array>
#include <cstdio>
#include <string>

namespace sendspin_cli {

/// @brief The tags sendspin-cli's own log lines carry.
///
/// One vocabulary rather than a literal per file, because the tag is also the filtering
/// surface: `grep 'I mdns:'` is what -d's per-category message points at, and it has to name
/// a set that really exists. Library lines carry their own `sendspin.<subsystem>` tags and
/// are deliberately not in here -- nothing in this process chooses those.
inline constexpr const char* LOG_TAG_CLI = "cli";
inline constexpr const char* LOG_TAG_AUDIO = "audio";
inline constexpr const char* LOG_TAG_MDNS = "mdns";
inline constexpr const char* LOG_TAG_DISCOVERY = "discovery";
inline constexpr const char* LOG_TAG_OUTBOUND = "outbound";
inline constexpr const char* LOG_TAG_PLAYER = "player";
inline constexpr const char* LOG_TAG_METADATA = "metadata";
inline constexpr const char* LOG_TAG_CONTROL = "control";

/// @brief Every tag above, for the diagnostics that have to list them.
///
/// Built from the constants rather than written out again, so a tag cannot exist in the log
/// and be missing from what -d tells the user to grep for. Deduced rather than sized, so the
/// count cannot drift either.
inline constexpr auto LOG_TAGS = std::to_array({
    LOG_TAG_CLI,
    LOG_TAG_AUDIO,
    LOG_TAG_MDNS,
    LOG_TAG_DISCOVERY,
    LOG_TAG_OUTBOUND,
    LOG_TAG_PLAYER,
    LOG_TAG_METADATA,
    LOG_TAG_CONTROL,
});

/// @brief Writes one tagged line to stderr if `level` passes the library's current level.
///
/// The line is `<L> <tag>: <message>`, with an RFC 3339 UTC timestamp in front of it once
/// log_to_file() has run. That tail is deliberately byte-identical to what the library's own
/// SS_LOG* macros emit, so `grep 'I mdns:'` and `grep 'I sendspin.player:'` both work on the
/// same file and nothing has to know which half of the binary wrote a line.
///
/// The level gate is SendspinClient's global one, so a single -d controls our lines and the
/// library's together; stderr rather than stdout so `-o stdout` can carry PCM.
///
/// Prefer the cli_log() macro below, which supplies the calling file's tag. Call this
/// directly only in a file that logs under more than one tag.
__attribute__((format(printf, 3, 4))) void log_line(sendspin::LogLevel level, const char* tag,
                                                    const char* fmt, ...);

/// @brief Reports a fatal startup failure, in the log's format but past the level gate.
///
/// For the errors that stop the player coming up: a locked pidfile, a device that will not
/// open, a server that will not start. Two properties it needs at once, which is why it is
/// not `cli_log`:
///
/// - **Ungated.** `-d none` means "do not narrate", not "exit 1 without saying why".
/// - **Formatted like every other line.** Under -f these are the most important lines in the
///   file, so they have to be the ones `grep 'E '` finds, and they have to carry a timestamp.
///
/// Emitted at ERROR under the tag of the **subsystem that failed**, not of the startup phase
/// -- a device that will not open is `audio`, so it sits in one greppable thread with every
/// other line about that device.
///
/// Two kinds of diagnostic deliberately stay plain `error:` lines on their own stream: the
/// flag parser's, and the pre-fork pidfile probe's. Both run before there is a log to write
/// to, and both answer a command line rather than recording a run.
__attribute__((format(printf, 2, 3))) void log_fatal(const char* tag, const char* fmt, ...);

/// @brief Points stderr -- where all logging goes -- at `path`, and timestamps every line.
///
/// Appends, so a restart does not truncate history. Called before -z forks, which is what
/// makes an unopenable path fail at the terminal rather than in a log nobody can read.
///
/// Only a logfile is stamped: a foreground run under systemd or Docker already gets a
/// timestamp from journald or the container runtime, and a second one would only be noise.
///
/// Implemented with open() and dup2() rather than freopen(), which buys three things: a
/// failure leaves stderr intact, so the complaint about it goes to stderr like every other
/// diagnostic instead of having to be diverted to stdout; a failed *reopen* leaves the log on
/// the descriptor it already had, so it can report itself; and there is no window where
/// stderr is a closed stream that a later log line would write through.
/// @return true if `path` is now carrying the log. On false, stderr is untouched.
bool log_to_file(const std::string& path);

/// @brief SIGHUP handler that flags the -f logfile for reopening.
///
/// Install this only when -f was given. SIGHUP's default disposition is terminate, which is
/// what a foreground run whose terminal has just closed should keep doing -- staying alive
/// with stderr on a dead pty is worse than exiting.
void log_handle_sighup(int sig);

/// @brief Reopens the -f logfile if a SIGHUP asked for it. Call from the main loop.
///
/// This is the whole of the rotation story: `logrotate` and `newsyslog` move the file and
/// send SIGHUP, and the next tick reopens the original path.
///
/// The handler only sets a flag because the reopen flushes the old stream and then logs the
/// result, and neither `fflush()` nor `fprintf()` is async-signal-safe. The `open`/`dup2` pair
/// at the heart of it would be.
void log_reopen_if_requested();

}  // namespace sendspin_cli

/// @brief Logs one line under the calling translation unit's tag.
///
/// Every .cpp that logs defines `static constexpr const char* LOG_TAG` near the top --
/// mirroring the library's own per-file `TAG` idiom -- so a tag is chosen once per file
/// instead of threaded through every call site. Deliberately a macro rather than a function:
/// only the preprocessor can reach a name that is private to the file doing the call.
#define cli_log(level, ...) ::sendspin_cli::log_line((level), LOG_TAG, __VA_ARGS__)
