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

/// @file control_socket.cpp
/// @brief The daemon's half of the control channel: a listening Unix socket, pumped per tick

#include "control.h"

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

/// The mode the socket is created with, explicitly rather than by umask.
///
/// `bind()` applies the umask, and `daemonize()` sets `umask(0022)` -- so a socket bound after
/// the fork would land 0755 and be connectable by every local account, which for this socket
/// means any local user could pause playback and `switch` this endpoint out of its group.
///
/// Linux enforces socket-inode permissions on `connect()`; macOS and the BSDs historically do
/// not, which is the second reason the parent directory must be the user-private
/// `$XDG_RUNTIME_DIR` rather than anywhere shared. The mode is the belt, the directory is the
/// braces, and neither is sufficient alone.
constexpr mode_t CONTROL_SOCKET_MODE = 0600;

/// How many pending connections the kernel queues, deliberately the same as the cap we enforce
/// ourselves.
///
/// Tied to it rather than picked independently, because whichever is smaller is the real limit
/// and only one of them can *explain* itself. A backlog below the cap has the kernel answering
/// ECONNREFUSED before accept() is ever reached -- and since the listener is only drained once
/// per main-loop tick, a burst of concurrent subcommands would hit that rather than the cap, and
/// arrive at the operator as "nothing is listening" on a player that is running perfectly well.
constexpr int CONTROL_SOCKET_BACKLOG = static_cast<int>(MAX_CONTROL_CONNECTIONS);

/// The suffix of the lock file held beside the socket for the process's lifetime.
constexpr const char* CONTROL_LOCK_SUFFIX = ".lock";

/// Whether `err` is flock() reporting that someone else holds the lock.
///
/// Both spellings compared for the reason `daemon.cpp` gives: POSIX permits EWOULDBLOCK and
/// EAGAIN to differ, and this is the one distinction the whole scheme rests on.
bool is_lock_contention(int err) {
    return err == EWOULDBLOCK || err == EAGAIN;
}

/// Fills `address` with `path`, which the caller has already checked fits.
void fill_address(sockaddr_un& address, const std::string& path) {
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
}

/// One control connection: its descriptor, the line it is assembling, and when it arrived.
struct Connection {
    int fd{-1};
    LineAssembler assembler;
    /// When this connection was accepted, for the idle deadline. A peer that connects and says
    /// nothing would otherwise hold a slot for the daemon's lifetime.
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

ControlSocketStatus ControlSocket::open(const std::string& path, std::string& error) {
    if (!control_socket_path_fits(path)) {
        // Normally unreachable: the parser refuses an over-long --control-socket. Kept because
        // a silent truncation here would bind a socket at a path nothing can find, and every
        // subcommand would then report "no daemon" against a daemon that is running.
        error = "control socket path '" + path + "' does not fit a Unix socket address (" +
                std::to_string(control_socket_path_limit()) + " bytes)";
        return ControlSocketStatus::Failed;
    }

    // Taken first and held for the process's lifetime. This is what makes the two cases
    // different: a crashed daemon's lock died with its descriptor, so its leftover socket file
    // is ours to unlink; a *running* daemon still holds it, and we stop here rather than
    // unlinking a socket it is listening on.
    const std::string lock_path = path + CONTROL_LOCK_SUFFIX;
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        error = "cannot open control socket lock " + lock_path + ": " + std::strerror(errno);
        return ControlSocketStatus::Failed;
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        const int reason = errno;
        ::close(lock_fd);
        if (is_lock_contention(reason)) {
            // Worded like the -P refusal, because it is the same fact: another instance is
            // already here. And reported as its own status, because it is the one control
            // socket failure that must stop this run rather than being carried on past.
            error = "another sendspin-cli is already running -- it holds the lock on " + lock_path;
            return ControlSocketStatus::AlreadyRunning;
        }
        error = "cannot lock " + lock_path + ": " + std::strerror(reason) +
                ". A control socket has to be on a local filesystem: flock is emulated "
                "over NFS and is not dependable over SMB";
        return ControlSocketStatus::Failed;
    }

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        error = std::string("cannot create the control socket: ") + std::strerror(errno);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    // Under the lock, so this can only ever remove a socket no live daemon owns. bind() fails
    // with EADDRINUSE on an existing path whether or not anything is listening, so a stale file
    // left by a SIGKILL has to be removed rather than detected -- which is exactly what the
    // lock makes safe. ENOENT is the normal case and not a failure.
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        error = "cannot remove the stale control socket " + path + ": " + std::strerror(errno);
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    sockaddr_un address = {};
    fill_address(address, path);

