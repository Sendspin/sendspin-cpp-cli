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

#include "hooks.h"

#include "log.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

// The environ POSIX promises but no header is required to declare.
extern char** environ;

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_HOOK;

namespace {

/// The prefix every variable a hook is handed carries, and the one inherited variables
/// under it are cleared from.
constexpr const char* ENV_PREFIX = "SENDSPIN_";

/// How far the child's close loop runs when the descriptor limit is unlimited or absurd.
///
/// The loop is on the stream path, once per hook, and `LimitNOFILE=infinity` under systemd
/// would otherwise ask it to close a million descriptors that were never open.
constexpr rlim_t FD_CLOSE_CAP = 4096;

/// Where the child's close loop stops: the soft descriptor limit, or FD_CLOSE_CAP when that is
/// higher. Read before the fork, where getrlimit() is still legal to call.
int fd_close_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return static_cast<int>(FD_CLOSE_CAP);
    }
    return static_cast<int>(std::min(limit.rlim_cur, FD_CLOSE_CAP));
}

/// Appends `name=value` to `env`, or nothing when the value is empty.
///
/// Empty means "unknown for this event", and an unknown exported as "" would make
/// `[ -n "$SENDSPIN_SERVER_ID" ]` lie to the hook.
void add_env(std::vector<std::string>& env, const char* name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    env.push_back(std::string(name) + "=" + value);
}

/// The environment the hook runs with: ours, minus stale SENDSPIN_* variables, plus the
/// event's own.
///
/// Inherited SENDSPIN_* variables are dropped rather than left to shadow: this player is
/// the authority on what they mean, and one inherited from a wrapper script would describe
/// some other run -- precisely the confusion an unset variable does not cause.
std::vector<std::string> hook_environment(const char* event, const HookContext& context) {
    std::vector<std::string> env;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        if (std::strncmp(*entry, ENV_PREFIX, std::strlen(ENV_PREFIX)) == 0) {
            continue;
        }
        env.emplace_back(*entry);
    }
    add_env(env, "SENDSPIN_EVENT", event);
    add_env(env, "SENDSPIN_SERVER_ID", context.server_id);
    add_env(env, "SENDSPIN_SERVER_NAME", context.server_name);
    add_env(env, "SENDSPIN_SERVER_URL", context.server_url);
    add_env(env, "SENDSPIN_CLIENT_ID", context.client_id);
    add_env(env, "SENDSPIN_CLIENT_NAME", context.client_name);
    return env;
}

}  // namespace

void HookRunner::run(const std::string& command, const char* event, const HookContext& context) {
    // Built before the fork, because after it almost nothing is legal: this process has the
    // library's background threads, so the child may only call async-signal-safe functions
    // between fork() and execve() -- which allocation, and so std::string, is not.
    const std::vector<std::string> env = hook_environment(event, context);
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const std::string& entry : env) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                          const_cast<char*>(command.c_str()), nullptr};

    // Read here for the same reason the environment is built here: getrlimit() is not on the
    // async-signal-safe list, and the close loop in the child needs the answer.
    const int fd_limit = fd_close_limit();

    const pid_t pid = fork();
    if (pid < 0) {
        cli_log(LogLevel::WARN, "Could not run the %s hook: fork: %s", event,
                std::strerror(errno));
        return;
    }
    if (pid == 0) {
        // Every call below is async-signal-safe, which after the fork is the bar: this process
        // has the library's background threads, and the child holds whatever they were holding.
        //
        // The player's stdout may be carrying PCM (-o stdout), so the hook's is pointed at
        // stderr -- which under -f is the logfile, where a hook's output belongs anyway.
        // Checked, because an unnoticed failure here is a hook's `echo` in the middle of the
        // audio -- the one thing this line exists to prevent.
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            _exit(127);
        }
        // Everything above stderr belongs to the player, not to the hook: the audio port's
        // listening socket, the control socket and the peers it has accepted, the connection to
        // the server. A hook that kept them holds the port a restart needs for as long as it
        // runs -- the restart then logs "Address already in use" and accepts nothing -- and
        // leaves a `status` reader waiting on an EOF that can no longer come. Closed here
        // rather than opened close-on-exec at each site, because the sockets the library and
        // its websocket dependency open are not this process's to reach into -- unlike the
        // pidfile and the logfile, which are opened O_CLOEXEC where they are opened.
        for (int fd = STDERR_FILENO + 1; fd < fd_limit; ++fd) {
            close(fd);
        }
        // The player ignores SIGPIPE, and an ignored disposition survives execve() where a
        // caught one does not. A hook that inherited it would find `... | head -1` failing on a
        // write where every other shell on the box ends the writer instead.
        std::signal(SIGPIPE, SIG_DFL);
        execve("/bin/sh", argv, envp.data());
        // Only reached when the exec itself failed. 127 is the shell's own "command not
        // found", so the reap below reports it like any other failing hook.
        _exit(127);
    }

    // The command itself is never logged, at any level: it is an arbitrary shell line, so it
    // routinely carries a credential -- a bearer token in a `curl -H`, a key in a query string
    // -- and under -f the log is a file on disk. Whoever set the hook already knows the text;
    // the event and pid identify the run.
    cli_log(LogLevel::DEBUG, "Running %s hook [%d]", event, static_cast<int>(pid));
    this->running_.push_back({pid, event});
}

void HookRunner::poll() {
    for (size_t index = 0; index < this->running_.size();) {
        const RunningHook& hook = this->running_[index];
        int status = 0;
        const pid_t reaped = waitpid(hook.pid, &status, WNOHANG);
        if (reaped == 0) {
            ++index;
            continue;
        }
        if (reaped < 0 && errno == EINTR) {
            // The signal landed on the call, not on the child: the hook is still running, and
            // the next poll() asks again. Every handler this daemon installs restarts its call
            // -- std::signal()'s BSD semantics for SIGINT and SIGTERM, SA_RESTART for SIGHUP --
            // so this is a guard rather than a path taken. Erasing here instead would leak a
            // zombie per stream, with nothing in the log to say why.
            ++index;
            continue;
        }
        if (reaped < 0) {
            // ECHILD is the one that reaches this, and a child that cannot be waited on can
            // only be leaked, not re-polled -- so the entry goes. Said out loud because it
            // means something else reaped the hook, which is worth a breadcrumb; DEBUG because
            // the hook itself ran and there is nothing an operator can do about it.
            cli_log(LogLevel::DEBUG, "The %s hook [%d] could not be waited on (%s)",
                    hook.event.c_str(), static_cast<int>(hook.pid), std::strerror(errno));
        }
        if (reaped == hook.pid && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            cli_log(LogLevel::WARN, "The %s hook [%d] exited %d", hook.event.c_str(),
                    static_cast<int>(hook.pid), WEXITSTATUS(status));
        } else if (reaped == hook.pid && WIFSIGNALED(status)) {
            cli_log(LogLevel::WARN, "The %s hook [%d] was killed by signal %d",
                    hook.event.c_str(), static_cast<int>(hook.pid), WTERMSIG(status));
        } else if (reaped == hook.pid) {
            cli_log(LogLevel::DEBUG, "The %s hook [%d] finished", hook.event.c_str(),
                    static_cast<int>(hook.pid));
        }
        this->running_.erase(this->running_.begin() + static_cast<ptrdiff_t>(index));
    }
}

}  // namespace sendspin_cli
