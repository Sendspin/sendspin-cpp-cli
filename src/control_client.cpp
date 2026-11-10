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

#include <csignal>
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

/// Connects to `path`, or explains why not: no socket, nothing accepting, or no permission.
socket_t connect_to_socket(const std::string& path, std::string& error, bool& denied) {
    denied = false;
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
    if (failure == ConnectFailure::Missing) {
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
        denied = true;
    } else {
        error = "cannot connect to " + path + ": " + socket_error_text(reason);
    }
    return INVALID_SOCKET_HANDLE;
}

/// Writes the whole of `text`, or says why it could not.
bool write_all(socket_t fd, const std::string& text, std::string& error) {
    size_t written = 0;
    while (written < text.size()) {
        const long wrote = send_bytes(fd, text.data() + written, text.size() - written);
        if (wrote > 0) {
            written += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote < 0 && socket_interrupted(socket_error())) {
            continue;
        }
        error = "cannot send the request: " +
                (wrote < 0 ? socket_error_text(socket_error()) : std::string("short write"));
        return false;
    }
    return true;
}

/// Reads until the daemon closes, which is its end-of-reply marker.
bool read_reply(socket_t fd, std::string& reply, std::string& error) {
    char buffer[4096];
    while (true) {
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

    std::string error;
    const socket_t fd = connect_to_socket(path, error, result.denied);
    if (fd == INVALID_SOCKET_HANDLE) {
        result.status = ControlStatus::NoDaemon;
        result.reason = error;
        return result;
    }

    const bool sent = write_all(fd, encode_control_request(request) + "\n", error);

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
