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
        ::mkdir(this->path_.c_str(), 0700);
    }

    ~ScratchDir() {
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

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
};

/// Sets an environment variable for the duration of a test, restoring it afterwards.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* previous = std::getenv(name);
        this->had_previous_ = previous != nullptr;
        if (this->had_previous_) {
            this->previous_ = previous;
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }

    ~ScopedEnv() {
        if (this->had_previous_) {
            ::setenv(this->name_, this->previous_.c_str(), 1);
        } else {
            ::unsetenv(this->name_);
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string previous_;
    bool had_previous_{false};
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

TEST(LastServer, AnUnwritableDirectoryFailsWithoutThrowing) {
    const ScratchDir scratch;

    // Two levels deeper than anything that exists: only the leaf directory is ever created,
    // so this is the "the state directory is not there" case.
    EXPECT_FALSE(save_last_server(scratch.path() + "/a/b/last-server", "srv-abc123"));
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
