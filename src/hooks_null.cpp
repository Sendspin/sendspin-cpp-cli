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

/// HookRunner for a platform without hooks; cli.cpp refuses --hook-start and --hook-stop there.

#include "hooks.h"

#include "log.h"

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_HOOK;

void HookRunner::run(const std::string& /*command*/, const char* event,
                     const HookContext& /*context*/) {
    cli_log(LogLevel::WARN, "The %s hook was not run: this platform has no hook support", event);
}

void HookRunner::poll() {}

void HookRunner::flush() {}

void HookRunner::spawn(const std::string& /*command*/, const char* /*event*/,
                       const HookContext& /*context*/) {}

}  // namespace sendspin_cli
