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
#include "scoped_env.h"

#include <gtest/gtest.h>
#include <unistd.h>

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
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
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
    runner.run("printf '%s|%s|%s|%s|%s|%s' \"$SENDSPIN_EVENT\" \"$SENDSPIN_SERVER_ID\" "
               "\"$SENDSPIN_SERVER_NAME\" \"$SENDSPIN_SERVER_URL\" \"$SENDSPIN_CLIENT_ID\" "
               "\"$SENDSPIN_CLIENT_NAME\" > " +
                   out.path(),
               "start", context);

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()),
              "start|srv-1|Living Room|ws://hifi:8927/sendspin|kitchen-left|kitchen");
}

TEST(HookRunner, AnUnknownFieldIsAbsentRatherThanEmpty) {
    // An unknown must be unset, not empty; ${VAR-unset} tells the two apart.
    ScratchFile out;
    HookContext context;
    context.client_name = "kitchen";

    HookRunner runner;
    runner.run("printf '%s|%s' \"${SENDSPIN_SERVER_ID-unset}\" \"${SENDSPIN_SERVER_URL-unset}\" > " +
                   out.path(),
               "stop", context);

    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "unset|unset");
}

TEST(HookRunner, AnInheritedSendspinVariableDoesNotLeakThrough) {
    ScopedEnv stale("SENDSPIN_SERVER_ID", "stale-server");
    ScratchFile out;

    HookRunner runner;
    runner.run("printf '%s' \"${SENDSPIN_SERVER_ID-unset}\" > " + out.path(), "start",
               HookContext{});

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

/// A hook that waits for `gate` to exist, then runs `then`: ordering without timing assumptions.
std::string gated(const std::string& gate, const std::string& then) {
    return "while [ ! -e " + gate + " ]; do sleep 0.01; done; " + then;
}

void open_gate(const std::string& path) {
    const std::ofstream gate(path);
}

TEST(HookRunner, ASecondEventWaitsForTheRunningHook) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    // Run side by side, the stop hook's 'b' would land first; the order below is the contract.
    runner.run(gated(gate.path(), "printf 'a' >> " + out.path()), "start", HookContext{});
    runner.run("printf 'b' >> " + out.path(), "stop", HookContext{});

    EXPECT_EQ(runner.running(), 1U);
    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "ab");
}

TEST(HookRunner, TheNewestEventReplacesTheWaitingOne) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    runner.run(gated(gate.path(), "printf 'a' >> " + out.path()), "start", HookContext{});
    runner.run("printf 'b' >> " + out.path(), "stop", HookContext{});
    runner.run("printf 'c' >> " + out.path(), "start", HookContext{});

    open_gate(gate.path());
    ASSERT_TRUE(drain(runner));
    EXPECT_EQ(slurp(out.path()), "ac");
}

TEST(HookRunner, AWaitingEventKeepsItsOwnContext) {
    ScratchFile out;
    ScratchFile gate;
    HookRunner runner;
    runner.run(gated(gate.path(), "true"), "start", HookContext{});

    HookContext context;
    context.server_id = "srv-b";
    runner.run("printf '%s' \"$SENDSPIN_SERVER_ID\" > " + out.path(), "stop", context);
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
    runner.run(gated(gate.path(), "true"), "start", HookContext{});
    runner.run("printf 'b' >> " + out.path(), "stop", HookContext{});

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
    runner.run("echo held 2>/dev/null >&" + std::to_string(held) + " || echo closed > " +
                   out.path(),
               "start", HookContext{});

    const bool drained = drain(runner);
    ::close(held);

    ASSERT_TRUE(drained);
    EXPECT_EQ(slurp(out.path()), "closed\n");
}

}  // namespace
}  // namespace sendspin_cli
