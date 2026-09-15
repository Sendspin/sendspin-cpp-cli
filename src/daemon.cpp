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

#include "daemon.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace sendspin_cli {

namespace {

/// Pidfile mode before umask: world-readable, owner-writable.
constexpr mode_t PIDFILE_MODE = 0644;

}  // namespace

PidFileStatus lock_file(const std::string& path, const char* what, unsigned mode, int& fd,
                        std::string& error) {
    const int opened =
        ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, static_cast<mode_t>(mode));
    if (opened < 0) {
        error = std::string("cannot open ") + what + " " + path + ": " + std::strerror(errno);
        return PidFileStatus::Failed;
    }

    if (::flock(opened, LOCK_EX | LOCK_NB) == 0) {
        fd = opened;
        return PidFileStatus::Ok;
    }

    const int reason = errno;
    ::close(opened);
    // POSIX lets EWOULDBLOCK and EAGAIN differ, so check both.
    if (reason == EWOULDBLOCK || reason == EAGAIN) {
        error = "another sendspin-cli is already running -- it holds the lock on " + path;
        return PidFileStatus::AlreadyRunning;
    }
    error = std::string("cannot lock ") + what + " " + path + ": " + std::strerror(reason) +
            ". A " + what +
            " has to be on a local filesystem: flock is emulated over NFS and is not dependable "
            "over SMB";
    return PidFileStatus::Failed;
}

PidFile::~PidFile() {
    if (this->fd_ < 0) {
        return;
    }
    // Unlink while still holding the lock.
    ::unlink(this->path_.c_str());
    ::close(this->fd_);
}

PidFileStatus PidFile::acquire(const std::string& path, std::string& error) {
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", PIDFILE_MODE, fd, error);
    if (locked != PidFileStatus::Ok) {
        return locked;
    }

    // Truncate only now that the lock is held, or a shorter pid leaves stale digits.
    if (::ftruncate(fd, 0) != 0) {
        error = "cannot truncate pidfile " + path + ": " + std::strerror(errno);
        ::close(fd);
        return PidFileStatus::Failed;
    }

    const std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
    // Raw write(): a FILE*'s fclose() would close the descriptor and drop the lock.
    const ssize_t written = ::write(fd, pid.data(), pid.size());
    if (written != static_cast<ssize_t>(pid.size())) {
        error = "cannot write pidfile " + path + ": " +
                (written < 0 ? std::strerror(errno) : "short write");
        ::close(fd);
        return PidFileStatus::Failed;
    }

    // Set last, so a failure above leaves the destructor nothing to unlink.
    this->path_ = path;
    this->fd_ = fd;
    return PidFileStatus::Ok;
}

PidFileStatus probe_pidfile(const std::string& path, std::string& error) {
    // Created, not just opened, so -P in a missing directory fails at the terminal.
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", PIDFILE_MODE, fd, error);
    if (locked == PidFileStatus::Ok) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
    return locked;
}

bool daemonize(bool discard_stderr, std::string& error) {
    // Must precede any device, socket, thread or mDNS handle: only the forking thread survives.
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (child > 0) {
        // _exit(), not exit(): stdio buffers and atexit handlers are shared with the child.
        ::_exit(0);
    }

    // One fork is enough: this daemon never opens a tty. daemon(3) is deprecated on macOS.
    if (::setsid() == static_cast<pid_t>(-1)) {
        error = std::string("cannot start a new session: ") + std::strerror(errno);
        return false;
    }

    if (::chdir("/") != 0) {
        error = std::string("cannot change directory to /: ") + std::strerror(errno);
        return false;
    }

    // No group- or world-writable files from here on.
    ::umask(S_IWGRP | S_IWOTH);

    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        error = std::string("cannot open /dev/null: ") + std::strerror(errno);
        return false;
    }
    // fd 2 stays on the -f logfile when there is one.
    const bool redirected = ::dup2(null_fd, STDIN_FILENO) >= 0 &&
                            ::dup2(null_fd, STDOUT_FILENO) >= 0 &&
                            (!discard_stderr || ::dup2(null_fd, STDERR_FILENO) >= 0);
    // Read errno before close(), which may overwrite it.
    const int reason = errno;
    if (null_fd > STDERR_FILENO) {
        ::close(null_fd);
    }
    if (!redirected) {
        error = std::string("cannot redirect the standard streams: ") + std::strerror(reason);
        return false;
    }
    return true;
}

}  // namespace sendspin_cli
