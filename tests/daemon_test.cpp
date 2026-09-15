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

/// The pidfile lock and the logfile, exercised without forking.
/// flock() locks per open file description, so one process can stand in for two instances.

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

/// A scratch directory under the build tree, per test, removed afterwards.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ =
            "daemon-test-" + std::to_string(getpid()) + "-" + std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
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

    /// Whether the directory was created, so setup failures fail at setup.
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

// The pidfile lock

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

    // The loser must not truncate the winner's pid.
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
    // A crashed process leaves the file but not the lock; the content is never parsed.
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
    // Longer than any pid, so a missing ftruncate() would leave stale digits.
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
    // -P under a missing directory must fail at the terminal, before the fork.
    EXPECT_EQ(probe_pidfile(dir.missing_parent(), error), PidFileStatus::Failed);
    EXPECT_NE(error.find("cannot open pidfile"), std::string::npos) << error;
}

// The logfile

/// One sequential test, since whether stderr is on a file is process-wide state.
TEST(LogFileTest, StampsOnlyAFileAndReopensThePathOnSighup) {
    ScratchDir dir;
    ASSERT_TRUE(dir.created());
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::INFO);

    // fd 2 is restored at the end, so a failing assertion after this point still reports.
    const int saved_stderr = ::dup(STDERR_FILENO);
    ASSERT_GE(saved_stderr, 0);

    // 1. Bare stderr: level letter and tag, no timestamp.
    const int scratch = ::open(dir.file("other").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(scratch, 0);
    ASSERT_GE(::dup2(scratch, STDERR_FILENO), 0);
    ::close(scratch);
    log_line(sendspin::LogLevel::INFO, LOG_TAG_MDNS, "unstamped %d", 1);

    // 2. At -d none, log_fatal still prints and an ordinary ERROR does not.
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::NONE);
    log_fatal(LOG_TAG_CLI, "fatal at none %d", 9);
    log_line(sendspin::LogLevel::ERROR, LOG_TAG_CLI, "gated away %d", 9);
    sendspin::SendspinClient::set_log_level(sendspin::LogLevel::INFO);
    std::fflush(stderr);

    // 3. An unopenable path leaves stderr intact.
    const bool unopenable = log_to_file(dir.file("no-such-directory/log"));

    // 4. A logfile: the same tail, with a UTC timestamp in front of it.
    const bool opened = log_to_file(dir.file("log"));
    log_line(sendspin::LogLevel::INFO, LOG_TAG_MDNS, "stamped %d", 2);

    // 5. Rotation: rename, then the reopen the SIGHUP handler requests.
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

    // Bare stderr lines, including the complaint about the unopenable path.
    const std::string bare = read_file(dir.file("other"));
    EXPECT_EQ(bare.rfind("I mdns: unstamped 1\n", 0), 0U) << bare;
    EXPECT_NE(bare.find("error: cannot open logfile"), std::string::npos) << bare;
    EXPECT_NE(bare.find("E cli: fatal at none 9\n"), std::string::npos) << bare;
    EXPECT_EQ(bare.find("gated away"), std::string::npos) << bare;

    const std::string rotated = read_file(dir.file("log.1"));
    EXPECT_NE(rotated.find(" I mdns: stamped 2\n"), std::string::npos) << rotated;
    // 2026-08-10T03:14:15Z, so the letter is at a fixed offset and the Z is the field's end.
    ASSERT_GE(rotated.size(), 21U);
    EXPECT_EQ(rotated[10], 'T');
    EXPECT_EQ(rotated[19], 'Z');
    EXPECT_EQ(rotated[20], ' ');
    // The old descriptor must no longer be written.
    EXPECT_EQ(rotated.find("after rotation"), std::string::npos) << rotated;

    const std::string fresh = read_file(dir.file("log"));
    EXPECT_NE(fresh.find("Reopened"), std::string::npos) << fresh;
    EXPECT_NE(fresh.find(" I mdns: after rotation 3\n"), std::string::npos) << fresh;
}

}  // namespace
}  // namespace sendspin_cli
