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

/// daemon.h on Windows: LockFileEx() in place of flock(), and a detached copy in place of fork().

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

/// Set in the environment of the copy daemonize() starts.
constexpr const wchar_t* DETACHED_MARKER = L"SENDSPIN_CLI_DETACHED";

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
    // The copy started below re-runs main() from the same command line; the marker says so.
    // It starts in the launch directory, so its relative paths absolutize as the parent's did.
    if (GetEnvironmentVariableW(DETACHED_MARKER, nullptr, 0) != 0) {
        SetEnvironmentVariableW(DETACHED_MARKER, nullptr);
        // This drive's root, as chdir("/") does: nothing keeps the launch directory busy, and a
        // path rooted without a drive letter still names what it did.
        if (SetCurrentDirectoryW(L"\\") == 0) {
            error = "cannot change directory to the drive root: " + last_error_text();
            return false;
        }
        return true;
    }

    wchar_t module[MAX_PATH];
    const DWORD module_length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (module_length == 0 || module_length == MAX_PATH) {
        error = "cannot find this executable to start it detached: " + last_error_text();
        return false;
    }
    // The command line as typed, so nothing is re-quoted; CreateProcessW() may write to it.
    std::wstring command_line = GetCommandLineW();

    // A hidden console of its own: no terminal, yet taskkill /PID still delivers the close event
    // main() shuts down cleanly on, as kill does with SIGTERM.
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    SetEnvironmentVariableW(DETACHED_MARKER, L"1");
    const BOOL created = CreateProcessW(module, command_line.data(), nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process);
    const DWORD reason = GetLastError();
    SetEnvironmentVariableW(DETACHED_MARKER, nullptr);
    if (created == 0) {
        SetLastError(reason);
        error = "cannot start the detached player: " + last_error_text();
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    // _exit(), as on POSIX: the copy owns everything from here.
    _exit(0);
}

}  // namespace sendspin_cli
