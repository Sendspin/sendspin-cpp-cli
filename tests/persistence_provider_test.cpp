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

#include "persistence_provider.h"

#include "posix_compat.h"
#include "state_store.h"

#include <sendspin/persistence_keys.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

class ScratchStateFile {
public:
    ScratchStateFile() {
        this->dir_ = "persistence-provider-test-" + std::to_string(getpid());
        this->created_ = ::mkdir(this->dir_.c_str(), 0700) == 0;
    }
    ~ScratchStateFile() {
        std::remove(this->path().c_str());
        ::rmdir(this->dir_.c_str());
    }
    ScratchStateFile(const ScratchStateFile&) = delete;
    ScratchStateFile& operator=(const ScratchStateFile&) = delete;

    bool created() const {
        return this->created_;
    }
    std::string path() const {
        return this->dir_ + "/state";
    }
    void write(const std::string& text) const {
        std::ofstream(this->path(), std::ios::binary) << text;
    }
    std::string contents() const {
        std::ifstream in(this->path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    std::string dir_;
    bool created_{false};
};

StateStore loaded(const std::string& path) {
    StateStore store(path);
    size_t malformed = 0;
    EXPECT_EQ(store.load(malformed), StateLoadResult::Loaded);
    return store;
}

TEST(CliPersistenceProvider, AMissingKeyReadsAsAbsent) {
    ScratchStateFile scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.path());
    CliPersistenceProvider provider(store);

    std::array<uint8_t, 8> out{};
    EXPECT_FALSE(provider.read_blob("keypair", out).has_value());
}

TEST(CliPersistenceProvider, ABlobLongerThanTheBufferReadsAsAbsentAndLeavesItAlone) {
    ScratchStateFile scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.path());
    CliPersistenceProvider provider(store);
    const std::array<uint8_t, 4> data{1, 2, 3, 4};
    ASSERT_TRUE(provider.write_blob("rec_order", data));

    std::array<uint8_t, 3> out{9, 9, 9};
    EXPECT_FALSE(provider.read_blob("rec_order", out).has_value());
    EXPECT_EQ(out, (std::array<uint8_t, 3>{9, 9, 9}));
}

TEST(CliPersistenceProvider, AShorterBlobReadsIntoTheFrontOfTheBuffer) {
    ScratchStateFile scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.path());
    CliPersistenceProvider provider(store);
    const std::array<uint8_t, 2> data{0xab, 0xcd};
    ASSERT_TRUE(provider.write_blob("rec_order", data));

    std::array<uint8_t, 4> out{};
    EXPECT_EQ(provider.read_blob("rec_order", out), 2U);
    EXPECT_EQ(out[0], 0xab);
    EXPECT_EQ(out[1], 0xcd);
}

TEST(CliPersistenceProvider, ReadsBlobsAnEarlierBuildWrote) {
    ScratchStateFile scratch;
    ASSERT_TRUE(scratch.created());
    scratch.write("blob-keypair = 00112233445566778899aabbccddeeff"
                  "00112233445566778899aabbccddeeff\n");
    StateStore store = loaded(scratch.path());
    CliPersistenceProvider provider(store);

    std::array<uint8_t, sendspin::persistence_keys::KEYPAIR_SIZE> out{};
    ASSERT_EQ(provider.read_blob(sendspin::persistence_keys::KEYPAIR, out),
              sendspin::persistence_keys::KEYPAIR_SIZE);
    EXPECT_EQ(out[0], 0x00);
    EXPECT_EQ(out[1], 0x11);
    EXPECT_EQ(out[31], 0xff);
}

TEST(CliPersistenceProvider, WritesBlobsInTheStateFileFormat) {
    ScratchStateFile scratch;
    ASSERT_TRUE(scratch.created());
    StateStore store(scratch.path());
    CliPersistenceProvider provider(store);
    const std::string key(sendspin::persistence_keys::RecordSlotKey(2).view());
    const std::vector<uint8_t> data{0x01, 0xfe};

    ASSERT_TRUE(provider.write_blob(key, data));

    EXPECT_NE(scratch.contents().find("blob-rec_2 = 01fe"), std::string::npos);
    EXPECT_EQ(loaded(scratch.path()).blob(key), data);
}

}  // namespace
}  // namespace sendspin_cli
