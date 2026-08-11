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

/// @file control_client.cpp
/// @brief The subcommand half: connect, send one line, print the reply, exit
///
/// Deliberately the whole of a subcommand run. Nothing here opens an audio device, starts a
/// WebSocket server, takes a pidfile or touches mDNS -- `sendspin-cli status` must be able to
/// ask a running player what it is doing without competing with it for any of that.
///
/// Diagnostics go to stderr as plain `error:` lines rather than through the tagged logger, for
/// the reason the flag parser's do: this answers a command line, it does not record a run.

#include "control.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace sendspin_cli {

namespace {

/// The longest reply this will read, in bytes.
///
/// A bound for the same reason the daemon bounds its request: the peer decides how much to
/// send, and a `status` block is a few hundred bytes.
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

/// Connects to `path`, or explains why there is nothing there.
///
/// The distinctions this function exists to keep, because each wants a different action:
/// ENOENT is no socket at all; ECONNREFUSED is a socket file with nothing accepting behind it,
/// which is a player that died without unlinking *or* a listener whose queue is momentarily
/// full; EACCES is a player that is there and will not talk to this user.
int connect_to_socket(const std::string& path, std::string& error) {
    // Checked here as well as at parse time, because what follows is a memcpy into a fixed
    // array: the parser refusing an over-long --control-socket is an invariant held two files
    // away, and the cost of not relying on it is one comparison.
    if (!control_socket_path_fits(path)) {
        error = "control socket path '" + path + "' does not fit a Unix socket address (" +
                std::to_string(control_socket_path_limit() - 1) + " bytes)";
        return -1;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("cannot create a socket: ") + std::strerror(errno);
        return -1;
    }

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return fd;
    }

    const int reason = errno;
    ::close(fd);
    if (reason == ENOENT) {
        error = "no sendspin-cli is listening on " + path +
                ". Start one, or point this at the right socket with --control-socket -- and "
                "note that a non-default --port moves the default path, so the same --port has "
                "to be given here";
    } else if (reason == ECONNREFUSED) {
        // A socket file with nothing behind it. Worth telling apart from ENOENT: the file's
        // presence means a player was here, so the operator is looking for a crash or a busy
        // listener rather than for a path they got wrong.
        error = path +
                " exists but nothing accepted the connection: either the player it belonged to "
                "is gone and left the file behind, or it already has as many control connections "
                "open as it will take. Try again, and if it keeps failing, restart the player";
    } else if (reason == EACCES || reason == EPERM) {
        error = "not allowed to connect to " + path +
                ": the socket is 0600, so only the user running the player can drive it";
    } else {
        error = "cannot connect to " + path + ": " + std::strerror(reason);
    }
    return -1;
}

/// Writes the whole of `text`, or says why it could not.
bool write_all(int fd, const std::string& text, std::string& error) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t wrote = ::write(fd, text.data() + written, text.size() - written);
        if (wrote > 0) {
            written += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        error = "cannot send the request: " +
                std::string(wrote < 0 ? std::strerror(errno) : "short write");
        return false;
    }
    return true;
}

/// Reads until the daemon closes, which is its end-of-reply marker.
bool read_reply(int fd, std::string& reply, std::string& error) {
    char buffer[4096];
    while (true) {
        const ssize_t read_bytes = ::read(fd, buffer, sizeof(buffer));
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
        if (errno == EINTR) {
            continue;
        }
        error = std::string("cannot read the reply: ") + std::strerror(errno);
        return false;
    }
}

}  // namespace

ControlStatus run_control_subcommand(const ControlRequest& request, const std::string& path,
                                     const std::string& absent_reason, std::FILE* out) {
    // A daemon that goes away between our connect() and our write() -- because it was at its
    // connection cap, or is shutting down -- makes that write raise SIGPIPE, whose default
    // disposition would kill this process with signal 13 instead of letting the EPIPE below
    // report itself. Ignored here rather than for the whole binary, and rather than reached for
    // per-call with MSG_NOSIGNAL/SO_NOSIGPIPE, which are spelled differently on Linux and macOS.
    // This process does nothing else with a pipe or a socket.
    std::signal(SIGPIPE, SIG_IGN);

    if (path.empty()) {
        // No socket to try, and the reason names the fix -- either the daemon was told
        // --no-control, or this host gave it nowhere to put one.
        std::fprintf(stderr, "error: %s\n",
                     absent_reason.empty()
                         ? "there is no control socket to talk to: this player was started with "
                           "--no-control, or --control-socket was not given"
                         : absent_reason.c_str());
        return ControlStatus::NoDaemon;
    }

    std::string error;
    const int fd = connect_to_socket(path, error);
    if (fd < 0) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return ControlStatus::NoDaemon;
    }

    // The request is fully formed before the connection is made, so nothing here can fail on
    // the command's own shape -- the parser already refused anything that could.
    const bool sent = write_all(fd, encode_control_request(request) + "\n", error);

    std::string reply;
    // Attempted even after a failed write: the daemon may have replied `error` and closed,
    // which is what a short write against a closed peer usually means, and that reply is a
    // better diagnostic than our own guess at what went wrong.
    const bool received = read_reply(fd, reply, error);
    ::close(fd);

    if (!sent && reply.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return ControlStatus::Failed;
    }
    if (!received) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return ControlStatus::Failed;
    }
    if (reply.empty()) {
        std::fprintf(stderr, "error: the daemon closed the connection without answering\n");
        return ControlStatus::Failed;
    }

    std::string first_line;
    std::string payload;
    split_first_line(reply, first_line, payload);

    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    if (!decode_control_reply(first_line, status, reason)) {
        // Something is listening on that path and it is not a sendspin-cli daemon. Worth
        // saying outright: the alternative is printing whatever it sent as though it were
        // a status.
        std::fprintf(stderr, "error: %s answered '%s', which is not a sendspin-cli reply\n",
                     path.c_str(), first_line.c_str());
        return ControlStatus::Failed;
    }

    if (status != ControlStatus::Ok) {
        std::fprintf(stderr, "error: %s\n", reason.c_str());
        return status;
    }
    // stdout rather than the log, because this is the answer to a question rather than a record
    // of a run -- so `sendspin-cli status | grep track` works.
    std::fwrite(payload.data(), 1, payload.size(), out);
    return ControlStatus::Ok;
}

}  // namespace sendspin_cli
