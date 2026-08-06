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
/// @brief Stderr logging gated on the sendspin library's global log level

#pragma once

#include <sendspin/client.h>

#include <cstdarg>
#include <cstdio>

namespace sendspin_cli {

/// @brief Logs one line to stderr if `level` passes the library's current log level.
///
/// Deliberately reuses SendspinClient's global level so a single -d controls our
/// messages and the library's together, and stderr so that -o stdout can carry PCM.
/// The real logging framework -- per-category levels, syslog, timestamps -- is a
/// follow-up task; see docs/ROADMAP.md.
__attribute__((format(printf, 2, 3))) inline void cli_log(sendspin::LogLevel level,
                                                          const char* fmt, ...) {
    if (sendspin::SendspinClient::get_log_level() < level) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

}  // namespace sendspin_cli
