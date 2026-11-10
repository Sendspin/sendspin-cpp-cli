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

/// exchange_control_request against no socket, and against a live ControlSocket.

#include "control.h"

#include "posix_compat.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace sendspin_cli {
namespace {

/// A scratch directory under the build tree, per test, removed afterwards.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ =
            "cx-" + std::to_string(getpid()) + "-" + std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
        std::remove(this->socket_path().c_str());
        std::remove((this->socket_path() + ".lock").c_str());
        ::rmdir(this->path_.c_str());
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    bool created() const {
        return this->created_;
    }

    /// Where a socket would live; short, because sun_path is ~108 bytes.
    std::string socket_path() const {
        return this->path_ + "/s";
    }

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
    bool created_{false};
};

/// Answers every request with a fixed reply.
class FixedHandler : public ControlHandler {
public:
    explicit FixedHandler(std::string reply) : reply_(std::move(reply)) {}

    std::string handle_control_request(const std::string& /*line*/) override {
        return this->reply_;
    }

private:
    std::string reply_;
};

/// A ControlSocket pumped on its own thread until destruction.
class PumpedSocket {
public:
    PumpedSocket(const std::string& path, ControlHandler& handler) {
        std::string error;
        this->opened_ = this->socket_.open(path, error) == ControlSocketStatus::Ok;
        if (!this->opened_) {
            return;
        }
        this->thread_ = std::thread([this, &handler] {
            const auto start = std::chrono::steady_clock::now();
            while (!this->stop_.load()) {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                this->socket_.poll(
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                    handler);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    ~PumpedSocket() {
        this->stop_.store(true);
        if (this->thread_.joinable()) {
            this->thread_.join();
        }
        this->socket_.close();
    }

    PumpedSocket(const PumpedSocket&) = delete;
    PumpedSocket& operator=(const PumpedSocket&) = delete;

    bool opened() const {
        return this->opened_;
    }

private:
    ControlSocket socket_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool opened_{false};
};

TEST(ControlExchange, AMissingSocketIsNoDaemonAndNotDenied) {
    const ScratchDir dir;
    ASSERT_TRUE(dir.created());

    const ControlExchange result =
        exchange_control_request(ControlRequest{}, dir.socket_path(), "");
    EXPECT_EQ(result.status, ControlStatus::NoDaemon);
    EXPECT_FALSE(result.denied);
    EXPECT_FALSE(result.reason.empty());
}

TEST(ControlExchange, AReplyIsSplitIntoStatusAndPayload) {
    const ScratchDir dir;
    ASSERT_TRUE(dir.created());
    FixedHandler handler(encode_control_reply(ControlStatus::Ok, "", "a=1\n"));
    const PumpedSocket pumped(dir.socket_path(), handler);
    ASSERT_TRUE(pumped.opened());

    const ControlExchange result =
        exchange_control_request(ControlRequest{}, dir.socket_path(), "");
    EXPECT_EQ(result.status, ControlStatus::Ok);
    EXPECT_EQ(result.payload, "a=1\n");
    EXPECT_TRUE(result.reason.empty());
    EXPECT_FALSE(result.denied);
}

TEST(ControlExchange, AnErrorReplyCarriesItsReason) {
    const ScratchDir dir;
    ASSERT_TRUE(dir.created());
    FixedHandler handler(encode_control_reply(ControlStatus::Unsupported, "nope", ""));
    const PumpedSocket pumped(dir.socket_path(), handler);
    ASSERT_TRUE(pumped.opened());

    const ControlExchange result =
        exchange_control_request(ControlRequest{}, dir.socket_path(), "");
    EXPECT_EQ(result.status, ControlStatus::Unsupported);
    EXPECT_EQ(result.reason, "nope");
    EXPECT_TRUE(result.payload.empty());
}

}  // namespace
}  // namespace sendspin_cli
