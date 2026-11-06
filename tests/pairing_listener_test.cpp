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

TEST(PairingListener, KeepsAnAttemptStartedBeforeTheSessionReadsConnected) {
    // The first server/activate can start pairing: loop() delivers the start, and only the tick
    // after it sees is_connected() true.
    PairingListener listener;
    listener.tick(false, false);
    listener.on_pairing_started(SERVER);
    listener.tick(true, false);
    EXPECT_EQ(listener.server_id(), SERVER);
}

TEST(PairingListener, KeepsAnAttemptWhileTheConnectionStays) {
    PairingListener listener;
    listener.tick(true, false);
    listener.on_pairing_started(SERVER);
    listener.tick(true, false);
    listener.tick(true, false);
    EXPECT_EQ(listener.server_id(), SERVER);
}

TEST(PairingListener, ForgetsAnAttemptWhoseConnectionDropped) {
    // A dropped connection ends its attempt without a callback.
    PairingListener listener;
    listener.tick(true, false);
    listener.on_pairing_started(SERVER);
    listener.tick(false, false);
    EXPECT_EQ(listener.server_id(), "");
    listener.tick(true, false);
    EXPECT_EQ(listener.server_id(), "");
}

TEST(PairingListener, AnOutcomeEndsTheAttempt) {
    PairingListener listener;
    listener.tick(true, false);
    listener.on_pairing_started(SERVER);
    listener.on_pairing_succeeded(SERVER);
    EXPECT_EQ(listener.server_id(), "");

    listener.on_pairing_started(SERVER);
    listener.on_pairing_failed(SERVER, sendspin::SendspinPairAbortReason::USER_CANCELLED);
    EXPECT_EQ(listener.server_id(), "");
}

}  // namespace
}  // namespace sendspin_cli
