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

/// The daemon's half of the control channel: a listening Unix socket, pumped per tick.

#include "control.h"

#include "daemon.h"
#include "log.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_CONTROL;

namespace {

/// Socket mode, forced around bind(): daemonize()'s umask alone would leave it world-connectable.
constexpr mode_t CONTROL_SOCKET_MODE = 0600;

/// Kernel backlog, equal to the connection cap so a burst hits the cap, which explains itself.
constexpr int CONTROL_SOCKET_BACKLOG = static_cast<int>(MAX_CONTROL_CONNECTIONS);

/// The suffix of the lock file held beside the socket for the process's lifetime.
constexpr const char* CONTROL_LOCK_SUFFIX = ".lock";

/// Fills `address` with `path`, which the caller has already checked fits.
void fill_address(sockaddr_un& address, const std::string& path) {
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
}

/// One control connection: its descriptor, the line it is assembling, and when it arrived.
struct Connection {
    int fd{-1};
    LineAssembler assembler;
    /// When this connection was accepted, for the idle deadline.
    int64_t accepted_ms{0};
};

}  // namespace

struct ControlSocket::Impl {
    int listener{-1};
    int lock_fd{-1};
    std::string path;
    std::string lock_path;
    std::vector<Connection> connections;
};

ControlSocket::ControlSocket() : impl_(std::make_unique<Impl>()) {}

ControlSocket::~ControlSocket() {
    this->close();
}

ControlSocketStatus probe_control_socket(const std::string& path, std::string& error) {
    if (!control_socket_path_fits(path)) {
        error = "control socket path '" + path + "' does not fit a Unix socket address (" +
                std::to_string(control_socket_path_limit() - 1) + " bytes)";
        return ControlSocketStatus::Failed;
    }

    int fd = -1;
    const PidFileStatus locked =
        lock_file(path + CONTROL_LOCK_SUFFIX, "control socket", CONTROL_SOCKET_MODE, fd, error);
    switch (locked) {
        case PidFileStatus::Ok:
            ::flock(fd, LOCK_UN);
            ::close(fd);
            return ControlSocketStatus::Ok;
        case PidFileStatus::AlreadyRunning:
            return ControlSocketStatus::AlreadyRunning;
        case PidFileStatus::Failed:
            break;
    }
    return ControlSocketStatus::Failed;
}

