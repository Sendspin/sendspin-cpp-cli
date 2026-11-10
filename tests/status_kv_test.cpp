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

#include "status_kv.h"

#include <gtest/gtest.h>

#include <string>

namespace sendspin_cli {
namespace {

/// A connected, playing snapshot with every field filled, so a round trip can check each one.
StatusSnapshot full_snapshot() {
    StatusSnapshot snapshot;
    snapshot.name = "living-room";
    snapshot.client_id = "abc123";
    snapshot.connected = true;
    snapshot.server_name = "Music Assistant";
    snapshot.server_id = "OraobU4l";
    snapshot.trust = sendspin::ConnectionTrust::USER;
    snapshot.paired_servers = 2;
    snapshot.pairing_server_id = "other-server";
    snapshot.pairing_code = "123456";
    snapshot.pairing_window_open = true;
    snapshot.last_pairing = {3, "OraobU4l", PairingOutcome::Failed, "wrong code"};
    snapshot.playback_speed = 1000;
    snapshot.streaming = true;
    snapshot.format = StreamFormat{48000, 2, 16};
    snapshot.codec = sendspin::SendspinCodecFormat::FLAC;
    snapshot.artist = "Nils Frahm";
    snapshot.title = "Says";
    snapshot.album = "Spaces";
    snapshot.progress_ms = 125000;
    snapshot.duration_ms = 543000;
    snapshot.group_state_known = true;
    snapshot.group_volume = 55;
    snapshot.group_muted = true;
    snapshot.group_name = "Downstairs";
    snapshot.group_repeat = sendspin::SendspinRepeatMode::ALL;
    snapshot.group_shuffle = true;
    snapshot.player_volume = 80;
    snapshot.player_muted = true;
    snapshot.player_volume_source = VolumeSource::Local;
    snapshot.output_delay_ms = 120;
    snapshot.output = "portaudio";
    snapshot.input = "hw:1,0";
    snapshot.input_format = {48000, 1, 16};
    snapshot.input_streaming = true;
    return snapshot;
}

}  // namespace

TEST(StatusKv, TheFirstLineIsTheVersion) {
    const std::string payload = format_status_kv(full_snapshot());
    EXPECT_EQ(payload.substr(0, payload.find('\n')), "version=1");
}

TEST(StatusKv, ARoundTripKeepsEveryField) {
    const StatusSnapshot snapshot = full_snapshot();
    PlayerView view;
    ASSERT_TRUE(parse_status_kv(format_status_kv(snapshot), view));

    EXPECT_EQ(view.name, "living-room");
    EXPECT_EQ(view.client_id, "abc123");
    EXPECT_TRUE(view.connected);
    EXPECT_EQ(view.server_name, "Music Assistant");
    EXPECT_EQ(view.server_id, "OraobU4l");
    EXPECT_EQ(view.trust, "paired");
    EXPECT_EQ(view.paired_servers, 2u);
    EXPECT_EQ(view.pairing_server_id, "other-server");
    EXPECT_EQ(view.pairing_code, "123456");
    EXPECT_TRUE(view.pairing_window);
    EXPECT_EQ(view.last_pairing_attempt, 3u);
    EXPECT_EQ(view.last_pairing_outcome, "failed");
    EXPECT_EQ(view.last_pairing_server_id, "OraobU4l");
    EXPECT_EQ(view.last_pairing_reason, "wrong code");
    EXPECT_EQ(view.state, status_state(snapshot));
    EXPECT_EQ(view.state, "playing");
    EXPECT_TRUE(view.streaming);
    EXPECT_EQ(view.codec, "flac");
    EXPECT_EQ(view.sample_rate, 48000u);
    EXPECT_EQ(view.channels, 2);
    EXPECT_EQ(view.bit_depth, 16);
    EXPECT_EQ(view.title, "Says");
    EXPECT_EQ(view.artist, "Nils Frahm");
    EXPECT_EQ(view.album, "Spaces");
    EXPECT_EQ(view.position_ms, std::optional<uint32_t>(125000));
    EXPECT_EQ(view.duration_ms, std::optional<uint32_t>(543000));
    EXPECT_TRUE(view.group_state_known);
    EXPECT_EQ(view.group_volume, 55);
    EXPECT_TRUE(view.group_muted);
    EXPECT_EQ(view.group_name, "Downstairs");
    EXPECT_EQ(view.repeat, "all");
    EXPECT_TRUE(view.shuffle);
    EXPECT_EQ(view.player_volume, 80);
    EXPECT_TRUE(view.player_muted);
    EXPECT_EQ(view.player_volume_source, "local");
    EXPECT_EQ(view.output_delay_ms, 120);
    EXPECT_EQ(view.output, "portaudio");
    EXPECT_EQ(view.input, "hw:1,0");
    EXPECT_TRUE(view.input_streaming);
}

TEST(StatusKv, UnknownValuesAreEmptyAndParseAsUnknown) {
    StatusSnapshot snapshot;  // not connected, no progress, no group state
    const std::string payload = format_status_kv(snapshot);
    EXPECT_NE(payload.find("\nposition_ms=\n"), std::string::npos);
    EXPECT_NE(payload.find("\ntrust=\n"), std::string::npos);
    EXPECT_NE(payload.find("\nrepeat=\n"), std::string::npos);

    PlayerView view;
    ASSERT_TRUE(parse_status_kv(payload, view));
    EXPECT_FALSE(view.position_ms.has_value());
    EXPECT_FALSE(view.duration_ms.has_value());
    EXPECT_FALSE(view.group_state_known);
    EXPECT_EQ(view.state, "unknown");
}

TEST(StatusKv, ANewlineInAValueCannotStartAKey) {
    StatusSnapshot snapshot = full_snapshot();
    snapshot.title = "a=b\nc";
    snapshot.artist = "x";
    PlayerView view;
    ASSERT_TRUE(parse_status_kv(format_status_kv(snapshot), view));
    EXPECT_EQ(view.title, "a=b c");
    EXPECT_EQ(view.artist, "x");
}

TEST(StatusKv, UnknownKeysAreIgnoredAndMissingKeysKeepDefaults) {
    PlayerView view;
    ASSERT_TRUE(parse_status_kv("version=1\nfuture_key=7\ntitle=T\n", view));
    EXPECT_EQ(view.title, "T");
    EXPECT_FALSE(view.connected);
}

TEST(StatusKv, AWrongVersionIsRefused) {
    PlayerView view;
    EXPECT_FALSE(parse_status_kv("version=2\n", view));
    EXPECT_FALSE(parse_status_kv("title=T\n", view));
}

}  // namespace sendspin_cli
