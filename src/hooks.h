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

#include <optional>
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
    /// SENDSPIN_SERVER_URL: the URL this run dialled, on an -s run only.
    ///
    /// What was dialled, not which server answered. A lost connection clears it rather than
    /// letting it describe whatever connects next, and a discovery dial is exported only
    /// when the stream arrived from the server_id it dialled. A literal -s URL is the case
    /// that cannot be checked: -s leaves the inbound listener up, and the library reports
    /// that a connection is up without saying where it came from, so a server that dialled
    /// in while that attempt was outstanding or had failed still reads as the dial.
    /// server_id always describes the connection the stream arrived on.
    std::string server_url;
    std::string client_id;    ///< SENDSPIN_CLIENT_ID: this player's id, when --id chose one
    std::string client_name;  ///< SENDSPIN_CLIENT_NAME: this player's friendly name
};

/// @brief Spawns hook commands without blocking, and reaps them from the main loop.
///
/// A hook is `/bin/sh -c <command>` with `SENDSPIN_EVENT` and the context above added to
/// this process's own environment -- a shell rather than an argv split, so `amixer set
/// Master unmute && relay on` is one hook. run() returns without waiting either way: a
/// hook that blocks must not stall the audio path.
///
/// Hooks run one at a time, in event order. Two events for the same stream would otherwise
/// race in the scheduler, and a start hook that runs long finishing *after* the stop hook
/// of its own stream leaves the amplifier on with the player idle -- the exact state the
/// hooks exist to prevent. While one runs, the newest event waits in a single pending
/// slot; a newer event replaces whatever waits there, because the hardware should end in
/// the *final* state, not replay a stale intermediate. A hook that counts events rather
/// than setting state will see such flapping coalesced away -- each replacement is a
/// `D hook:` line. The slot is also the bound: one event waiting at most, and one child at
/// most until the shutdown flush(), no matter how a hook misbehaves.
///
/// The child's stdout and stderr both go where the player's stderr goes -- under -f, the
/// logfile -- so whatever a hook prints lands beside the player's own lines. Its stdout is
/// deliberately not inherited: with -o stdout that stream is carrying PCM, and a hook's
/// `echo` would land in the middle of the audio.
///
/// Nothing else of the player's crosses into the hook. Every descriptor above stderr is closed
/// -- the listening sockets and the connection to the server are the player's, and a hook
/// holding them keeps a restart from binding its port -- and SIGPIPE goes back to its default,
/// which the player ignores and an exec would otherwise carry through.
///
/// THREAD SAFETY: run(), poll() and flush() must all be called on the main loop thread.
/// That is where the stream callbacks that trigger hooks already fire and where the
/// shutdown path runs, and it is what lets the bookkeeping below go unsynchronised.
class HookRunner {
public:
    /// @brief Runs `command` with `SENDSPIN_EVENT=<event>` and `context` in its environment,
    /// or holds it in the pending slot while an earlier hook is still running.
    ///
    /// Failure to spawn is a WARN, not an error: the stream the event describes is fine,
    /// and the player must keep playing it.
    /// @param event What SENDSPIN_EVENT carries: "start" or "stop".
    /// @param context Copied when the event has to wait: it describes this event's stream,
    /// and the caller's object will already describe the next one by the time the slot is
    /// spawned.
    void run(const std::string& command, const char* event, const HookContext& context);

    /// @brief Reaps any hooks that have finished, logging the ones that failed, and spawns
    /// the pending event once the running hook is out.
    ///
    /// Call from the main loop. A hook still running when the daemon exits is left to
    /// finish on its own -- an amplifier half-switched-off is worse than an orphan.
    void poll();

    /// @brief Spawns the pending event now, beside the running hook if there still is one.
    ///
    /// For the shutdown path, after the stream-end drain: the daemon promises that
    /// stopping it runs the stop hook, and a start hook that never finishes must not be
    /// allowed to turn that promise into an amplifier left on. Ordering is knowingly given
    /// up here -- there is no later event left to order against, and the alternative is
    /// the hook never running at all.
    void flush();

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
    };

    /// The event waiting for the running hook to finish, newest wins.
    struct PendingHook {
        std::string command;
        std::string event;
        HookContext context;
    };

    /// Forks and execs one hook, unconditionally. run() decides whether now is the time.
    void spawn(const std::string& command, const char* event, const HookContext& context);

    std::vector<RunningHook> running_;
    std::optional<PendingHook> pending_;
};

}  // namespace sendspin_cli
