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

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
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

    const pid_t pid = fork();
    if (pid < 0) {
        cli_log(LogLevel::WARN, "Could not run the %s hook: fork: %s", event,
                std::strerror(errno));
        return;
    }
    if (pid == 0) {
        // The player's stdout may be carrying PCM (-o stdout), so the hook's is pointed at
        // stderr -- which under -f is the logfile, where a hook's output belongs anyway.
        // dup2() and execve() are both async-signal-safe, which after the fork is the bar.
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execve("/bin/sh", argv, envp.data());
        // Only reached when the exec itself failed. 127 is the shell's own "command not
        // found", so the reap below reports it like any other failing hook.
        _exit(127);
    }

    cli_log(LogLevel::DEBUG, "Running %s hook [%d]: %s", event, static_cast<int>(pid),
            command.c_str());
    this->running_.push_back({pid, event, command});
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
        // An error from waitpid() is treated as reaped too: the one it returns here is
        // ECHILD, and a child that cannot be waited on can only be leaked, not re-polled.
        if (reaped == hook.pid && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            cli_log(LogLevel::WARN, "The %s hook exited %d: %s", hook.event.c_str(),
                    WEXITSTATUS(status), hook.command.c_str());
        } else if (reaped == hook.pid && WIFSIGNALED(status)) {
            cli_log(LogLevel::WARN, "The %s hook was killed by signal %d: %s",
                    hook.event.c_str(), WTERMSIG(status), hook.command.c_str());
        } else if (reaped == hook.pid) {
            cli_log(LogLevel::DEBUG, "The %s hook finished: %s", hook.event.c_str(),
                    hook.command.c_str());
        }
        this->running_.erase(this->running_.begin() + static_cast<ptrdiff_t>(index));
    }
}

}  // namespace sendspin_cli
