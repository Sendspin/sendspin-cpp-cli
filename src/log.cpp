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

/// The -f path, empty while logging to stderr. Written once, before any thread starts.
std::string g_logfile;

/// Set by the SIGHUP handler, cleared on the main loop.
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
    return '?';
}

/// Writes a UTC `2026-08-10T03:14:15Z ` stamp into `out`, or leaves it empty on stderr.
void write_timestamp(char* out, size_t size) {
    out[0] = '\0';
    if (g_logfile.empty()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    // gmtime_r: sinks log from the sync task's thread.
    if (::gmtime_r(&now, &utc) == nullptr) {
        return;
    }
    if (std::strftime(out, size, "%Y-%m-%dT%H:%M:%SZ ", &utc) == 0) {
        out[0] = '\0';
    }
}

/// Writes one whole line as a single fprintf, so threads cannot interleave within it.
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
/// @return true on success; on false errno is the reason.
bool point_stderr_at(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::fflush(stderr);
    const bool ok = ::dup2(fd, STDERR_FILENO) >= 0;
    // Saved before close(), which may set errno even on success.
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
    // No level gate: -d none must not hide why the player did not start.
    va_list args;
    va_start(args, fmt);
    emit(sendspin::LogLevel::ERROR, tag, fmt, args);
    va_end(args);
}

bool log_to_file(const std::string& path) {
    if (!point_stderr_at(path)) {
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
        return;
    }

    // dup2() swaps fd 2 atomically and each line is one O_APPEND write, so no line is split.
    if (point_stderr_at(g_logfile)) {
        cli_log(sendspin::LogLevel::INFO, "Reopened %s on SIGHUP", g_logfile.c_str());
        return;
    }
    log_fatal(LOG_TAG, "cannot reopen %s on SIGHUP, still logging to the previous file: %s",
              g_logfile.c_str(), std::strerror(errno));
}

}  // namespace sendspin_cli
