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

/// @file last_server_test.cpp
/// @brief Remembering the last server, against an injected path rather than the real one

#include "last_server.h"

#include "scoped_env.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace sendspin_cli {
namespace {

/// A scratch directory of its own per test, removed again afterwards.
///
/// Under the test binary's own working directory rather than /tmp: a suite that scatters
/// files outside the build tree is a suite that leaves something behind when it fails.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ = "last-server-test-" + std::to_string(getpid()) + "-" +
                      std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
        // Restored in case a test made it unwritable, or the rmdir below cannot work.
        ::chmod(this->path_.c_str(), 0700);
        // Only ever the one file and the one directory, so no walk is needed.
        std::remove((this->path_ + "/state/last-server").c_str());
        ::rmdir((this->path_ + "/state").c_str());
        std::remove((this->path_ + "/last-server").c_str());
        ::rmdir(this->path_.c_str());
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    /// A path directly inside the scratch directory, whose parent already exists.
    std::string file() const {
        return this->path_ + "/last-server";
    }

    /// A path one directory deeper, whose parent save_last_server() has to create.
    std::string nested_file() const {
        return this->path_ + "/state/last-server";
    }

    const std::string& path() const {
        return this->path_;
    }

    /// Whether the directory was actually created, so a test fails on its own setup rather
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

// ---------------------------------------------------------------------------
// Round-tripping
// ---------------------------------------------------------------------------

TEST(LastServer, SavesAndLoadsTheSameId) {
    const ScratchDir scratch;

    ASSERT_TRUE(save_last_server(scratch.file(), "srv-abc123"));

    std::string loaded;
    ASSERT_TRUE(load_last_server(scratch.file(), loaded));
    EXPECT_EQ(loaded, "srv-abc123");
}

TEST(LastServer, OverwritesRatherThanAppending) {
    const ScratchDir scratch;

    ASSERT_TRUE(save_last_server(scratch.file(), "srv-first"));
    ASSERT_TRUE(save_last_server(scratch.file(), "srv-second"));

    std::string loaded;
    ASSERT_TRUE(load_last_server(scratch.file(), loaded));
    EXPECT_EQ(loaded, "srv-second");
}

TEST(LastServer, CreatesTheStateDirectory) {
    const ScratchDir scratch;

    ASSERT_TRUE(save_last_server(scratch.nested_file(), "srv-abc123"));

    std::string loaded;
    ASSERT_TRUE(load_last_server(scratch.nested_file(), loaded));
    EXPECT_EQ(loaded, "srv-abc123");
}

// ---------------------------------------------------------------------------
// Degrading rather than failing
// ---------------------------------------------------------------------------

TEST(LastServer, LoadingAMissingFileIsNotAnError) {
    const ScratchDir scratch;

    std::string loaded = "stale";
    EXPECT_FALSE(load_last_server(scratch.file(), loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST(LastServer, LoadingAnEmptyFileYieldsNothing) {
    const ScratchDir scratch;
    std::FILE* file = std::fopen(scratch.file().c_str(), "w");
    ASSERT_NE(file, nullptr);
    std::fclose(file);

    std::string loaded;
    EXPECT_FALSE(load_last_server(scratch.file(), loaded));
}

TEST(LastServer, AnEmptyPathIsAcceptedAsHavingNoMemory) {
    std::string loaded;
    EXPECT_FALSE(load_last_server("", loaded));
    EXPECT_FALSE(save_last_server("", "srv-abc123"));
}

TEST(LastServer, AnEmptyIdIsNotWritten) {
    const ScratchDir scratch;

    EXPECT_FALSE(save_last_server(scratch.file(), ""));

    std::string loaded;
    EXPECT_FALSE(load_last_server(scratch.file(), loaded));
}

TEST(LastServer, AMissingParentDirectoryFails) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    // Two levels deeper than anything that exists: only the leaf directory is ever created,
    // so this is the "the state directory is not there" case.
    EXPECT_FALSE(save_last_server(scratch.path() + "/a/b/last-server", "srv-abc123"));
}

TEST(LastServer, AnUnwritableDirectoryFails) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    // Read and execute but not write: the directory is there and can be walked, so this is
    // the permissions case rather than the missing-directory one above.
    ASSERT_EQ(::chmod(scratch.path().c_str(), 0500), 0);

    EXPECT_FALSE(save_last_server(scratch.file(), "srv-abc123"));
}

TEST(LastServer, AnOverLongIdIsRejectedRatherThanTruncated) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    std::FILE* file = std::fopen(scratch.file().c_str(), "w");
    ASSERT_NE(file, nullptr);
    // No newline, and longer than load_last_server() will read: a truncated prefix could
    // never match a browsed instance, so it must read as no memory at all.
    const std::string huge(4096, 'x');
    std::fwrite(huge.data(), 1, huge.size(), file);
    std::fclose(file);

    std::string loaded;
    EXPECT_FALSE(load_last_server(scratch.file(), loaded));
    EXPECT_TRUE(loaded.empty());
}

TEST(LastServer, TheTrailingNewlineIsNotPartOfTheId) {
    const ScratchDir scratch;
    std::FILE* file = std::fopen(scratch.file().c_str(), "w");
    ASSERT_NE(file, nullptr);
    std::fputs("srv-abc123\n", file);
    std::fclose(file);

    std::string loaded;
    ASSERT_TRUE(load_last_server(scratch.file(), loaded));
    EXPECT_EQ(loaded, "srv-abc123");
}

// ---------------------------------------------------------------------------
// Where the file goes
// ---------------------------------------------------------------------------

TEST(LastServerPath, PrefersXdgStateHome) {
    const ScopedEnv state("XDG_STATE_HOME", "/xdg/state");
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(last_server_path(), "/xdg/state/sendspin-cli/last-server");
}

TEST(LastServerPath, FallsBackToHomeLocalState) {
    const ScopedEnv state("XDG_STATE_HOME", nullptr);
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(last_server_path(), "/home/someone/.local/state/sendspin-cli/last-server");
}

TEST(LastServerPath, AnEmptyXdgStateHomeCountsAsUnset) {
    const ScopedEnv state("XDG_STATE_HOME", "");
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(last_server_path(), "/home/someone/.local/state/sendspin-cli/last-server");
}

TEST(LastServerPath, IsEmptyWhenThereIsNowhereToPutIt) {
    const ScopedEnv state("XDG_STATE_HOME", nullptr);
    const ScopedEnv home("HOME", nullptr);

    EXPECT_TRUE(last_server_path().empty());
}

}  // namespace
}  // namespace sendspin_cli
