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

/// PlayerListener's stream events and volume handling, against a client that is never started.

#include "player_listener.h"

#include "null_sink.h"
#include "state_store.h"

#include <sendspin/client.h>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

/// A listener on an unstarted client's player role, recording every stream event.
struct Harness {
    explicit Harness(StateStore* store = nullptr)
        : client(sendspin::SendspinClientConfig{}),
          player(client.add_player(player_config())),
          sink(NullSinkOutput::Discard),
          listener(player, sink, store) {
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

/// A sink that logs flushes and releases, beside the listener's stop events.
class RecordingSink final : public AudioSink {
public:
    explicit RecordingSink(std::vector<std::string>& log) : log_(log) {}

    std::string name() const override {
        return "recording";
    }
    bool configure(uint32_t /*sample_rate*/, uint8_t /*channels*/, uint8_t /*bits*/) override {
        return true;
    }
    size_t write(const uint8_t* /*data*/, size_t length, uint32_t /*timeout_ms*/) override {
        return length;
    }
    void clear() override {
        this->log_.emplace_back("clear");
    }
    void release() override {
        this->log_.emplace_back("release");
    }

private:
    std::vector<std::string>& log_;
};

/// A listener over a RecordingSink, logging stream events into the sink's log.
struct RecordingHarness {
    RecordingHarness()
        : client(sendspin::SendspinClientConfig{}),
          player(client.add_player(Harness::player_config())),
          sink(log),
          listener(player, sink) {
        this->listener.on_stream_event = [this](bool started) {
            this->log.emplace_back(started ? "start" : "stop");
        };
    }

    std::vector<std::string> log;
    sendspin::SendspinClient client;
    sendspin::PlayerRole& player;
    RecordingSink sink;
    PlayerListener listener;
};

/// A state file under the build tree holding a remembered volume, removed afterwards.
class RememberedVolume {
public:
    RememberedVolume() {
        this->dir_ = "player-listener-test-" + std::to_string(getpid());
        this->created_ = ::mkdir(this->dir_.c_str(), 0700) == 0;
        StateStore store(this->path());
        this->created_ = this->created_ && store.set_volume_and_muted(42, true);
    }
    ~RememberedVolume() {
        std::remove(this->path().c_str());
        ::rmdir(this->dir_.c_str());
    }
    RememberedVolume(const RememberedVolume&) = delete;
    RememberedVolume& operator=(const RememberedVolume&) = delete;

    bool created() const {
        return this->created_;
    }
    std::string path() const {
        return this->dir_ + "/state";
    }
    std::string contents() const {
        std::ifstream in(this->path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    std::string dir_;
    bool created_{false};
};

TEST(PlayerListener, AServerVolumeIsAppliedAndPersisted) {
    RememberedVolume remembered;
    ASSERT_TRUE(remembered.created());
    StateStore store(remembered.path());
    size_t malformed = 0;
    ASSERT_EQ(store.load(malformed), StateLoadResult::Loaded);
    Harness harness(&store);
    harness.listener.restore_volume(42, true);

    harness.listener.on_volume_changed(60);
    harness.listener.on_mute_changed(false);

    EXPECT_EQ(harness.listener.applied_volume(), 60U);
    EXPECT_FALSE(harness.listener.applied_muted());
    EXPECT_EQ(harness.listener.volume_source(), VolumeSource::Server);
    EXPECT_EQ(store.volume(), 60U);
    EXPECT_EQ(store.muted(), false);
    EXPECT_NE(remembered.contents().find("volume = 60"), std::string::npos);
}

TEST(PlayerListener, AFixedVolumeIgnoresServerCommandsAndLeavesTheStateFileAlone) {
    RememberedVolume remembered;
    ASSERT_TRUE(remembered.created());
    StateStore store(remembered.path());
    size_t malformed = 0;
    ASSERT_EQ(store.load(malformed), StateLoadResult::Loaded);
    const std::string before = remembered.contents();
    Harness harness(&store);

    harness.listener.fix_volume();
    harness.listener.on_volume_changed(60);
    harness.listener.on_mute_changed(true);

    EXPECT_EQ(harness.listener.applied_volume(), DEFAULT_SINK_VOLUME);
    EXPECT_FALSE(harness.listener.applied_muted());
    EXPECT_EQ(harness.listener.volume_source(), VolumeSource::Fixed);
    EXPECT_EQ(store.volume(), 42U);
    EXPECT_EQ(store.muted(), true);
    EXPECT_EQ(remembered.contents(), before);
}

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

TEST(PlayerListener, AStreamEndClearsTheSinkByDefault) {
    RecordingHarness harness;

    harness.listener.on_stream_start();
    harness.listener.on_stream_end();

    EXPECT_EQ(harness.log, (std::vector<std::string>{"start", "clear", "stop"}));
}

TEST(PlayerListener, AStreamEndReleasesTheDeviceBeforeTheStopEventWhenAsked) {
    RecordingHarness harness;
    harness.listener.release_device_between_streams();

    harness.listener.on_stream_start();
    harness.listener.on_stream_end();

    EXPECT_EQ(harness.log, (std::vector<std::string>{"start", "release", "stop"}));
}

}  // namespace
}  // namespace sendspin_cli
