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

/// @file state_store_test.cpp
/// @brief The daemon's own memory, against an injected path rather than the real one
///
/// Covers the shared `key = value` reader too, since the store is its first consumer and the format
/// is where a bad line's line number comes from.

#include "state_store.h"

#include "key_value_file.h"
#include "scoped_env.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

/// A scratch directory of its own per test, removed again afterwards.
///
/// Under the test binary's own working directory rather than /tmp: a suite that scatters
/// files outside the build tree is a suite that leaves something behind when it fails.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ = "state-store-test-" + std::to_string(getpid()) + "-" +
                      std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
        // Restored in case a test made it unwritable, or the removals below cannot work.
        ::chmod(this->path_.c_str(), 0700);
        // A failed write can leave its temporary behind, and the pid is in that name.
        const std::string temporary_suffix = ".tmp." + std::to_string(getpid());
        for (const std::string& leaf : {std::string("state"), "state" + temporary_suffix}) {
            std::remove((this->path_ + "/nested/" + leaf).c_str());
            std::remove((this->path_ + "/" + leaf).c_str());
        }
        ::rmdir((this->path_ + "/nested").c_str());
        ::rmdir(this->path_.c_str());
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    /// A path directly inside the scratch directory, whose parent already exists.
    std::string file() const {
        return this->path_ + "/state";
    }

    /// A path one directory deeper, whose parent the store has to create.
    std::string nested_file() const {
        return this->path_ + "/nested/state";
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

/// Writes `text` to `path` verbatim, for the cases that need a file the store did not produce.
void write_file(const std::string& path, const std::string& text) {
    std::FILE* file = std::fopen(path.c_str(), "w");
    ASSERT_NE(file, nullptr);
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

/// A store already loaded from `path`, which is how every caller uses one.
StateStore loaded(const std::string& path) {
    StateStore store(path);
    size_t malformed_line = 0;
    store.load(malformed_line);
    return store;
}

// ---------------------------------------------------------------------------
// Round-tripping every key
// ---------------------------------------------------------------------------

TEST(StateStore, RoundTripsEveryKeyAcrossAReload) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_last_server("srv-abc123"));
    ASSERT_TRUE(store.set_last_server_hash(0xDEADBEEFU));
    ASSERT_TRUE(store.set_static_delay_ms(275));
    ASSERT_TRUE(store.set_volume_and_muted(42, true));

    // A second store over the same path is the restart this whole file exists for.
    const StateStore reloaded = loaded(scratch.file());
    EXPECT_EQ(reloaded.last_server(), "srv-abc123");
    EXPECT_EQ(reloaded.last_server_hash(), 0xDEADBEEFU);
    EXPECT_EQ(reloaded.static_delay_ms(), 275);
    EXPECT_EQ(reloaded.volume(), 42);
    EXPECT_EQ(reloaded.muted(), true);
}

TEST(StateStore, RemembersMutedFalseRatherThanForgettingIt) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(100, false));

    // `false` is a value a server chose, not an absence: a store that dropped it would have a
    // player un-mute itself on restart only when the answer was the interesting one.
    EXPECT_EQ(loaded(scratch.file()).muted(), false);
}

TEST(StateStore, AVolumeOfZeroIsRememberedRatherThanReadAsAbsent) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(0, false));

    // The same trap as muted=false, and worse: 0 is exactly the value whose loss is audible.
    EXPECT_EQ(loaded(scratch.file()).volume(), 0);
}

TEST(StateStore, OverwritesAKeyRatherThanAppendingIt) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_last_server("srv-first"));
    ASSERT_TRUE(store.set_last_server("srv-second"));

    EXPECT_EQ(loaded(scratch.file()).last_server(), "srv-second");
    // And the file really holds one of it, rather than reading right only because last wins.
    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    size_t occurrences = 0;
    for (const KeyValueEntry& entry : entries) {
        occurrences += entry.key == "last-server" ? 1 : 0;
    }
    EXPECT_EQ(occurrences, 1U);
}

TEST(StateStore, SettingOneKeyKeepsTheOthers) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(30, false));
    ASSERT_TRUE(store.set_static_delay_ms(120));

    // The whole file is rewritten on every set, so this is the case where a rewrite that only
    // knew about the key being changed would quietly drop everything else.
    const StateStore reloaded = loaded(scratch.file());
    EXPECT_EQ(reloaded.volume(), 30);
    EXPECT_EQ(reloaded.static_delay_ms(), 120);
}

