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

#include "log.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace sendspin_cli {

static constexpr const char* LOG_TAG = LOG_TAG_CLI;

namespace {

/// The path -f named, empty while the log is still on stderr.
///
/// Written once, by log_to_file(), before anything in this process starts a thread -- the
/// sinks log from the sync task's thread, so a value that changed later would be a data race.
/// The SIGHUP reopen deliberately does not touch it: it reopens the same path.
std::string g_logfile;

/// Set by the SIGHUP handler and cleared on the main loop. `volatile sig_atomic_t` because
/// that is the one type a handler may write to and the loop may read.
volatile sig_atomic_t g_reopen_requested = 0;

/// How long a formatted line may be before it costs an allocation.
constexpr size_t INLINE_MESSAGE_BYTES = 512;

/// The level letter the library's SS_LOG* macros print, so both halves of the log read alike.
char level_letter(sendspin::LogLevel level) {
    switch (level) {
        case sendspin::LogLevel::ERROR:
            return 'E';
        case sendspin::LogLevel::WARN:
            return 'W';
        case sendspin::LogLevel::INFO:
            return 'I';
        case sendspin::LogLevel::DEBUG:
            return 'D';
        case sendspin::LogLevel::VERBOSE:
            return 'V';
        case sendspin::LogLevel::NONE:
            break;
    }
    // Unreachable through log_line(): NONE never passes the gate, since nothing is logged
    // *at* NONE. A letter rather than nothing, so a future level cannot silently lose its
    // column and shift every field of the line.
    return '?';
}

/// Writes `2026-08-10T03:14:15Z ` into `out`, or leaves it empty when the log is on stderr.
///
/// The trailing space belongs to the stamp, so the caller has one format string either way.
/// UTC rather than local time: a log pulled off a player is usually read somewhere else, and
/// an unqualified local timestamp is the field report that cannot be lined up with anything.
void write_timestamp(char* out, size_t size) {
    out[0] = '\0';
    if (g_logfile.empty()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    // gmtime_r rather than gmtime: the sinks log from the sync task's thread, and gmtime
    // returns a pointer into shared storage.
    if (::gmtime_r(&now, &utc) == nullptr) {
        return;
    }
    if (std::strftime(out, size, "%Y-%m-%dT%H:%M:%SZ ", &utc) == 0) {
        out[0] = '\0';
    }
}

/// Writes one whole line. Whether the level passes is the caller's decision.
///
/// Formatted in full before anything is written, so one log line is one fprintf: our lines
/// come from the main loop and from the sync task's thread, and a line assembled in several
/// writes could have another thread's line land inside it.
__attribute__((format(printf, 3, 0))) void emit(sendspin::LogLevel level, const char* tag,
                                                const char* fmt, va_list args) {
    char inline_message[INLINE_MESSAGE_BYTES];
    std::vector<char> long_message;
    const char* message = inline_message;

    va_list retry;
    va_copy(retry, args);
    const int needed = std::vsnprintf(inline_message, sizeof(inline_message), fmt, args);
    if (needed < 0) {
        // vsnprintf failed outright, which leaves the buffer's contents unspecified.
        message = "(this log line could not be formatted)";
    } else if (static_cast<size_t>(needed) >= sizeof(inline_message)) {
        // A device list or a format digest can outrun the buffer, and a truncated log line is
        // exactly the quiet loss the rest of this file exists to avoid.
        long_message.resize(static_cast<size_t>(needed) + 1);
        std::vsnprintf(long_message.data(), long_message.size(), fmt, retry);
        message = long_message.data();
    }
    va_end(retry);

    char stamp[32];
    write_timestamp(stamp, sizeof(stamp));
    std::fprintf(stderr, "%s%c %s: %s\n", stamp, level_letter(level), tag, message);
    std::fflush(stderr);
}

/// Points fd 2 at `path`, appending, and leaves it alone if the file cannot be opened.
///
/// open() plus dup2() rather than freopen(), which closes the stream even when it fails and
/// would therefore leave stderr dead at the moment something needs to complain about it.
/// @return true on success; on false errno is the reason.
bool point_stderr_at(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    // Anything still buffered belongs to the old destination, not behind whatever is already
    // in the new file.
    std::fflush(stderr);
    const bool ok = ::dup2(fd, STDERR_FILENO) >= 0;
    // Saved before the close(), which is allowed to set errno even when it succeeds, and
    // restored only on the path where the caller is going to read it.
    const int reason = errno;
    ::close(fd);
    if (!ok) {
        errno = reason;
    }
    return ok;
}

}  // namespace

void log_line(sendspin::LogLevel level, const char* tag, const char* fmt, ...) {
    if (sendspin::SendspinClient::get_log_level() < level) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    emit(level, tag, fmt, args);
    va_end(args);
}

void log_fatal(const char* tag, const char* fmt, ...) {
    // No gate: -d none silences the narration of a working player, not the explanation of one
    // that never came up.
    va_list args;
    va_start(args, fmt);
    emit(sendspin::LogLevel::ERROR, tag, fmt, args);
    va_end(args);
}

bool log_to_file(const std::string& path) {
    if (!point_stderr_at(path)) {
        // stderr is still whatever it was, so this reads like any other startup failure and a
        // 2> capture sees it.
        std::fprintf(stderr, "error: cannot open logfile %s: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }
    g_logfile = path;
    return true;
}

void log_handle_sighup(int /*sig*/) {
    g_reopen_requested = 1;
}

void log_reopen_if_requested() {
    if (g_reopen_requested == 0) {
        return;
    }
    g_reopen_requested = 0;
    if (g_logfile.empty()) {
        // No -f, so no handler was installed and nothing can have asked for this.
        return;
    }

    // Not a race against a sink logging from the sync task's thread, though not because of
    // the stdio lock -- that covers the fflush, but dup2() is a raw descriptor operation
    // outside it. What makes it safe is that dup2() replaces fd 2 atomically and each of our
    // lines is a single write() on an O_APPEND descriptor, so a concurrent line lands wholly
    // in the old file or wholly in the new one. The worst case is one line landing in the
    // file that has just been rotated away.
    if (point_stderr_at(g_logfile)) {
        cli_log(sendspin::LogLevel::INFO, "Reopened %s on SIGHUP", g_logfile.c_str());
        return;
    }
    // A failed reopen leaves the old descriptor untouched, so unlike freopen() this can
    // report itself -- into the file logrotate just moved, which is where someone will look.
    log_fatal(LOG_TAG, "cannot reopen %s on SIGHUP, still logging to the previous file: %s",
              g_logfile.c_str(), std::strerror(errno));
}

}  // namespace sendspin_cli