    // Set around bind() rather than fixed up with chmod() afterwards, so the socket is never
    // world-connectable even briefly. fchmod() on a socket is not portable, and a chmod() on
    // the path after bind() leaves exactly that window open.
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

    // Non-blocking, because poll() runs on the main loop: an accept() that blocked would stop
    // the player pumping audio until someone connected.
    const int flags = ::fcntl(listener, F_GETFL, 0);
    if (flags < 0 || ::fcntl(listener, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = "cannot make the control socket non-blocking: " + std::string(std::strerror(errno));
        ::unlink(path.c_str());
        ::close(listener);
        ::close(lock_fd);
        return ControlSocketStatus::Failed;
    }

    // Recorded last, so every failure above leaves close() with nothing to unlink: this
    // process never owned the path.
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

    // Accepted until the kernel says there is nothing waiting, so a burst is drained in one
    // tick rather than one connection per LOOP_INTERVAL_MS.
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
            // Dropped without a reply: the cap exists to stop a local peer spending our
            // descriptors, and writing an explanation to it would be work on its terms.
            cli_log(LogLevel::WARN,
                    "refusing a control connection: %zu are already open, which is the limit",
                    this->impl_->connections.size());
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

    // Iterated by index and compacted at the end rather than erased in place, so a connection
    // that finishes mid-loop cannot invalidate the iteration.
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
                // The peer shut its write side down. Whatever is buffered is the whole request:
                // end-of-input terminates a line as unambiguously as '\n' does.
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
            // Closed with nothing to answer: a port scan, or a client that changed its mind.
            ::close(connection.fd);
            continue;
        } else if (now_ms - connection.accepted_ms >= CONTROL_IDLE_TIMEOUT_MS) {
            cli_log(LogLevel::DEBUG, "dropping a control connection that sent nothing in %lld ms",
                    static_cast<long long>(CONTROL_IDLE_TIMEOUT_MS));
            ::close(connection.fd);
            continue;
        } else {
            // Still assembling, and inside its deadline: come back next tick.
            surviving.push_back(std::move(connection));
            continue;
        }

        // One command per connection, so the reply is the last thing this descriptor carries.
        // A partial write is possible and is a real outcome rather than a theoretical one: the
        // status block is a few hundred bytes and a socket buffer is not guaranteed to take it,
        // so it is written in a loop.
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
            // SIGPIPE is ignored process-wide, so a peer that has already gone arrives here as
            // EPIPE rather than as a signal -- which is the whole reason the write path has to
            // handle it. EAGAIN on a blocking-sized reply means the peer is not reading; either
            // way the connection is done and the log line says so once.
            cli_log(LogLevel::DEBUG, "control reply write failed after %zu of %zu bytes: %s",
                    written, reply.size(), std::strerror(errno));
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
    // The lock goes last, and the socket file is unlinked while it is still held -- the same
    // order PidFile unwinds in, so a restart racing this shutdown cannot bind before the old
    // socket's name is gone.
    //
    // The lock file itself is left behind on purpose: it carries no content, and unlinking it
    // would let a second instance create and lock a fresh one while a third still holds a lock
    // on the now-nameless inode. That is the race `daemon.cpp` documents and accepts for the
    // pidfile; here it can simply be avoided.
    if (this->impl_->lock_fd >= 0) {
        ::close(this->impl_->lock_fd);
        this->impl_->lock_fd = -1;
    }
    this->impl_->lock_path.clear();
}

}  // namespace sendspin_cli
