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

/// The volume-to-dB curve behind --alsa-mixer; built only with the ALSA backend.

#ifdef SENDSPIN_CLI_HAVE_ALSA

#include "alsa_sink.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace sendspin_cli {
namespace {

// A control whose range runs from -60 dB to 0 dB, in hundredths of a dB.
constexpr long MIN_DB = -6000;
constexpr long MAX_DB = 0;

TEST(MixerVolumeCurve, FullVolumeIsTheControlMax) {
    EXPECT_EQ(mixer_dB_for_volume(MIN_DB, MAX_DB, 100), MAX_DB);
}

TEST(MixerVolumeCurve, ZeroVolumeIsTheControlFloor) {
    EXPECT_EQ(mixer_dB_for_volume(MIN_DB, MAX_DB, 0), MIN_DB);
}

TEST(MixerVolumeCurve, FollowsTheSpecThirtyLogSlope) {
    // The spec's (v/100)^1.5 is 30*log10(v/100) dB; at 50 that is -9.03 dB, not -18.
    EXPECT_EQ(mixer_dB_for_volume(MIN_DB, MAX_DB, 50), -903);
}

TEST(MixerVolumeCurve, IsClampedToTheControlFloor) {
    // 30*log10(0.1) is -30 dB, below a floor of -20 dB, so the floor wins.
    EXPECT_EQ(mixer_dB_for_volume(-2000, MAX_DB, 10), -2000);
}

TEST(MixerVolumeCurve, RoundTripsThroughTheInverse) {
    // 1 is left out: its dB lands on the floor, which the inverse reads back as 0.
    for (uint8_t volume : {2, 25, 50, 75, 99, 100}) {
        const long dB = mixer_dB_for_volume(MIN_DB, MAX_DB, volume);
        EXPECT_EQ(volume_for_mixer_dB(MIN_DB, MAX_DB, dB), volume) << "volume " << int{volume};
    }
}

TEST(MixerVolumeCurve, InverseClampsAtBothEnds) {
    EXPECT_EQ(volume_for_mixer_dB(MIN_DB, MAX_DB, MIN_DB), 0);
    EXPECT_EQ(volume_for_mixer_dB(MIN_DB, MAX_DB, MIN_DB - 100), 0);
    EXPECT_EQ(volume_for_mixer_dB(MIN_DB, MAX_DB, MAX_DB), 100);
    EXPECT_EQ(volume_for_mixer_dB(MIN_DB, MAX_DB, MAX_DB + 100), 100);
}

}  // namespace
}  // namespace sendspin_cli

#endif  // SENDSPIN_CLI_HAVE_ALSA
