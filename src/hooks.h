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

/// Shell commands run on stream start and stop (--hook-start / --hook-stop).

#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Connection facts exported to a hook; empty fields are left out of the environment.
struct HookContext {
    std::string server_id;    ///< SENDSPIN_SERVER_ID: the connected server's id
    std::string server_name;  ///< SENDSPIN_SERVER_NAME: its friendly name
    /// SENDSPIN_SERVER_URL: the URL an -s run dialled, only for the server it dialled.
    std::string server_url;
    std::string client_id;    ///< SENDSPIN_CLIENT_ID: this player's id, when --id chose one
    std::string client_name;  ///< SENDSPIN_CLIENT_NAME: this player's friendly name
};

/// Runs hooks as `/bin/sh -c`, one at a time in event order, without blocking the caller.
/// While one runs, only the newest event waits. Every method must be called on the main loop.
class HookRunner {
public:
    /// Runs `command` with SENDSPIN_EVENT and `context`, or queues it behind a running hook.
    /// @param event "start" or "stop".
    void run(const std::string& command, const char* event, const HookContext& context);

    /// Reaps finished hooks, logging failures, and spawns the pending event when free.
    void poll();

    /// Spawns the pending event now, even beside a running hook. For shutdown.
    void flush();

    /// Hooks spawned and not yet reaped. For tests.
    size_t running() const {
        return this->running_.size();
    }

private:
    /// One spawned hook: the pid to reap, and what to call it when it fails.
    struct RunningHook {
        pid_t pid;
        std::string event;
    };

    /// The event waiting for the running hook to finish, newest wins.
    struct PendingHook {
        std::string command;
        std::string event;
        HookContext context;
    };

    /// Forks and execs one hook unconditionally.
    void spawn(const std::string& command, const char* event, const HookContext& context);

    std::vector<RunningHook> running_;
    std::optional<PendingHook> pending_;
};

}  // namespace sendspin_cli
