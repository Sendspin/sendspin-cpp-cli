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

/// Subcommand client: connect, send one line, print the reply. Opens nothing else.

#include "control.h"

#include "socket_compat.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <filesystem>
#endif

namespace sendspin_cli {

namespace {

/// The longest reply this will read, in bytes.
constexpr size_t MAX_REPLY_BYTES = 64 * 1024;

/// How long one send or receive may wait, so a wedged player cannot hang the caller.
constexpr unsigned ANSWER_TIMEOUT_MS = 3000;
constexpr const char* ANSWER_TIMEOUT_REASON = "the player did not answer within 3 s";

/// Everything up to the first newline, and everything after it.
void split_first_line(const std::string& reply, std::string& first, std::string& rest) {
    const size_t newline = reply.find('\n');
    if (newline == std::string::npos) {
        first = reply;
        rest.clear();
        return;
    }
    first = reply.substr(0, newline);
    rest = reply.substr(newline + 1);
}

/// Connects to `path`, or explains why not in `result`: NoDaemon for no socket, nothing
/// accepting or no permission, Failed for a player that would not make room in time.
socket_t connect_to_socket(const std::string& path, ControlExchange& result) {
    result.status = ControlStatus::NoDaemon;
    result.denied = false;
    std::string& error = result.reason;
    // Re-checked because a memcpy into sun_path follows.
    if (!control_socket_path_fits(path)) {
        error = "control socket path '" + path + "' does not fit a Unix socket address (" +
                std::to_string(control_socket_path_limit() - 1) + " bytes)";
        return INVALID_SOCKET_HANDLE;
    }

    if (!socket_startup()) {
        error = "cannot start the socket library: " + socket_error_text(socket_error());
        return INVALID_SOCKET_HANDLE;
    }

    const socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_HANDLE) {
        error = "cannot create a socket: " + socket_error_text(socket_error());
        return INVALID_SOCKET_HANDLE;
    }

    // Before connect(): on Linux, a wedged player's full backlog would otherwise block it forever.
    // Windows refuses a full backlog at once, and its connect() takes no bound from this.
    if (!set_io_timeout(fd, ANSWER_TIMEOUT_MS)) {
        result.status = ControlStatus::Failed;
        error = "cannot bound the wait for the player: " + socket_error_text(socket_error());
        close_socket(fd);
        return INVALID_SOCKET_HANDLE;
    }

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return fd;
    }

    const int reason = socket_error();
    close_socket(fd);
    ConnectFailure failure = classify_connect_error(reason);
#ifdef _WIN32
    std::error_code ignored;
    if (failure == ConnectFailure::Refused && !std::filesystem::exists(path, ignored)) {
        failure = ConnectFailure::Missing;
    }
#endif
    if (failure == ConnectFailure::TimedOut) {
        result.status = ControlStatus::Failed;
        error = ANSWER_TIMEOUT_REASON;
    } else if (failure == ConnectFailure::Missing) {
        error = "no sendspin-cli is listening on " + path +
                ". Start one, or point this at the right socket with --control-socket -- and "
                "note that a non-default --port moves the default path, so the same --port has "
                "to be given here";
    } else if (failure == ConnectFailure::Refused) {
        error = path +
                " exists but nothing accepted the connection: either the player it belonged to "
                "is gone and left the file behind, or it already has as many control connections "
                "open as it will take. Try again, and if it keeps failing, restart the player";
    } else if (failure == ConnectFailure::Denied) {
        error = "not allowed to connect to " + path +
                ": the socket is 0600, so only the user running the player can drive it "
                "-- under the system service, run it with sudo";
        result.denied = true;
    } else {
        error = "cannot connect to " + path + ": " + socket_error_text(reason);
    }
    return INVALID_SOCKET_HANDLE;
}

/// Writes the whole of `text`, or says why it could not.
/// @param timed_out Set when the player stopped taking the request.
bool write_all(socket_t fd, const std::string& text, std::string& error, bool& timed_out) {
    size_t written = 0;
    while (written < text.size()) {
        const long wrote = send_bytes(fd, text.data() + written, text.size() - written);
        if (wrote > 0) {
            written += static_cast<size_t>(wrote);
            continue;
        }
        const int reason = wrote < 0 ? socket_error() : 0;
        if (wrote < 0 && socket_interrupted(reason)) {
            continue;
        }
        if (wrote < 0 && socket_timed_out(reason)) {
            timed_out = true;
            error = ANSWER_TIMEOUT_REASON;
            return false;
        }
        error = "cannot send the request: " +
                (wrote < 0 ? socket_error_text(socket_error()) : std::string("short write"));
        return false;
    }
    return true;
}

