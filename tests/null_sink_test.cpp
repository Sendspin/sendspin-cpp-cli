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

/// NullAudioSink's playback reports, which the player releases each chunk against.

#include "null_sink.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sendspin_cli {
namespace {

struct Report {
    uint32_t frames;
    int64_t finish_us;
};

/// A discarding sink with every report recorded.
struct Harness {
    Harness() {
        this->sink.on_frames_played = [this](uint32_t frames, int64_t finish_us) {
            this->reports.push_back({frames, finish_us});
        };
    }

    NullAudioSink sink{NullSinkOutput::Discard};
    std::vector<Report> reports;
};

/// 480 frames of 16-bit stereo: 10 ms at 48 kHz.
const std::vector<uint8_t> TEN_MS(480 * 4, 0);

TEST(NullSink, EveryWriteIsReportedPlayedOnAClockThatKeepsAdvancing) {
    Harness harness;
    ASSERT_TRUE(harness.sink.configure(48000, 2, 16));

    EXPECT_EQ(harness.sink.write(TEN_MS.data(), TEN_MS.size(), 20), TEN_MS.size());
    EXPECT_EQ(harness.sink.write(TEN_MS.data(), TEN_MS.size(), 20), TEN_MS.size());

    ASSERT_EQ(harness.reports.size(), 2U);
    EXPECT_EQ(harness.reports[0].frames, 480U);
    EXPECT_EQ(harness.reports[1].frames, 480U);
    // The second write finishes a full write after the first, however fast the two ran.
    EXPECT_GE(harness.reports[1].finish_us - harness.reports[0].finish_us, 10000);
}

TEST(NullSink, NothingIsReportedWithoutAStreamOrFrames) {
    Harness harness;

    EXPECT_EQ(harness.sink.write(TEN_MS.data(), TEN_MS.size(), 20), TEN_MS.size());
    ASSERT_TRUE(harness.sink.configure(48000, 2, 16));
    EXPECT_EQ(harness.sink.write(TEN_MS.data(), 0, 20), 0U);
    harness.sink.stop();
    EXPECT_EQ(harness.sink.write(TEN_MS.data(), TEN_MS.size(), 20), TEN_MS.size());

    EXPECT_TRUE(harness.reports.empty());
}

}  // namespace
}  // namespace sendspin_cli
