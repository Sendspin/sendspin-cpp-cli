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

/// @file daemon_test.cpp
/// @brief The pidfile lock and the logfile, exercised without forking
///
/// Nothing here forks, opens a device or a socket, or talks to the mDNS daemon, so the suite
/// stays runnable on a bare CI runner. That is affordable because of what flock() locks: a
/// lock belongs to the *open file description*, so two open() calls on one path conflict
/// inside a single process exactly as two instances would. fcntl() record locks would not --
/// they are per-process, and the second lock would be granted silently.
///
/// `-z` itself is deliberately not covered: fork() in a gtest process leaves two test runners
/// reporting results. It is exercised by hand, and docs/ROADMAP.md records what that covered.

#include "daemon.h"

#include "log.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <string>

namespace sendspin_cli {
namespace {

/// A scratch directory of its own per test, removed again afterwards.
///
/// Under the test binary's own working directory rather than /tmp, for the reason
/// last_server_test.cpp gives: a suite that scatters files outside the build tree is a suite
/// that leaves something behind when it fails.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ =
            "daemon-test-" + std::to_string(getpid()) + "-" + std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
        // Named individually rather than walked: every test here makes at most these four.
        std::remove(this->file("pid").c_str());
        std::remove(this->file("log").c_str());
        std::remove(this->file("log.1").c_str());
        std::remove(this->file("other").c_str());
        ::rmdir(this->path_.c_str());
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string file(const std::string& name) const {
        return this->path_ + "/" + name;
    }

    /// A path whose parent does not exist, for the "something else went wrong" branch.
    std::string missing_parent() const {
        return this->path_ + "/no-such-directory/pid";
    }

    /// Whether the directory was really created, so a test fails on its own setup rather
    /// than further down in whatever it was trying to prove.
    bool created() const {
        return this->created_;
    }

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
    bool created_{false};
};

/// The whole of `path`, or an empty string if it cannot be read.
std::string read_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return "";
    }
    std::string text;
    char buffer[256];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, read);
    }
    std::fclose(file);
    return text;
}

void write_file(const std::string& path, const std::string& content) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
}

std::string own_pid_line() {
    return std::to_string(static_cast<long>(getpid())) + "\n";
}

// ---------------------------------------------------------------------------
// The pidfile lock
// ---------------------------------------------------------------------------

TEST(PidFileTest, AcquireWritesOwnPid) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());

    std::string error;
    PidFile pidfile;
    EXPECT_EQ(pidfile.acquire(dir.file("pid"), error), PidFileStatus::Ok) << error;
    EXPECT_EQ(read_file(dir.file("pid")), own_pid_line());
}

TEST(PidFileTest, SecondAcquireIsRefusedAndLeavesTheFirstUntouched) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");

    std::string error;
    PidFile first;
    ASSERT_EQ(first.acquire(path, error), PidFileStatus::Ok) << error;
    const std::string held = read_file(path);
    ASSERT_FALSE(held.empty());

    // The whole point of opening without O_TRUNC and truncating only under the lock: the
    // loser must not have destroyed the winner's pid on its way to finding out it lost.
    PidFile second;
    std::string conflict;
    EXPECT_EQ(second.acquire(path, conflict), PidFileStatus::AlreadyRunning);
    EXPECT_NE(conflict.find("already running"), std::string::npos) << conflict;
    EXPECT_EQ(read_file(path), held);
}

TEST(PidFileTest, ReusesAFileLeftBehindByACrash) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");
    // A crashed process leaves the file but not the lock, since the kernel closed its
    // descriptor. Nothing parses this content, which is what keeps a recycled pid from ever
    // being read as a live instance.
    write_file(path, "not even a number\n");

    std::string error;
    PidFile pidfile;
    EXPECT_EQ(pidfile.acquire(path, error), PidFileStatus::Ok) << error;
    EXPECT_EQ(read_file(path), own_pid_line());
}

TEST(PidFileTest, LeavesNoTrailingBytesFromALongerPid) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");
    // Longer than any pid this process could have, so a missing ftruncate() would leave the
    // tail of it behind and a supervisor would read a pid that never existed.
    write_file(path, "4294967295\n");

    std::string error;
    PidFile pidfile;
    ASSERT_EQ(pidfile.acquire(path, error), PidFileStatus::Ok) << error;
    EXPECT_EQ(read_file(path), own_pid_line());
}

TEST(PidFileTest, RemovesTheFileOnDestruction) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");

    {
        std::string error;
        PidFile pidfile;
        ASSERT_EQ(pidfile.acquire(path, error), PidFileStatus::Ok) << error;
        ASSERT_FALSE(read_file(path).empty());
    }
    EXPECT_EQ(::access(path.c_str(), F_OK), -1);
}

TEST(PidFileTest, ReportsAnUnopenablePathAsFailedRatherThanAlreadyRunning) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());

    std::string error;
    PidFile pidfile;
    EXPECT_EQ(pidfile.acquire(dir.missing_parent(), error), PidFileStatus::Failed);
    EXPECT_NE(error.find("cannot open pidfile"), std::string::npos) << error;
}

TEST(PidFileTest, ProbeKeepsNoLockOfItsOwn) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");

    std::string error;
    ASSERT_EQ(probe_pidfile(path, error), PidFileStatus::Ok) << error;
    // If the probe held on to its lock, the child after the fork could never take it.
    ASSERT_EQ(probe_pidfile(path, error), PidFileStatus::Ok) << error;
    PidFile pidfile;
    EXPECT_EQ(pidfile.acquire(path, error), PidFileStatus::Ok) << error;
}