TEST(StateStore, CreatesTheLeafDirectory) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.nested_file());
    ASSERT_TRUE(store.set_last_server("srv-abc123"));

    EXPECT_EQ(loaded(scratch.nested_file()).last_server(), "srv-abc123");
}

TEST(StateStore, WritesTheFileAt0600) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_last_server("srv-abc123"));

    // The file names the server this player talks to, so it is the owner's business alone.
    struct stat info = {};
    ASSERT_EQ(::stat(scratch.file().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
}

TEST(StateStore, LeavesNoTemporaryBehind) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(55, false));

    // The write goes through a temporary and rename(), which is what makes a kill mid-write
    // leave either the old file or the new one. A temporary left in place would be a second
    // file a later reader could find.
    const std::string temporary = scratch.file() + ".tmp." + std::to_string(getpid());
    struct stat info = {};
    EXPECT_NE(::stat(temporary.c_str(), &info), 0);
}

// ---------------------------------------------------------------------------
// Degrading rather than failing
// ---------------------------------------------------------------------------

TEST(StateStore, AMissingFileIsNotAnError) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    size_t malformed_line = 0;
    EXPECT_EQ(store.load(malformed_line), StateLoadResult::Absent);
    EXPECT_TRUE(store.last_server().empty());
    EXPECT_FALSE(store.volume().has_value());
    EXPECT_FALSE(store.muted().has_value());
    EXPECT_FALSE(store.static_delay_ms().has_value());
    EXPECT_FALSE(store.last_server_hash().has_value());
}

TEST(StateStore, AnEmptyPathIsAcceptedAsHavingNoMemory) {
    StateStore store("");
    size_t malformed_line = 0;
    EXPECT_EQ(store.load(malformed_line), StateLoadResult::Absent);
    EXPECT_TRUE(store.last_server().empty());
    // Reported so main() can say so once, and non-fatal: a player that cannot remember its
    // volume is still a player.
    EXPECT_FALSE(store.set_volume_and_muted(50, false));
}

TEST(StateStore, AnEmptyServerIdIsNotWritten) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    StateStore store(scratch.file());
    EXPECT_FALSE(store.set_last_server(""));
    EXPECT_TRUE(loaded(scratch.file()).last_server().empty());
}

TEST(StateStore, AMissingParentDirectoryFails) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    // Two levels deeper than anything that exists: only the leaf directory is ever created,
    // so this is the "the state directory is not there" case.
    StateStore store(scratch.path() + "/a/b/state");
    EXPECT_FALSE(store.set_last_server("srv-abc123"));
}

TEST(StateStore, AnUnwritableDirectoryFails) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    // Read and execute but not write: the directory is there and can be walked, so this is
    // the permissions case rather than the missing-directory one above.
    ASSERT_EQ(::chmod(scratch.path().c_str(), 0500), 0);

    StateStore store(scratch.file());
    EXPECT_FALSE(store.set_last_server("srv-abc123"));
}

TEST(StateStore, AFailedWriteIsNotRememberedAsASuccess) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(30, false));
    ASSERT_EQ(::chmod(scratch.path().c_str(), 0500), 0);

    EXPECT_FALSE(store.set_volume_and_muted(40, false));
    // The value held has to keep describing what is on disk. Otherwise the identical-value
    // short-circuit would answer the retry below from memory and claim a write that never happened
    // -- and a full disk that later cleared would never be written to again.
    EXPECT_EQ(store.volume(), 30);
    EXPECT_FALSE(store.set_volume_and_muted(40, false));

    ASSERT_EQ(::chmod(scratch.path().c_str(), 0700), 0);
    EXPECT_TRUE(store.set_volume_and_muted(40, false));
    EXPECT_EQ(loaded(scratch.file()).volume(), 40);
}

