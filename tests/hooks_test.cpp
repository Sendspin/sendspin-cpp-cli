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

/// @file hooks_test.cpp
/// @brief --hook-start/--hook-stop: the flag surface, and what a spawned hook really sees

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

// ---------------------------------------------------------------------------
// The flag surface
// ---------------------------------------------------------------------------

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
    // Its own tiny scratch file rather than config_file_test's ScratchDir, which is private
    // to that suite. Removed before the assertions can throw, so a failure leaves nothing.
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

// ---------------------------------------------------------------------------
// What a spawned hook really sees
// ---------------------------------------------------------------------------

/// Polls `runner` until every spawned hook has been reaped, or fails after `timeout_ms`.
///
/// The runner is polled from the main loop in the real daemon; here the test *is* the main
/// loop, so it has to keep calling poll() the same way.
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
    // `[ -n "$SENDSPIN_SERVER_ID" ]` is the test the docs promise a hook can write, so an
    // unknown must be an *unset* variable -- ${VAR-unset} tells the two apart where a plain
    // expansion cannot.
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
    // A wrapper script that exported SENDSPIN_SERVER_ID would otherwise describe some other
    // run to every hook this one spawns.
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

    // The warning it logs goes to stderr; what the test can hold it to is that the child is
    // waited on and the bookkeeping empties -- a leak here is a zombie per stream forever.
    EXPECT_TRUE(drain(runner));
    EXPECT_EQ(runner.running(), 0U);
}

// ---------------------------------------------------------------------------
// One at a time, newest event wins the wait
// ---------------------------------------------------------------------------

/// A hook command that spins until `gate` exists, then runs `then`.
///
/// What every ordering test below hangs off: the gated hook is deterministically still
/// running until the test opens the gate, with no scheduler timing assumed anywhere.
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
    // The start hook cannot write until the gate exists, so spawned side by side the stop
    // hook's 'b' would deterministically land first. The order below is the contract.
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
    // The stop was superseded while it waited: the hardware ends in the final state, not
    // replaying the intermediate on the way there.
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
    // What the caller does to its object between events must not reach into the slot: the
    // waiting event describes the stream it was fired for.
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

    // What the shutdown path does when the drain ends: two children out at once,
    // deliberately -- the promise that stopping the player runs the stop hook outranks
    // ordering when no more events can come.
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
    // The player ignores SIGPIPE, and an ignored disposition survives execve() where a caught
    // one does not -- so without the reset in the child, every hook and everything it spawns
    // would run with it ignored. 141 is the writer ended by the pipe closing (128 + SIGPIPE),
    // which is what a shell command anywhere else on the box does; 1 is it giving up on a
    // write error instead, which is what inheriting the ignore looks like.
    //
    // dd rather than `yes` because it stops on its own: a regression here should fail this
    // assertion, not leave something writing until the timeout.
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
    // Standing in for the player's real ones: the audio port's listening socket, the control
    // socket's accepted peers, the connection to the server. A hook that inherited them would
    // hold the port a restart needs for as long as it ran.
    ScratchFile out;
    const int held = ::dup(STDERR_FILENO);
    ASSERT_GE(held, 0);
    if (held > 9) {
        ::close(held);
        GTEST_SKIP() << "no single-digit descriptor free; the shell below cannot name one above 9";
    }

    HookRunner runner;
    // stderr redirected before the descriptor is named, so the shell's complaint about a
    // descriptor that is not there lands in /dev/null rather than in the test's output.
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
