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

/// PlayerListener's stream events, against a client that is never started.

#include "player_listener.h"

#include "null_sink.h"

#include <sendspin/client.h>

#include <gtest/gtest.h>

#include <vector>

namespace sendspin_cli {
namespace {

/// A listener on an unstarted client's player role, recording every stream event.
struct Harness {
    Harness()
        : client(sendspin::SendspinClientConfig{}),
          player(client.add_player(player_config())),
          sink(NullSinkOutput::Discard),
          listener(player, sink) {
        this->listener.on_stream_event = [this](bool started) { this->events.push_back(started); };
    }

    static sendspin::PlayerRoleConfig player_config() {
        sendspin::PlayerRoleConfig config;
        config.audio_formats = {{sendspin::SendspinCodecFormat::PCM, 2, 48000, 16}};
        return config;
    }

    sendspin::SendspinClient client;
    sendspin::PlayerRole& player;
    NullAudioSink sink;
    PlayerListener listener;
    std::vector<bool> events;
};

TEST(PlayerListener, AnEndWithNoStartFiresNoStopEvent) {
    Harness harness;

    harness.listener.on_stream_end();

    EXPECT_TRUE(harness.events.empty());
    EXPECT_FALSE(harness.listener.streaming());
}

TEST(PlayerListener, ARepeatedEndFiresOneStopEventPerStart) {
    Harness harness;

    harness.listener.on_stream_start();
    harness.listener.on_stream_end();
    harness.listener.on_stream_end();

    EXPECT_EQ(harness.events, (std::vector<bool>{true, false}));
}

}  // namespace
}  // namespace sendspin_cli