TEST(StateStore, AMalformedLineIsSkippedRatherThanRefused) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    // Nothing but this daemon writes here, so a line that does not parse means the file was
    // corrupted rather than mistyped -- and refusing to start over something we wrote ourselves
    // would strand a player. Unlike the config file, which refuses.
    write_file(scratch.file(), "volume = 40\nthis is not a pair\nmuted = true\n");

    StateStore store(scratch.file());
    size_t malformed_line = 0;
    // Reported as Corrupt rather than as Absent, and with the line, so main() can WARN about it.
    // Discarding a corrupted state file silently would have the volume and delay it held come back
    // as defaults with no explanation, and the next change overwrites the evidence.
    EXPECT_EQ(store.load(malformed_line), StateLoadResult::Corrupt);
    EXPECT_EQ(malformed_line, 2U);
    // The bad line takes the whole file's entries with it, by design: a partly-applied corrupt file
    // is worse to reason about than an empty one. Not fatal either way -- this is a file we wrote.
    EXPECT_FALSE(store.volume().has_value());
}

TEST(StateStore, WritesVolumeAndMuteInOneGo) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.file());
    ASSERT_TRUE(store.set_volume_and_muted(20, false));

    // The pair is one decision, so it has to reach the disk in one write: a server can send volume
    // and mute in the same command, and two writes would let a kill land between them and persist a
    // pair that was never true. Checked by making the write impossible and asserting that *neither*
    // half moved, which is what one write buys and two do not.
    ASSERT_EQ(::chmod(scratch.path().c_str(), 0500), 0);
    EXPECT_FALSE(store.set_volume_and_muted(90, true));
    ASSERT_EQ(::chmod(scratch.path().c_str(), 0700), 0);

    const StateStore reloaded = loaded(scratch.file());
    EXPECT_EQ(reloaded.volume(), 20);
    EXPECT_EQ(reloaded.muted(), false);
    // And in memory too, so the short-circuit cannot later claim the failed write succeeded.
    EXPECT_EQ(store.volume(), 20);
    EXPECT_EQ(store.muted(), false);
}

TEST(StateStore, AnOutOfRangeNumberReadsAsAbsentRatherThanBeingClamped) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    // A volume is a percentage and a delay is a uint16_t. An impossible figure means something
    // else wrote here, and honouring part of it is worse than starting from the default.
    write_file(scratch.file(),
               "volume = 900\nstatic-delay-ms = 70000\nlast-server-hash = 99999999999\n");

    const StateStore store = loaded(scratch.file());
    EXPECT_FALSE(store.volume().has_value());
    EXPECT_FALSE(store.static_delay_ms().has_value());
    EXPECT_FALSE(store.last_server_hash().has_value());
}

TEST(StateStore, ANonNumericNumberReadsAsAbsent) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "volume = loud\nmuted = perhaps\n");

    const StateStore store = loaded(scratch.file());
    EXPECT_FALSE(store.volume().has_value());
    EXPECT_FALSE(store.muted().has_value());
}

TEST(StateStore, LastWinsWithinOneFile) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "volume = 10\nvolume = 70\n");

    EXPECT_EQ(loaded(scratch.file()).volume(), 70);
}

TEST(StateStore, ReadsBackAFileItWroteWithCommentsAndBlankLines) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "# Written by sendspin-cli. Edits are overwritten.\n\n"
                               "  volume  =  65  \n# a comment\nlast-server = srv-x\n");

    const StateStore store = loaded(scratch.file());
    EXPECT_EQ(store.volume(), 65);
    EXPECT_EQ(store.last_server(), "srv-x");
}

// ---------------------------------------------------------------------------
// Where the file goes
// ---------------------------------------------------------------------------

TEST(StateStorePath, PrefersTheExplicitStateDir) {
    const ScopedEnv state("XDG_STATE_HOME", "/xdg/state");
    const ScopedEnv home("HOME", "/home/someone");

    // --state-dir overrides rather than being another fallback: it is what a systemd system
    // unit's StateDirectory= is pointed at, and that has to beat an inherited variable.
    EXPECT_EQ(state_store_path("/var/lib/sendspin-cli"), "/var/lib/sendspin-cli/state");
}

TEST(StateStorePath, PrefersXdgStateHome) {
    const ScopedEnv state("XDG_STATE_HOME", "/xdg/state");
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(state_store_path(""), "/xdg/state/sendspin-cli/state");
}

TEST(StateStorePath, FallsBackToHomeLocalState) {
    const ScopedEnv state("XDG_STATE_HOME", nullptr);
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(state_store_path(""), "/home/someone/.local/state/sendspin-cli/state");
}

