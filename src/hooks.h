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

/// @file hooks.h
/// @brief Shell commands run on stream start and stop (--hook-start / --hook-stop)

#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief What a hook's environment says about the connection the event happened on.
///
/// Every field is optional: an empty one is left out of the environment entirely rather
/// than exported empty, so a hook can test `[ -n "$SENDSPIN_SERVER_ID" ]` and mean it.
/// The vocabulary is the Python CLI's, deliberately -- a hook script written against
/// `sendspin-cli` there runs here unchanged.
struct HookContext {
    std::string server_id;    ///< SENDSPIN_SERVER_ID: the connected server's id
    std::string server_name;  ///< SENDSPIN_SERVER_NAME: its friendly name
    std::string server_url;   ///< SENDSPIN_SERVER_URL: the URL this run dialled, outbound only
    std::string client_id;    ///< SENDSPIN_CLIENT_ID: this player's id, when one was chosen
    std::string client_name;  ///< SENDSPIN_CLIENT_NAME: this player's friendly name
};

/// @brief Spawns hook commands without blocking, and reaps them from the main loop.
///
/// A hook is `/bin/sh -c <command>` with `SENDSPIN_EVENT` and the context above added to
/// this process's own environment -- a shell rather than an argv split, so `amixer set
/// Master unmute && relay on` is one hook. The spawn returns as soon as the child is
/// forked: a hook that blocks must not stall the audio path, so nothing here waits.
///
/// The child's stdout and stderr both go where the player's stderr goes -- under -f, the
/// logfile -- so whatever a hook prints lands beside the player's own lines. Its stdout is
/// deliberately not inherited: with -o stdout that stream is carrying PCM, and a hook's
/// `echo` would land in the middle of the audio.
///
/// THREAD SAFETY: run() and poll() must both be called on the main loop thread. That is
/// where the stream callbacks that trigger hooks already fire, and it is what lets the
/// bookkeeping below go unsynchronised.
class HookRunner {
public:
    /// @brief Runs `command` with `SENDSPIN_EVENT=<event>` and `context` in its environment.
    ///
    /// Failure to spawn is a WARN, not an error: the stream the event describes is fine,
    /// and the player must keep playing it.
    /// @param event What SENDSPIN_EVENT carries: "start" or "stop".
    void run(const std::string& command, const char* event, const HookContext& context);

    /// @brief Reaps any hooks that have finished, logging the ones that failed.
    ///
    /// Call from the main loop. A hook still running when the daemon exits is left to
    /// finish on its own -- an amplifier half-switched-off is worse than an orphan.
    void poll();

    /// How many hooks have been spawned and not yet reaped. For tests, which need to know
    /// when poll() has seen a child out, and for nothing else.
    size_t running() const {
        return this->running_.size();
    }

private:
    /// One spawned hook: the pid to reap, and what to call it when it fails.
    struct RunningHook {
        pid_t pid;
        std::string event;
        std::string command;
    };

    std::vector<RunningHook> running_;
};

}  // namespace sendspin_cli
