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

/// --hook-start/--hook-stop: the flag surface, and what a spawned hook really sees.

#include "hooks.h"

#include "cli.h"
#include "parse_harness.h"
#include "posix_compat.h"
#include "scoped_env.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace sendspin_cli {
namespace {

// The flag surface

TEST(HookFlags, EachFlagSetsItsField) {
    Parse parse({"--hook-start", "amp on", "--hook-stop", "amp off"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().hook_start, "amp on");
    EXPECT_EQ(parse.options().hook_stop, "amp off");
}

TEST(HookFlags, DefaultToNoHook) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().hook_start.empty());
    EXPECT_TRUE(parse.options().hook_stop.empty());
}

TEST(HookFlags, EmptyValuesAreRejected) {
    for (const char* flag : {"--hook-start", "--hook-stop"}) {
        Parse parse({flag, ""});

        EXPECT_FALSE(parse.ok()) << flag << " accepted an empty value";
        EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos) << flag;
        EXPECT_NE(parse.diagnostics().find(flag), std::string::npos) << flag;
    }
}

TEST(HookFlags, AreSettableFromAConfigFile) {
    // Removed before the assertions can throw, so a failure leaves nothing behind.
    const std::string path = "hooks-config-" + std::to_string(getpid());
    {
        std::ofstream config(path);
        config << "hook-start = amp on\nhook-stop = amp off\n";
    }
    Parse parse({}, path);
    std::remove(path.c_str());

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().hook_start, "amp on");
    EXPECT_EQ(parse.options().hook_stop, "amp off");
}

TEST(HookFlags, AWarnedAboutAsDaemonOnlyOnASubcommandRun) {
    Parse parse({"status", "--hook-start", "amp on"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("warning: a subcommand reads only"), std::string::npos);
}

// What a spawned hook really sees

/// Polls `runner` until every spawned hook is reaped; false after `timeout_ms`.
bool drain(HookRunner& runner, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        runner.poll();
        if (runner.running() == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string slurp(const std::string& path) {
    std::ifstream file(path);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

/// A scratch path for a hook to write into, removed when the test ends.
class ScratchFile {
public:
    ScratchFile()
        : path_("hooks-out-" + std::to_string(getpid()) + "-" + std::to_string(next_id())) {}

    ~ScratchFile() {
        std::remove(this->path_.c_str());
    }

    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    const std::string& path() const {
        return this->path_;
    }

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
};

/// Gives a signal a disposition for the length of a test, and puts back what it found.
class ScopedSignal {
public:
    ScopedSignal(int number, void (*handler)(int))
        : number_(number), previous_(std::signal(number, handler)) {}

    ~ScopedSignal() {
        std::signal(this->number_, this->previous_);
    }

    ScopedSignal(const ScopedSignal&) = delete;
    ScopedSignal& operator=(const ScopedSignal&) = delete;

private:
    int number_;
    void (*previous_)(int);
};

TEST(HookRunner, RunsTheCommandWithTheEventEnvironment) {
    ScratchFile out;
    HookContext context;
    context.server_id = "srv-1";
    context.server_name = "Living Room";
    context.server_url = "ws://hifi:8927/sendspin";
    context.client_id = "kitchen-left";
    context.client_name = "kitchen";

    HookRunner runner;
#ifdef _WIN32
    runner.run("<nul set /p =\"%SENDSPIN_EVENT%|%SENDSPIN_SERVER_ID%|%SENDSPIN_SERVER_NAME%|"
               "%SENDSPIN_SERVER_URL%|%SENDSPIN_CLIENT_ID%|%SENDSPIN_CLIENT_NAME%\"> " +
                   out.path(),
               "start", context);
#else
    runner.run("printf '%s|%s|%s|%s|%s|%s' \"$SENDSPIN_EVENT\" \"$SENDSPIN_SERVER_ID\" "
               "\"$SENDSPIN_SERVER_NAME\" \"$SENDSPIN_SERVER_URL\" \"$SENDSPIN_CLIENT_ID\" "
               "\"$SENDSPIN_CLIENT_NAME\" > " +
                   out.path(),
               "start", context);
#endif

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()),
              "start|srv-1|Living Room|ws://hifi:8927/sendspin|kitchen-left|kitchen");
}

#ifdef _WIN32
/// A command printing `name`'s value, or "unset" when the hook did not get it, with no newline.
std::string value_or_unset(const std::string& name) {
    return "(if defined " + name + " (<nul set /p =\"%" + name + "%\") else (<nul set /p =unset))";
}
#endif

TEST(HookRunner, AnUnknownFieldIsAbsentRatherThanEmpty) {
    // An unknown must be unset, not empty; ${VAR-unset} tells the two apart.
    ScratchFile out;
    HookContext context;
    context.client_name = "kitchen";

    HookRunner runner;
#ifdef _WIN32
    runner.run("(" + value_or_unset("SENDSPIN_SERVER_ID") + " & <nul set /p =\"|\" & " +
                   value_or_unset("SENDSPIN_SERVER_URL") + ")> " + out.path(),
               "stop", context);
#else
    runner.run(
        "printf '%s|%s' \"${SENDSPIN_SERVER_ID-unset}\" \"${SENDSPIN_SERVER_URL-unset}\" > " +
            out.path(),
        "stop", context);
#endif

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "unset|unset");
}

TEST(HookRunner, AnInheritedSendspinVariableDoesNotLeakThrough) {
    ScopedEnv stale("SENDSPIN_SERVER_ID", "stale-server");
    ScratchFile out;

    HookRunner runner;
#ifdef _WIN32
    runner.run(value_or_unset("SENDSPIN_SERVER_ID") + "> " + out.path(), "start", HookContext{});
#else
    runner.run("printf '%s' \"${SENDSPIN_SERVER_ID-unset}\" > " + out.path(), "start",
               HookContext{});
#endif

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "unset");
}

