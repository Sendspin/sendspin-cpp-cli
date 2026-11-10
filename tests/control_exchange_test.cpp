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

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
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

TEST(ControlExchange, APlayerThatNeverAnswersTimesOut) {
    const ScratchDir dir;
    ASSERT_TRUE(dir.created());
    // Bound but never pumped: the connection waits in the backlog and nothing ever replies.
    ControlSocket socket;
    std::string error;
    ASSERT_EQ(socket.open(dir.socket_path(), error), ControlSocketStatus::Ok) << error;

    std::promise<ControlExchange> promise;
    std::future<ControlExchange> future = promise.get_future();
    const auto start = std::chrono::steady_clock::now();
    std::thread client([&promise, &dir] {
        promise.set_value(exchange_control_request(ControlRequest{}, dir.socket_path(), ""));
    });
    const bool answered = future.wait_for(std::chrono::seconds(6)) == std::future_status::ready;
    if (!answered) {
        // Closing the listener resets the waiting connection, so the client returns and joins.
        socket.close();
    }
    client.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(answered) << "the exchange was still waiting after 6 s";
    const ControlExchange result = future.get();
    EXPECT_EQ(result.status, ControlStatus::Failed);
    EXPECT_EQ(result.reason, "the player did not answer within 3 s");
    EXPECT_GE(elapsed, std::chrono::milliseconds(2500));
    EXPECT_LT(elapsed, std::chrono::milliseconds(4500));
}

#ifdef __linux__
// Linux only: there a connect to a full backlog waits for room, bounded only by SO_SNDTIMEO.
// Windows and macOS refuse it at once, so nothing can hang there.
TEST(ControlExchange, AFullBacklogTimesOutTheConnect) {
    const ScratchDir dir;
    ASSERT_TRUE(dir.created());
    // Bound but never pumped, so nothing ever leaves the accept backlog.
    ControlSocket socket;
    std::string error;
    ASSERT_EQ(socket.open(dir.socket_path(), error), ControlSocketStatus::Ok) << error;

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, dir.socket_path().c_str(), sizeof(address.sun_path) - 1);
    std::vector<int> fillers;
    bool full = false;
    while (!full && fillers.size() < 64) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK), 0);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            fillers.push_back(fd);
            continue;
        }
        const int reason = errno;
        ::close(fd);
        ASSERT_TRUE(reason == EAGAIN || reason == EWOULDBLOCK) << std::strerror(reason);
        full = true;
    }
    ASSERT_TRUE(full) << "the backlog never filled";

    std::promise<ControlExchange> promise;
    std::future<ControlExchange> future = promise.get_future();
    const auto start = std::chrono::steady_clock::now();
    std::thread client([&promise, &dir] {
        promise.set_value(exchange_control_request(ControlRequest{}, dir.socket_path(), ""));
    });
    const bool answered = future.wait_for(std::chrono::seconds(6)) == std::future_status::ready;
    if (!answered) {
        // Closing the listener wakes the waiting connect, so the client returns and joins.
        socket.close();
    }
    client.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    for (const int fd : fillers) {
        ::close(fd);
    }

    ASSERT_TRUE(answered) << "the connect was still waiting after 6 s";
    const ControlExchange result = future.get();
    EXPECT_EQ(result.status, ControlStatus::Failed);
    EXPECT_EQ(result.reason, "the player did not answer within 3 s");
    EXPECT_GE(elapsed, std::chrono::milliseconds(2500));
    EXPECT_LT(elapsed, std::chrono::milliseconds(4500));
}
#endif

}  // namespace
}  // namespace sendspin_cli
