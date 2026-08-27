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

#include <algorithm>
#include <chrono>
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

TEST(HookRunner, SeveralHooksInFlightAreEachReaped) {
    ScratchFile out;
    HookRunner runner;
    runner.run("printf 'a' >> " + out.path(), "start", HookContext{});
    runner.run("printf 'b' >> " + out.path(), "stop", HookContext{});
    runner.run("printf 'c' >> " + out.path(), "start", HookContext{});

    ASSERT_TRUE(drain(runner));
    // Order is the scheduler's, so only the multiset of writes is promised.
    std::string content = slurp(out.path());
    std::sort(content.begin(), content.end());
    EXPECT_EQ(content, "abc");
}

}  // namespace
}  // namespace sendspin_cli
