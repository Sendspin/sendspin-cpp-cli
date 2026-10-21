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

/// daemon.h on Windows: LockFileEx() in place of flock(), and no fork() for -z.

#include "daemon.h"

#include "win_security.h"

#include <io.h>
#include <process.h>
#include <windows.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace sendspin_cli {

namespace {

/// One byte far past any content, so the lock never blocks a reader of the pidfile.
OVERLAPPED lock_region() {
    OVERLAPPED region = {};
    region.OffsetHigh = 0x7FFFFFFF;
    return region;
}

HANDLE handle_of(int fd) {
    return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
}

}  // namespace

PidFileStatus lock_file(const std::string& path, const char* what, unsigned mode, int& fd,
                        std::string& error) {
    std::string reason;
    // Only "no group or other bits" carries over: it becomes an owner-only ACL.
    const int opened = open_file(path, false, (mode & 077) == 0, reason);
    if (opened < 0) {
        error = std::string("cannot open ") + what + " " + path + ": " + reason;
        return PidFileStatus::Failed;
    }

    OVERLAPPED region = lock_region();
    if (LockFileEx(handle_of(opened), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &region) != 0) {
        fd = opened;
        return PidFileStatus::Ok;
    }

    const DWORD code = GetLastError();
    ::close(opened);
    if (code == ERROR_LOCK_VIOLATION) {
        error = "another sendspin-cli is already running -- it holds the lock on " + path;
        return PidFileStatus::AlreadyRunning;
    }
    SetLastError(code);
    error = std::string("cannot lock ") + what + " " + path + ": " + last_error_text();
    return PidFileStatus::Failed;
}

void release_lock(int fd) {
    OVERLAPPED region = lock_region();
    UnlockFileEx(handle_of(fd), 0, 1, 0, &region);
    ::close(fd);
}

PidFile::~PidFile() {
    if (this->fd_ < 0) {
        return;
    }
    // Unlink while still holding the lock; open_file() shares the file for deletion.
    ::unlink(this->path_.c_str());
    release_lock(this->fd_);
}

PidFileStatus PidFile::acquire(const std::string& path, std::string& error) {
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", 0644, fd, error);
    if (locked != PidFileStatus::Ok) {
        return locked;
    }

    // Truncate only now that the lock is held, or a shorter pid leaves stale digits.
    if (_chsize_s(fd, 0) != 0) {
        error = "cannot truncate pidfile " + path + ": " + std::strerror(errno);
        release_lock(fd);
        return PidFileStatus::Failed;
    }

    const std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
    const int written = ::write(fd, pid.data(), static_cast<unsigned>(pid.size()));
    if (written != static_cast<int>(pid.size())) {
        error = "cannot write pidfile " + path + ": " +
                (written < 0 ? std::strerror(errno) : "short write");
        release_lock(fd);
        return PidFileStatus::Failed;
    }

    // Set last, so a failure above leaves the destructor nothing to unlink.
    this->path_ = path;
    this->fd_ = fd;
    return PidFileStatus::Ok;
}

PidFileStatus probe_pidfile(const std::string& path, std::string& error) {
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", 0644, fd, error);
    if (locked == PidFileStatus::Ok) {
        release_lock(fd);
    }
    return locked;
}

bool daemonize(bool /*discard_stderr*/, std::string& error) {
    // cli.cpp refuses -z on this platform, so this is only reached by a caller that skipped it.
    error = "-z needs fork(), which Windows does not have";
    return false;
}

}  // namespace sendspin_cli