/// Reads until the daemon closes, which is its end-of-reply marker, within ANSWER_TIMEOUT_MS.
bool read_reply(socket_t fd, std::string& reply, std::string& error) {
    char buffer[4096];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ANSWER_TIMEOUT_MS);
    while (true) {
        // Wait first, so the read never blocks: see wait_readable() for why that matters.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int ready = wait_readable(fd, static_cast<int>(std::max<int64_t>(left.count(), 0)));
        if (ready == 0) {
            error = ANSWER_TIMEOUT_REASON;
            return false;
        }
        if (ready < 0) {
            const int reason = socket_error();
            if (socket_interrupted(reason)) {
                continue;
            }
            error = "cannot read the reply: " + socket_error_text(reason);
            return false;
        }
        const long read_bytes = receive_bytes(fd, buffer, sizeof(buffer));
        if (read_bytes > 0) {
            if (reply.size() + static_cast<size_t>(read_bytes) > MAX_REPLY_BYTES) {
                error = "the reply is longer than " + std::to_string(MAX_REPLY_BYTES) +
                        " bytes, which is not something a sendspin-cli daemon sends";
                return false;
            }
            reply.append(buffer, static_cast<size_t>(read_bytes));
            continue;
        }
        if (read_bytes == 0) {
            return true;
        }
        const int reason = socket_error();
        if (socket_interrupted(reason)) {
            continue;
        }
        if (socket_timed_out(reason)) {
            error = ANSWER_TIMEOUT_REASON;
            return false;
        }
        error = "cannot read the reply: " + socket_error_text(reason);
        return false;
    }
}

}  // namespace

ControlExchange exchange_control_request(const ControlRequest& request, const std::string& path,
                                         const std::string& absent_reason) {
#ifndef _WIN32
    // A daemon closing before our write would raise SIGPIPE; ignore it and report EPIPE.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    ControlExchange result;
    if (path.empty()) {
        result.status = ControlStatus::NoDaemon;
        result.reason = absent_reason.empty()
                            ? "there is no control socket to talk to: this player was started with "
                              "--no-control, or --control-socket was not given"
                            : absent_reason;
        return result;
    }

    const socket_t fd = connect_to_socket(path, result);
    if (fd == INVALID_SOCKET_HANDLE) {
        return result;
    }
    result.reason.clear();

    std::string error;
    bool timed_out = false;
    const bool sent = write_all(fd, encode_control_request(request) + "\n", error, timed_out);
    if (timed_out) {
        // Reading too would wait out a second bound for the same silent player.
        close_socket(fd);
        result.status = ControlStatus::Failed;
        result.reason = error;
        return result;
    }

    std::string reply;
    // Read even after a failed write: the daemon's `error` reply beats our guess.
    const bool received = read_reply(fd, reply, error);
    close_socket(fd);

    result.status = ControlStatus::Failed;
    if (!sent && reply.empty()) {
        result.reason = error;
        return result;
    }
    if (!received) {
        result.reason = error;
        return result;
    }
    if (reply.empty()) {
        result.reason = "the daemon closed the connection without answering";
        return result;
    }

    std::string first_line;
    split_first_line(reply, first_line, result.payload);

    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    if (!decode_control_reply(first_line, status, reason)) {
        result.payload.clear();
        result.reason = path + " answered '" + first_line + "', which is not a sendspin-cli reply";
        return result;
    }

    result.status = status;
    result.reason = reason;
    if (status != ControlStatus::Ok) {
        result.payload.clear();
    }
    return result;
}

ControlStatus run_control_subcommand(const ControlRequest& request, const std::string& path,
                                     const std::string& absent_reason, std::FILE* out) {
    const ControlExchange result = exchange_control_request(request, path, absent_reason);
    if (result.status != ControlStatus::Ok) {
        std::fprintf(stderr, "error: %s\n", result.reason.c_str());
        return result.status;
    }
    std::fwrite(result.payload.data(), 1, result.payload.size(), out);
    return ControlStatus::Ok;
}

}  // namespace sendspin_cli