ControlSocketStatus ControlSocket::open(const std::string& path, std::string& error) {
    if (!control_socket_path_fits(path)) {
        // Normally refused by the parser; truncating would bind a socket nothing can find.
        error = "control socket path '" + path + "' does not fit a Unix socket address (" +
                std::to_string(control_socket_path_limit()) + " bytes)";
        return ControlSocketStatus::Failed;
    }

    // Held for the process's lifetime: a live daemon keeps it, so only a stale socket gets
    // unlinked.
    const std::string lock_path = path + CONTROL_LOCK_SUFFIX;
    int lock_fd = -1;
    switch (lock_file(lock_path, "control socket", CONTROL_SOCKET_MODE, lock_fd, error)) {
        case PidFileStatus::Ok:
            break;
        case PidFileStatus::AlreadyRunning:
            return ControlSocketStatus::AlreadyRunning;
        case PidFileStatus::Failed:
            return ControlSocketStatus::Failed;
    }

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        error = std::string("cannot create the control socket: ") + std::strerror(errno);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    // Safe under the lock: bind() fails on any existing path, so a stale file must be removed.
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        error = "cannot remove the stale control socket " + path + ": " + std::strerror(errno);
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    sockaddr_un address = {};
    fill_address(address, path);

    // umask around bind(), so the socket is never world-connectable even briefly.
    const mode_t previous_umask = ::umask(0777 & ~CONTROL_SOCKET_MODE);
    const bool bound =
        ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    const int bind_errno = errno;
    ::umask(previous_umask);

    if (!bound) {
        error = "cannot bind the control socket " + path + ": " + std::strerror(bind_errno);
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    if (::listen(listener, CONTROL_SOCKET_BACKLOG) != 0) {
        error = "cannot listen on the control socket " + path + ": " + std::strerror(errno);
        ::unlink(path.c_str());
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    // Non-blocking: poll() runs on the main loop.
    const int flags = ::fcntl(listener, F_GETFL, 0);
    if (flags < 0 || ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = "cannot make the control socket non-blocking: " + std::string(std::strerror(errno));
        ::unlink(path.c_str());
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    // Set last, so a failure above leaves close() nothing to unlink.
    this->impl_->listener = listener;
    this->impl_->lock_fd = lock_fd;
    this->impl_->path = path;
    this->impl_->lock_path = lock_path;
    return ControlSocketStatus::Ok;
}

void ControlSocket::poll(int64_t now_ms, ControlHandler& handler) {
    if (this->impl_->listener < 0) {
        return;
    }

    // Drain every waiting connection this tick.
    while (true) {
        const int fd = ::accept(this->impl_->listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                cli_log(LogLevel::DEBUG, "accept on %s failed: %s", this->impl_->path.c_str(),
                        std::strerror(errno));
            }
            break;
        }
        if (this->impl_->connections.size() >= MAX_CONTROL_CONNECTIONS) {
            // Tell the peer why before closing; debug, so a connect loop cannot flood the log.
            cli_log(LogLevel::DEBUG,
                    "refusing a control connection: %zu are already open, which is the limit",
                    this->impl_->connections.size());
            const std::string busy = encode_control_reply(
                ControlStatus::Failed,
                "this player already has " + std::to_string(MAX_CONTROL_CONNECTIONS) +
                    " control connections open, which is as many as it will take -- try again",
                "");
            // Best-effort: the peer is about to be closed anyway.
            const ssize_t ignored = ::write(fd, busy.data(), busy.size());
            static_cast<void>(ignored);
            ::close(fd);
            continue;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            cli_log(LogLevel::DEBUG, "cannot make a control connection non-blocking: %s",
                    std::strerror(errno));
            ::close(fd);
            continue;
        }
        Connection connection;
        connection.fd = fd;
        connection.accepted_ms = now_ms;
        this->impl_->connections.push_back(std::move(connection));
    }

    // Collect survivors into a new vector so the handler cannot invalidate the iteration.
    std::vector<Connection> surviving;
    surviving.reserve(this->impl_->connections.size());

    for (Connection& connection : this->impl_->connections) {
        char buffer[MAX_CONTROL_LINE_BYTES];
        LineState state = LineState::Incomplete;
        bool closed = false;

        while (state == LineState::Incomplete && !closed) {
            const ssize_t read_bytes = ::read(connection.fd, buffer, sizeof(buffer));
            if (read_bytes > 0) {
                state = connection.assembler.feed(buffer, static_cast<size_t>(read_bytes));
                continue;
            }
            if (read_bytes == 0) {
                // EOF ends the request as unambiguously as '\n'.
                closed = true;
                state = connection.assembler.finish();
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            cli_log(LogLevel::DEBUG, "control connection read failed: %s", std::strerror(errno));
            closed = true;
            break;
        }

        std::string reply;
        if (state == LineState::Ready) {
            reply = handler.handle_control_request(connection.assembler.line());
        } else if (state == LineState::TooLong || state == LineState::Invalid) {
            reply = encode_control_reply(ControlStatus::Usage, line_state_reason(state), "");
        } else if (closed) {
            ::close(connection.fd);
            continue;
        } else if (now_ms - connection.accepted_ms >= CONTROL_IDLE_TIMEOUT_MS) {
            cli_log(LogLevel::DEBUG, "dropping a control connection that sent nothing in %lld ms",
                    static_cast<long long>(CONTROL_IDLE_TIMEOUT_MS));
            ::close(connection.fd);
            continue;
        } else {
            surviving.push_back(std::move(connection));
            continue;
        }

        // One command per connection; the reply may need several writes.
        size_t written = 0;
        while (written < reply.size()) {
            const ssize_t wrote =
                ::write(connection.fd, reply.data() + written, reply.size() - written);
            if (wrote > 0) {
                written += static_cast<size_t>(wrote);
                continue;
            }
            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            // SIGPIPE is ignored process-wide, so a vanished peer shows up here as EPIPE.
            cli_log(LogLevel::DEBUG, "control reply write failed after %zu of %zu bytes: %s",
                    written, reply.size(), wrote < 0 ? std::strerror(errno) : "wrote nothing");
            break;
        }
        ::close(connection.fd);
    }

    this->impl_->connections = std::move(surviving);
}

void ControlSocket::close() {
    for (Connection& connection : this->impl_->connections) {
        ::close(connection.fd);
    }
    this->impl_->connections.clear();

    if (this->impl_->listener >= 0) {
        ::close(this->impl_->listener);
        this->impl_->listener = -1;
    }
    if (!this->impl_->path.empty()) {
        ::unlink(this->impl_->path.c_str());
        this->impl_->path.clear();
    }
    // Unlink under the lock; keep the lock file, since removing it reopens the stale-inode race.
    if (this->impl_->lock_fd >= 0) {
        ::close(this->impl_->lock_fd);
        this->impl_->lock_fd = -1;
    }
    this->impl_->lock_path.clear();
}

}  // namespace sendspin_cli