TEST(HookRunner, AFailingHookIsReapedRatherThanLeaked) {
    HookRunner runner;
    runner.run("exit 3", "stop", HookContext{});

    // A leaked child here would be a zombie per stream.
    EXPECT_TRUE(drain(runner));
    EXPECT_EQ(runner.running(), 0U);
}

// One at a time, newest event wins the wait

/// A command appending `text` to `path` with no newline, which cmd's echo cannot leave off.
std::string append_text(const std::string& path, const std::string& text) {
#ifdef _WIN32
    return "<nul set /p =\"" + text + "\">> " + path;
#else
    return "printf '" + text + "' >> " + path;
#endif
}

/// A command that succeeds and does nothing.
std::string succeed() {
#ifdef _WIN32
    return "exit /b 0";
#else
    return "true";
#endif
}

/// A hook that waits for `gate` to exist, then runs `then`: ordering without timing assumptions.
std::string gated(const std::string& gate, const std::string& then) {
#ifdef _WIN32
    // cmd has neither a short sleep nor a loop it can leave.
    return "powershell -NoProfile -NonInteractive -Command \"while (-not (Test-Path -LiteralPath "
           "'" +
           gate + "')) { Start-Sleep -Milliseconds 10 }\" & " + then;
#else
    return "while [ ! -e " + gate + " ]; do sleep 0.01; done; " + then;
#endif
}

void open_gate(const std::string& path) {
    const std::ofstream gate(path);
}

TEST(HookRunner, ASecondEventWaitsForTheRunningHook) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    // Run side by side, the stop hook's 'b' would land first; the order below is the contract.
    runner.run(gated(gate.path(), append_text(out.path(), "a")), "start", HookContext{});
    runner.run(append_text(out.path(), "b"), "stop", HookContext{});

    EXPECT_EQ(runner.running(), 1U);
    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "ab");
}

