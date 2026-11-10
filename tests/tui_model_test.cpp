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

/// TuiModel: screens, keys and pairing takeovers, fed status-kv replies built by the real
/// formatter.

#include "tui_model.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

constexpr const char* SERVER_ID = "OraobU4lq7Zx2mNc";

/// A connected player, playing, with group state known and no pairing under way.
StatusSnapshot attached_snapshot() {
    StatusSnapshot snapshot;
    snapshot.name = "living-room";
    snapshot.connected = true;
    snapshot.server_name = "Music Assistant";
    snapshot.server_id = SERVER_ID;
    snapshot.paired_servers = 1;
    snapshot.playback_speed = 1000;
    snapshot.title = "Says";
    snapshot.artist = "Nils Frahm";
    snapshot.progress_ms = 125000;
    snapshot.duration_ms = 543000;
    snapshot.group_state_known = true;
    snapshot.group_volume = 55;
    snapshot.group_repeat = sendspin::SendspinRepeatMode::OFF;
    snapshot.player_volume = 80;
    snapshot.output_delay_ms = 0;
    return snapshot;
}

ControlExchange ok(const StatusSnapshot& snapshot) {
    ControlExchange reply;
    reply.status = ControlStatus::Ok;
    reply.payload = format_status_kv(snapshot);
    return reply;
}

ControlExchange refused(ControlStatus status, const std::string& reason, bool denied = false) {
    ControlExchange reply;
    reply.status = status;
    reply.reason = reason;
    reply.denied = denied;
    return reply;
}

/// A model that has seen one poll of `snapshot` at `now_ms`.
TuiModel model_with(const StatusSnapshot& snapshot, int64_t now_ms = 0) {
    TuiModel model;
    model.on_status(ok(snapshot), now_ms);
    return model;
}

}  // namespace

TEST(TuiModel, NoSocketIsNoPlayerAndDeniedIsItsOwnScreen) {
    TuiModel model;
    model.on_status(refused(ControlStatus::NoDaemon, "connect: no such file"), 0);
    EXPECT_EQ(model.screen(0), TuiScreen::NoPlayer);
    EXPECT_EQ(model.no_player_reason(), "connect: no such file");
    EXPECT_FALSE(model.request_for(TuiKey::Space).has_value());

    model.on_status(refused(ControlStatus::NoDaemon, "connect: permission denied", true), 1000);
    EXPECT_EQ(model.screen(1000), TuiScreen::Denied);
    EXPECT_EQ(model.no_player_reason(), "connect: permission denied");
    EXPECT_FALSE(model.request_for(TuiKey::Space).has_value());

    model.on_status(ok(attached_snapshot()), 2000);
    EXPECT_EQ(model.screen(2000), TuiScreen::Main);
    EXPECT_TRUE(model.request_for(TuiKey::Space).has_value());
}

TEST(TuiModel, AnOlderPlayerIsNamedAsSuch) {
    TuiModel model;
    model.on_status(refused(ControlStatus::Usage, "unknown command 'status-kv'"), 0);
    EXPECT_EQ(model.screen(0), TuiScreen::PlayerTooOld);
    EXPECT_FALSE(model.request_for(TuiKey::Space).has_value());
}

TEST(TuiModel, ACodeTakesOverTheScreen) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.pairing_server_id = SERVER_ID;
    snapshot.pairing_code = "042735";
    TuiModel model = model_with(snapshot);
    EXPECT_EQ(model.screen(0), TuiScreen::PairingCode);
    EXPECT_EQ(model.view().pairing_code, "042735");

    // The code is gone but the attempt is open: the outcome is still awaited.
    snapshot.pairing_code.clear();
    model.on_status(ok(snapshot), 1000);
    EXPECT_EQ(model.screen(1000), TuiScreen::PairingCode);

    snapshot.pairing_server_id.clear();
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.screen(2000), TuiScreen::Main);
}

