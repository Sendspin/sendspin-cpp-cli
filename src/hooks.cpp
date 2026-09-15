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

/// Prefix of every hook variable; inherited ones are cleared first.
constexpr const char* ENV_PREFIX = "SENDSPIN_";

/// Cap on the child's close loop, for an unlimited or huge descriptor limit.
constexpr rlim_t FD_CLOSE_CAP = 4096;

/// Where the child's close loop stops; call before the fork.
int fd_close_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return static_cast<int>(FD_CLOSE_CAP);
    }
    return static_cast<int>(std::min(limit.rlim_cur, FD_CLOSE_CAP));
}

/// Appends `name=value` to `env`, or nothing when the value is empty.
void add_env(std::vector<std::string>& env, const char* name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    env.push_back(std::string(name) + "=" + value);
}

/// Our environment minus inherited SENDSPIN_* variables, plus the event's own.
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
    if (!this->running_.empty()) {
        if (this->pending_.has_value()) {
            cli_log(LogLevel::DEBUG, "The queued %s hook was superseded by the %s event",
                    this->pending_->event.c_str(), event);
        }
        this->pending_ = PendingHook{command, event, context};
        return;
    }
    this->spawn(command, event, context);
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
            // Interrupted: the hook is still running, so ask again next poll().
            ++index;
            continue;
        }
        if (reaped < 0) {
            // ECHILD: something else reaped it, so drop the entry.
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

    if (this->running_.empty() && this->pending_.has_value()) {
        const PendingHook next = *this->pending_;
        this->pending_.reset();
        this->spawn(next.command, next.event.c_str(), next.context);
    }
}

void HookRunner::flush() {
    if (!this->pending_.has_value()) {
        return;
    }
    if (!this->running_.empty()) {
        cli_log(LogLevel::WARN, "The %s hook has not finished -- running the %s hook beside it",
                this->running_.front().event.c_str(), this->pending_->event.c_str());
    }
    const PendingHook next = *this->pending_;
    this->pending_.reset();
    this->spawn(next.command, next.event.c_str(), next.context);
}

void HookRunner::spawn(const std::string& command, const char* event, const HookContext& context) {
    // Built before the fork: the child may only make async-signal-safe calls.
    const std::vector<std::string> env = hook_environment(event, context);
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (const std::string& entry : env) {
        envp.push_back(const_cast<char*>(entry.c_str()));
    }
    envp.push_back(nullptr);

    char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                          const_cast<char*>(command.c_str()), nullptr};

    const int fd_limit = fd_close_limit();

    const pid_t pid = fork();
    if (pid < 0) {
        cli_log(LogLevel::WARN, "Could not run the %s hook: fork: %s", event,
                std::strerror(errno));
        return;
    }
    if (pid == 0) {
        // Async-signal-safe calls only from here. stdout may carry PCM, so point it at stderr.
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            _exit(127);
        }
        // Close the player's descriptors so a hook cannot hold its ports across a restart.
        for (int fd = STDERR_FILENO + 1; fd < fd_limit; ++fd) {
            close(fd);
        }
        // An ignored SIGPIPE survives execve(); give the hook the default.
        std::signal(SIGPIPE, SIG_DFL);
        execve("/bin/sh", argv, envp.data());
        // exec failed; 127 is the shell's "command not found".
        _exit(127);
    }

    // Never log the command: it may carry credentials.
    cli_log(LogLevel::DEBUG, "Running %s hook [%d]", event, static_cast<int>(pid));
    this->running_.push_back({pid, event});
}

}  // namespace sendspin_cli
