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

/// PairingListener's attempt tracking across main-loop ticks, in the orders loop() can deliver.

#include "pairing_listener.h"

#include <gtest/gtest.h>

namespace sendspin_cli {
namespace {

constexpr const char* SERVER = "server-a";
constexpr const char* OTHER_SERVER = "server-b";

TEST(PairingListener, KeepsAnAttemptStartedBeforeTheSessionReadsConnected) {
    // The first server/activate can start pairing: loop() delivers the start, and only the tick
    // after it sees is_connected() true.
    PairingListener listener;
    listener.tick(false, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.tick(true, false, 10);
    EXPECT_EQ(listener.server_id(), SERVER);
}

TEST(PairingListener, KeepsAnAttemptWhileTheConnectionStays) {
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.tick(true, false, 10);
    listener.tick(true, false, 20);
    EXPECT_EQ(listener.server_id(), SERVER);
}

TEST(PairingListener, ForgetsAnAttemptWhoseConnectionDropped) {
    // A dropped connection ends its attempt without a callback.
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.tick(true, false, 10);
    listener.tick(false, false, 20);
    EXPECT_EQ(listener.server_id(), "");
    listener.tick(true, false, 30);
    EXPECT_EQ(listener.server_id(), "");
}

TEST(PairingListener, KeepsAnAttemptDeliveredInTheLoopThatDropsTheConnection) {
    // One server drops and the next one's first server/activate starts pairing between two ticks:
    // the tick that sees the drop must not take the new attempt with it.
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(OTHER_SERVER);
    listener.tick(false, false, 10);
    listener.tick(true, false, 20);
    EXPECT_EQ(listener.server_id(), OTHER_SERVER);
}

TEST(PairingListener, AnOutcomeEndsTheAttempt) {
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.on_paired(SERVER);
    EXPECT_EQ(listener.server_id(), "");

    listener.on_pairing_attempt(SERVER);
    listener.on_pairing_aborted(SERVER, sendspin::SendspinPairAbortReason::USER_CANCELLED);
    EXPECT_EQ(listener.server_id(), "");
}

TEST(PairingListener, AWithdrawnPromptEndsTheAttempt) {
    // How an attempt that showed a code or a window ends when the server cancels it.
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.on_pairing_code("123456", sendspin::SendspinPairingCodeFormat{});
    listener.on_clear_pairing_code();
    EXPECT_EQ(listener.server_id(), "");
    EXPECT_EQ(listener.code(), "");

    listener.on_pairing_attempt(SERVER);
    listener.on_open_pairing_window();
    listener.on_close_pairing_window();
    EXPECT_EQ(listener.server_id(), "");
    EXPECT_FALSE(listener.window_open());
}

TEST(PairingListener, ForgetsAnUnpromptedAttemptPastTheLibraryTimeout) {
    // A server that cancels a token attempt with server/activate ends it without a callback.
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.tick(true, false, 1000);
    listener.tick(true, false, 1000 + UNPROMPTED_ATTEMPT_LIMIT_MS - 1);
    EXPECT_EQ(listener.server_id(), SERVER);
    listener.tick(true, false, 1000 + UNPROMPTED_ATTEMPT_LIMIT_MS);
    EXPECT_EQ(listener.server_id(), "");
}

TEST(PairingListener, KeepsAPromptedAttemptPastTheLibraryTimeout) {
    // A prompted attempt ends when its prompt is withdrawn, so the fallback leaves it alone.
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.on_pairing_code("123456", sendspin::SendspinPairingCodeFormat{});
    listener.tick(true, false, 1000);
    listener.tick(true, false, 1000 + (2 * UNPROMPTED_ATTEMPT_LIMIT_MS));
    EXPECT_EQ(listener.server_id(), SERVER);
}

TEST(PairingListener, ANewAttemptRestartsTheTimeout) {
    PairingListener listener;
    listener.tick(true, false, 0);
    listener.on_pairing_attempt(SERVER);
    listener.tick(true, false, 1000);
    listener.on_pairing_attempt(OTHER_SERVER);
    listener.tick(true, false, 1000 + UNPROMPTED_ATTEMPT_LIMIT_MS);
    EXPECT_EQ(listener.server_id(), OTHER_SERVER);
}

}  // namespace
}  // namespace sendspin_cli