TEST(TuiModel, AWaitingConfirmAsksYesOrNo) {
    StatusSnapshot snapshot = attached_snapshot();
    TuiModel model = model_with(snapshot);
    EXPECT_EQ(model.screen(0), TuiScreen::Main);
    EXPECT_FALSE(model.request_for(TuiKey::Yes).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::No).has_value());

    snapshot.pairing_server_id = SERVER_ID;
    snapshot.pairing_window_open = true;
    model.on_status(ok(snapshot), 1000);
    EXPECT_EQ(model.screen(1000), TuiScreen::PairingConfirm);

    const std::optional<ControlRequest> yes = model.request_for(TuiKey::Yes);
    ASSERT_TRUE(yes.has_value());
    EXPECT_EQ(yes->command, ControlCommand::Pair);
    EXPECT_EQ(yes->confirm, std::optional<bool>(true));

    const std::optional<ControlRequest> no = model.request_for(TuiKey::No);
    ASSERT_TRUE(no.has_value());
    EXPECT_EQ(no->command, ControlCommand::Pair);
    EXPECT_EQ(no->confirm, std::optional<bool>(false));
}

TEST(TuiModel, AnOutcomeFromBeforeLaunchIsNotReplayed) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.last_pairing = {3, SERVER_ID, PairingOutcome::Paired, ""};
    const TuiModel model = model_with(snapshot);
    EXPECT_EQ(model.screen(0), TuiScreen::Main);
}

TEST(TuiModel, ANewFailureShowsItsReasonForThreeSeconds) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.last_pairing = {3, SERVER_ID, PairingOutcome::Paired, ""};
    TuiModel model = model_with(snapshot, 1000);

    snapshot.last_pairing = {4, SERVER_ID, PairingOutcome::Failed,
                             "the pairing code did not match"};
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.screen(2000), TuiScreen::PairingOutcome);
    EXPECT_EQ(model.outcome_text(), "Pairing failed: the pairing code did not match");

    // Later polls of the same attempt do not restart the clock.
    model.on_status(ok(snapshot), 4000);
    EXPECT_EQ(model.screen(4000), TuiScreen::PairingOutcome);
    EXPECT_EQ(model.screen(2000 + 3001), TuiScreen::Main);
}

TEST(TuiModel, APairingNamesTheServerOrItsId) {
    StatusSnapshot snapshot = attached_snapshot();
    TuiModel model = model_with(snapshot);

    snapshot.last_pairing = {1, SERVER_ID, PairingOutcome::Paired, ""};
    model.on_status(ok(snapshot), 1000);
    EXPECT_EQ(model.screen(1000), TuiScreen::PairingOutcome);
    EXPECT_EQ(model.outcome_text(), "Paired with Music Assistant");

    snapshot.last_pairing = {2, "0123456789abcdefghij", PairingOutcome::Paired, ""};
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.outcome_text(), "Paired with 0123456789ab...");

    snapshot.last_pairing = {3, SERVER_ID, PairingOutcome::Abandoned, ""};
    model.on_status(ok(snapshot), 3000);
    EXPECT_EQ(model.screen(3000), TuiScreen::PairingOutcome);
    EXPECT_EQ(model.outcome_text(), "The server stopped pairing");
}

TEST(TuiModel, ReattachingResetsThePairingBaseline) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.last_pairing = {5, SERVER_ID, PairingOutcome::Paired, ""};
    TuiModel model = model_with(snapshot, 0);

    model.on_status(refused(ControlStatus::NoDaemon, "connect: connection refused"), 1000);
    EXPECT_EQ(model.screen(1000), TuiScreen::NoPlayer);

    // A restarted player counts attempts from 1 again; its attempt 1 predates this attach.
    snapshot.last_pairing = {1, SERVER_ID, PairingOutcome::Failed, "timed out"};
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.screen(2000), TuiScreen::Main);
}

TEST(TuiModel, SpacePausesWhilePlayingAndPlaysOtherwise) {
    StatusSnapshot snapshot = attached_snapshot();
    TuiModel model = model_with(snapshot);
    EXPECT_EQ(model.request_for(TuiKey::Space)->command, ControlCommand::Pause);

    snapshot.playback_speed = 0;
    model.on_status(ok(snapshot), 1000);
    EXPECT_EQ(model.request_for(TuiKey::Space)->command, ControlCommand::Play);

    snapshot.playback_speed.reset();
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.request_for(TuiKey::Space)->command, ControlCommand::Play);
}