TEST(StateStorePath, AnEmptyXdgStateHomeCountsAsUnset) {
    const ScopedEnv state("XDG_STATE_HOME", "");
    const ScopedEnv home("HOME", "/home/someone");

    EXPECT_EQ(state_store_path(""), "/home/someone/.local/state/sendspin-cli/state");
}

TEST(StateStorePath, IsEmptyWhenThereIsNowhereToPutIt) {
    const ScopedEnv state("XDG_STATE_HOME", nullptr);
    const ScopedEnv home("HOME", nullptr);

    EXPECT_TRUE(state_store_path("").empty());
}

// ---------------------------------------------------------------------------
// The shared flat file format
// ---------------------------------------------------------------------------

TEST(KeyValueFile, SplitsOnTheFirstEqualsAndTrimsAroundIt) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "  server =  ws://host:8927/sendspin?a=b  \n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "server");
    // Everything after the first '=' is the value, so a URL with its own '=' survives.
    EXPECT_EQ(entries[0].value, "ws://host:8927/sendspin?a=b");
    EXPECT_EQ(entries[0].line, 1U);
}

TEST(KeyValueFile, TreatsHashAsAComentOnlyAtTheStartOfALine) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "# a comment\n   # an indented comment\nname = studio#2\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    ASSERT_EQ(entries.size(), 1U);
    // No trailing comments, which is what keeps a value byte-for-byte what the command line
    // would have passed -- a name or a path is free to contain a '#'.
    EXPECT_EQ(entries[0].value, "studio#2");
    EXPECT_EQ(entries[0].line, 3U);
}

TEST(KeyValueFile, ReportsTheLineOfAPairlessLine) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "port = 9000\n\nnonsense\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    EXPECT_EQ(read_key_value_file(scratch.file(), entries, malformed_line),
              KeyValueStatus::Malformed);
    EXPECT_EQ(malformed_line, 3U);
    // Nothing partial is handed back, so a caller that refuses the file cannot also act on the
    // half of it that parsed.
    EXPECT_TRUE(entries.empty());
}

TEST(KeyValueFile, RefusesAnEmptyKey) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), " = 9000\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    EXPECT_EQ(read_key_value_file(scratch.file(), entries, malformed_line),
              KeyValueStatus::Malformed);
    EXPECT_EQ(malformed_line, 1U);
}

TEST(KeyValueFile, AcceptsAnEmptyValueAndLeavesTheJudgementToTheOption) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "name =\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    ASSERT_EQ(entries.size(), 1U);
    // The reader has no opinion: whether an empty value means anything is the flag's own rule,
    // and duplicating it here would put it in two places.
    EXPECT_TRUE(entries[0].value.empty());
}

TEST(KeyValueFile, StripsACarriageReturnFromAFileWrittenOnWindows) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "port = 9000\r\nname = studio\r\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    ASSERT_EQ(entries.size(), 2U);
    // A stray '\r' on the end otherwise fails much further away, as an unopenable path or an
    // unknown device.
    EXPECT_EQ(entries[0].value, "9000");
    EXPECT_EQ(entries[1].value, "studio");
}

TEST(KeyValueFile, ReadsALastLineWithNoTrailingNewline) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "port = 9000");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    ASSERT_EQ(read_key_value_file(scratch.file(), entries, malformed_line), KeyValueStatus::Ok);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].value, "9000");
}

TEST(KeyValueFile, RefusesAnOverLongLine) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    write_file(scratch.file(), "name = " + std::string(MAX_KEY_VALUE_LINE_BYTES, 'x') + "\n");

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    EXPECT_EQ(read_key_value_file(scratch.file(), entries, malformed_line),
              KeyValueStatus::Malformed);
    EXPECT_EQ(malformed_line, 1U);
}

TEST(KeyValueFile, ReportsAMissingFileAsUnreadableRatherThanMalformed) {
    const ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    std::vector<KeyValueEntry> entries;
    size_t malformed_line = 0;
    // The two are different answers: one file is absent, which is normal, and the other is
    // present and wrong, which a config file has to refuse.
    EXPECT_EQ(read_key_value_file(scratch.file(), entries, malformed_line),
              KeyValueStatus::Unreadable);
    EXPECT_EQ(read_key_value_file("", entries, malformed_line), KeyValueStatus::Unreadable);
}

}  // namespace
}  // namespace sendspin_cli