TEST(HookRunner, TheNewestEventReplacesTheWaitingOne) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    runner.run(gated(gate.path(), append_text(out.path(), "a")), "start", HookContext{});
    runner.run(append_text(out.path(), "b"), "stop", HookContext{});
    runner.run(append_text(out.path(), "c"), "start", HookContext{});

    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "ac");
}

TEST(HookRunner, AWaitingEventKeepsItsOwnContext) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    runner.run(gated(gate.path(), succeed()), "start", HookContext{});

    HookContext context;
    context.server_id = "srv-b";
#ifdef _WIN32
    runner.run("<nul set /p =\"%SENDSPIN_SERVER_ID%\"> " + out.path(), "stop", context);
#else
    runner.run("printf '%s' \"$SENDSPIN_SERVER_ID\" > " + out.path(), "stop", context);
#endif
    // The waiting event keeps the context it was fired with.
    context.server_id = "srv-c";

    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "srv-b");
}

TEST(HookRunner, FlushRunsTheWaitingHookBesideAHungOne) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    runner.run(gated(gate.path(), succeed()), "start", HookContext{});
    runner.run(append_text(out.path(), "b"), "stop", HookContext{});

    // flush() runs both at once: the stop hook outranks ordering at shutdown.
    runner.flush();
    EXPECT_EQ(runner.running(), 2U);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (slurp(out.path()) != "b" && std::chrono::steady_clock::now() < deadline) {
        runner.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(slurp(out.path()), "b");

    open_gate(gate.path());
    EXPECT_TRUE(drain(runner));
}

TEST(HookRunner, FlushWithNothingWaitingDoesNothing) {
    HookRunner runner;
    runner.flush();

    EXPECT_EQ(runner.running(), 0U);
}

#ifndef _WIN32
TEST(HookRunner, DoesNotHandTheHookThePlayersIgnoredSIGPIPE) {
    // An ignored SIGPIPE survives execve(); 141 (128 + SIGPIPE) shows the child reset it.
    ScopedSignal ignored(SIGPIPE, SIG_IGN);
    ScratchFile out;

    HookRunner runner;
    runner.run("{ dd if=/dev/zero bs=65536 count=64 2>/dev/null; echo \"$?\" > " + out.path() +
                   "; } | head -c 1 > /dev/null",
               "start", HookContext{});

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "141\n");
}

TEST(HookRunner, DoesNotHandTheHookThePlayersOpenDescriptors) {
    // Stands in for the player's own sockets.
    ScratchFile out;
    const int held = ::dup(STDERR_FILENO);
    ASSERT_GE(held, 0);
    if (held > 9) {
        ::close(held);
        GTEST_SKIP() << "no single-digit descriptor free; the shell below cannot name one above 9";
    }

    HookRunner runner;
    // stderr first, so the shell's complaint about a closed descriptor is discarded.
    runner.run(
        "echo held 2>/dev/null >&" + std::to_string(held) + " || echo closed > " + out.path(),
        "start", HookContext{});

    const bool drained = drain(runner);
    ::close(held);

    ASSERT_TRUE(drained);
    EXPECT_EQ(slurp(out.path()), "closed\n");
}
#else
TEST(HookRunner, DoesNotHandTheHookThePlayersOpenHandles) {
    // An inheritable, unshared handle stands in for the player's own sockets.
    ScratchFile held_path;
    ScratchFile gate;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE held = CreateFileA(held_path.path().c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);

    HookRunner runner;
    runner.run(gated(gate.path(), succeed()), "start", HookContext{});
    CloseHandle(held);

    // A copy inherited by the hook would still hold the file open and unshared.
    HANDLE reopened = CreateFileA(held_path.path().c_str(), GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool free = reopened != INVALID_HANDLE_VALUE;
    if (free) {
        CloseHandle(reopened);
    }

    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_TRUE(free);
}
#endif

}  // namespace
}  // namespace sendspin_cli