TEST(TuiModel, VolumeKeysClampAtTheEnds) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.player_volume = 98;
    snapshot.group_volume = 3;
    const TuiModel model = model_with(snapshot);

    const std::optional<ControlRequest> up = model.request_for(TuiKey::Up);
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->command, ControlCommand::PlayerVolume);
    EXPECT_EQ(up->volume, std::optional<uint8_t>(100));
    EXPECT_EQ(model.request_for(TuiKey::Down)->volume, std::optional<uint8_t>(93));

    const std::optional<ControlRequest> down = model.request_for(TuiKey::LeftBracket);
    ASSERT_TRUE(down.has_value());
    EXPECT_EQ(down->command, ControlCommand::Volume);
    EXPECT_EQ(down->volume, std::optional<uint8_t>(0));
    EXPECT_EQ(model.request_for(TuiKey::RightBracket)->volume, std::optional<uint8_t>(8));
}

TEST(TuiModel, TheToggleAndTransportKeysSendWhatTheyName) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.player_muted = true;
    snapshot.group_muted = false;
    snapshot.group_shuffle = true;
    const TuiModel model = model_with(snapshot);

    const std::optional<ControlRequest> mute_self = model.request_for(TuiKey::MuteSelf);
    ASSERT_TRUE(mute_self.has_value());
    EXPECT_EQ(mute_self->command, ControlCommand::PlayerMute);
    EXPECT_EQ(mute_self->flag, std::optional<bool>(false));

    const std::optional<ControlRequest> mute_group = model.request_for(TuiKey::MuteGroup);
    ASSERT_TRUE(mute_group.has_value());
    EXPECT_EQ(mute_group->command, ControlCommand::Mute);
    EXPECT_EQ(mute_group->flag, std::optional<bool>(true));

    const std::optional<ControlRequest> shuffle = model.request_for(TuiKey::Shuffle);
    ASSERT_TRUE(shuffle.has_value());
    EXPECT_EQ(shuffle->command, ControlCommand::Shuffle);
    EXPECT_EQ(shuffle->flag, std::optional<bool>(false));

    EXPECT_EQ(model.request_for(TuiKey::Left)->command, ControlCommand::Previous);
    EXPECT_EQ(model.request_for(TuiKey::Right)->command, ControlCommand::Next);
    EXPECT_EQ(model.request_for(TuiKey::Switch)->command, ControlCommand::Switch);
    EXPECT_FALSE(model.request_for(TuiKey::Token).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::Quit).has_value());
}

TEST(TuiModel, RepeatCyclesOffAllOne) {
    StatusSnapshot snapshot = attached_snapshot();
    TuiModel model;
    const auto next_repeat = [&](sendspin::SendspinRepeatMode current) {
        snapshot.group_repeat = current;
        model.on_status(ok(snapshot), 0);
        const std::optional<ControlRequest> request = model.request_for(TuiKey::Repeat);
        EXPECT_TRUE(request.has_value() && request->command == ControlCommand::Repeat);
        return request.has_value() ? request->repeat : std::nullopt;
    };
    using sendspin::SendspinRepeatMode;
    EXPECT_EQ(next_repeat(SendspinRepeatMode::OFF), std::optional(SendspinRepeatMode::ALL));
    EXPECT_EQ(next_repeat(SendspinRepeatMode::ALL), std::optional(SendspinRepeatMode::ONE));
    EXPECT_EQ(next_repeat(SendspinRepeatMode::ONE), std::optional(SendspinRepeatMode::OFF));
}

TEST(TuiModel, DelayClampsWithinTheSpecRange) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.output_delay_ms = 4995;
    TuiModel model = model_with(snapshot);
    const std::optional<ControlRequest> up = model.request_for(TuiKey::DelayUp);
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->command, ControlCommand::Delay);
    EXPECT_EQ(up->delay_ms, std::optional<uint16_t>(5000));
    EXPECT_EQ(model.request_for(TuiKey::DelayDown)->delay_ms, std::optional<uint16_t>(4985));

    snapshot.output_delay_ms = 5;
    model.on_status(ok(snapshot), 1000);
    EXPECT_EQ(model.request_for(TuiKey::DelayDown)->delay_ms, std::optional<uint16_t>(0));
    EXPECT_EQ(model.request_for(TuiKey::DelayUp)->delay_ms, std::optional<uint16_t>(15));
}