TEST(PidFileTest, ProbeSeesAHeldLock) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    const std::string path = dir.file("pid");

    std::string error;
    PidFile pidfile;
    ASSERT_EQ(pidfile.acquire(path, error), PidFileStatus::Ok) << error;
    EXPECT_EQ(probe_pidfile(path, error), PidFileStatus::AlreadyRunning);
}

TEST(PidFileTest, ProbeReportsAPathItCouldNotEvenCreate) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());

    std::string error;
    // The reason the probe creates rather than merely opens: -P under a directory that does
    // not exist has to fail at the terminal, not in a log written after the fork.
    EXPECT_EQ(probe_pidfile(dir.missing_parent(), error), PidFileStatus::Failed);
    EXPECT_NE(error.find("cannot open pidfile"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// The logfile
// ---------------------------------------------------------------------------

/// One test for the whole logfile story, in sequence, because it is a story about
/// process-wide state: whether stderr is on a file is a single global, so "a bare stderr line
/// carries no timestamp" is only true before log_to_file() has ever run. Asserting it in its
/// own test would make the suite depend on gtest's execution order.
TEST(LogFileTest, StampsOnlyAFileAndReopensThePathOnSighup) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::INFO);

    // fd 2 is restored at the end, so a failing assertion after this point still reports.
    const int saved_stderr = ::dup(STDERR_FILENO);
    ASSERT_GE(saved_stderr, 0);

    // 1. Bare stderr: the level letter and the tag, and no timestamp -- a foreground run
    //    under systemd or Docker is already stamped by journald or the container runtime.
    const int scratch = ::open(dir.file("other").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(scratch, 0);
    ASSERT_GE(::dup2(scratch, STDERR_FILENO), 0);
    ::close(scratch);
    log_line(sendspin::LogLevel::INFO, LOG_TAG_MDNS, "unstamped %d", 1);

    // 2. The gate asymmetry the whole of log_fatal() exists for: at -d none a fatal error is
    //    still the one thing that gets said, while an ordinary ERROR line is not. A refactor
    //    that shared too much between the two would flatten this silently.
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::NONE);
    log_fatal(LOG_TAG_CLI, "fatal at none %d", 9);
    log_line(sendspin::LogLevel::ERROR, LOG_TAG_CLI, "gated away %d", 9);
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::INFO);
    std::fflush(stderr);

    // 3. A path that cannot be opened must leave stderr intact -- that is the whole reason
    //    this goes through open()/dup2() rather than freopen(), which closes the stream even
    //    when it fails and would take the complaint about it down too.
    const bool unopenable = log_to_file(dir.file("no-such-directory/log"));

    // 4. A logfile: the same tail, with a UTC timestamp in front of it.
    const bool opened = log_to_file(dir.file("log"));
    log_line(sendspin::LogLevel::INFO, LOG_TAG_MDNS, "stamped %d", 2);

    // 5. Rotation, as logrotate and newsyslog do it: move the file, then SIGHUP. The handler
    //    only sets a flag -- the reopen flushes and then logs, neither of which is
    //    async-signal-safe -- so it is called here the way the main loop calls it, rather
    //    than through a real signal.
    const bool renamed = std::rename(dir.file("log").c_str(), dir.file("log.1").c_str()) == 0;
    log_handle_sighup(SIGHUP);
    log_reopen_if_requested();
    log_line(sendspin::LogLevel::INFO, LOG_TAG_MDNS, "after rotation %d", 3);
    std::fflush(stderr);

    ASSERT_GE(::dup2(saved_stderr, STDERR_FILENO), 0);
    ::close(saved_stderr);

    EXPECT_TRUE(opened);
    EXPECT_FALSE(unopenable);
    ASSERT_TRUE(renamed);

    // Bare stderr carries the letter and the tag and no timestamp -- and the complaint about
    // the path that would not open is here too, on stderr, rather than lost to stdout.
    const std::string bare = read_file(dir.file("other"));
    EXPECT_EQ(bare.rfind("I mdns: unstamped 1\n", 0), 0U) << bare;
    EXPECT_NE(bare.find("error: cannot open logfile"), std::string::npos) << bare;
    // -d none says nothing about why a start failed only if log_fatal is gated, which it
    // must not be; an ordinary ERROR line at the same level must still be dropped.
    EXPECT_NE(bare.find("E cli: fatal at none 9\n"), std::string::npos) << bare;
    EXPECT_EQ(bare.find("gated away"), std::string::npos) << bare;

    const std::string rotated = read_file(dir.file("log.1"));
    EXPECT_NE(rotated.find(" I mdns: stamped 2\n"), std::string::npos) << rotated;
    // 2026-08-10T03:14:15Z, so the letter is at a fixed offset and the Z is the field's end.
    ASSERT_GE(rotated.size(), 21U);
    EXPECT_EQ(rotated[10], 'T');
    EXPECT_EQ(rotated[19], 'Z');
    EXPECT_EQ(rotated[20], ' ');
    // The old descriptor must not still be being written to, or rotation would keep filling
    // a file nothing can find.
    EXPECT_EQ(rotated.find("after rotation"), std::string::npos) << rotated;

    const std::string fresh = read_file(dir.file("log"));
    EXPECT_NE(fresh.find("Reopened"), std::string::npos) << fresh;
    EXPECT_NE(fresh.find(" I mdns: after rotation 3\n"), std::string::npos) << fresh;
}

}  // namespace
}  // namespace sendspin_cli