TEST(TuiModel, GroupKeysDoNothingWhileTheGroupIsUnknown) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.group_state_known = false;
    const TuiModel model = model_with(snapshot);
    EXPECT_FALSE(model.request_for(TuiKey::RightBracket).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::LeftBracket).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::MuteGroup).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::Repeat).has_value());
    EXPECT_FALSE(model.request_for(TuiKey::Shuffle).has_value());
    // This player's own controls do not depend on the group.
    EXPECT_TRUE(model.request_for(TuiKey::Up).has_value());
    EXPECT_TRUE(model.request_for(TuiKey::MuteSelf).has_value());
}

TEST(TuiModel, AnUnsupportedCommandLeavesANotice) {
    TuiModel model = model_with(attached_snapshot());
    EXPECT_EQ(model.notice(0), "");

    model.on_command_reply(refused(ControlStatus::Unsupported, "server does not offer that"),
                           1000);
    EXPECT_EQ(model.notice(1000), "The server doesn't offer that");
    EXPECT_EQ(model.notice(1000 + 3001), "");

    model.on_command_reply(refused(ControlStatus::NotConnected, "not connected"), 5000);
    EXPECT_EQ(model.notice(5000), "No server is connected");

    model.on_command_reply(refused(ControlStatus::Usage, "volume must be 0-100"), 6000);
    EXPECT_EQ(model.notice(6000), "volume must be 0-100");

    TuiModel quiet = model_with(attached_snapshot());
    quiet.on_command_reply(ok(attached_snapshot()), 1000);
    EXPECT_EQ(quiet.notice(1000), "");
}

TEST(TuiModel, ThePositionAdvancesOnlyWhilePlaying) {
    StatusSnapshot snapshot = attached_snapshot();
    snapshot.progress_ms = 125000;
    snapshot.duration_ms = 126000;
    TuiModel model = model_with(snapshot, 1000);
    EXPECT_EQ(model.position_ms(1500), std::optional<uint32_t>(125500));
    EXPECT_EQ(model.position_ms(9000), std::optional<uint32_t>(126000));  // capped

    snapshot.duration_ms = 0;  // live: nothing to cap at
    model.on_status(ok(snapshot), 2000);
    EXPECT_EQ(model.position_ms(9000), std::optional<uint32_t>(132000));

    snapshot.playback_speed = 0;
    model.on_status(ok(snapshot), 3000);
    EXPECT_EQ(model.position_ms(9000), std::optional<uint32_t>(125000));

    snapshot.progress_ms.reset();
    model.on_status(ok(snapshot), 4000);
    EXPECT_FALSE(model.position_ms(9000).has_value());
}

TEST(TuiModel, TheTokenIsFetchedOnceWhileShown) {
    TuiModel model = model_with(attached_snapshot());
    EXPECT_FALSE(model.wants_token());

    model.toggle_token();
    EXPECT_TRUE(model.wants_token());
    ControlExchange reply;
    reply.status = ControlStatus::Ok;
    reply.payload = "s3cret\n";
    model.on_token(reply);
    EXPECT_FALSE(model.wants_token());
    EXPECT_EQ(model.token(), std::optional<std::string>("s3cret"));

    // Hiding forgets it; showing again asks again.
    model.toggle_token();
    EXPECT_FALSE(model.token().has_value());
    EXPECT_FALSE(model.wants_token());
    model.toggle_token();
    EXPECT_TRUE(model.wants_token());
}

TEST(TuiModel, ThePairingCodeFallsBackToPlainTextWhenNarrow) {
    const std::vector<std::string> wide = pairing_code_lines("042735", 80);
    EXPECT_EQ(wide.size(), 5u);

    // 6 digits need 6*6-1 = 35 columns.
    EXPECT_EQ(pairing_code_lines("042735", 35).size(), 5u);

    const std::vector<std::string> narrow = pairing_code_lines("042735", 30);
    ASSERT_EQ(narrow.size(), 1u);
    EXPECT_NE(narrow[0].find("0 4 2 7 3 5"), std::string::npos);
}

}  // namespace sendspin_cli
