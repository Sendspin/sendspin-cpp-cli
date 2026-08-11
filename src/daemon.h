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

/// @file daemon.h
/// @brief Detaching from the terminal (-z), and the exclusive file lock -P and -z's siblings use

#pragma once

#include <string>

namespace sendspin_cli {

/// @brief How an attempt on a lock file ended.
///
/// Three outcomes rather than a bool, because "someone else is running" is the answer a
/// supervisor acts on and every other errno is one an operator has to read -- folding them
/// together is the same mistake as one catch-all message for every failure.
enum class PidFileStatus {
    Ok,              ///< the lock was free
    AlreadyRunning,  ///< another process holds the lock
    Failed,          ///< something else went wrong; `error` says what
};

/// @brief Opens `path` and takes an exclusive, non-blocking `flock()` on it.
///
/// The whole of this repo's "is another instance running" mechanism, in one place because there
/// is now more than one caller -- the -P pidfile and the control socket's sibling lock -- and
/// `README.md` states that both refusals are worded the same. Sharing the code is what makes
/// that true rather than a coincidence two files apart.
///
/// `flock()` rather than `fcntl()` record locks, for two reasons that both matter. A flock lock
/// belongs to the open file description, so two `open()` calls conflict even inside one process
/// -- which is what makes the conflict testable without forking, and a second instance's
/// collision real rather than a silently granted re-lock. And the kernel drops it when the
/// descriptor closes for any reason, including a crash, which is the whole of stale-file
/// handling. The alternatives are worse: an `O_EXCL` create has no stale handling at all, and
/// reading a pid and signalling it has a pid-reuse race that cannot be closed.
///
/// `O_TRUNC` is deliberately absent. Truncating on open would destroy a running instance's
/// content on the way to *discovering* that the file belongs to it -- the file has to survive
/// until the lock says whose it is.
///
/// @param path The file to lock. Created if it is not there yet.
/// @param what What this lock is protecting, for the "has to be on a local filesystem" message:
/// "pidfile", "control socket". Named rather than hardcoded because the two callers protect
/// different things and the advice is about the thing, not the lock.
/// @param mode The mode to create `path` with, before umask.
/// @param fd Set to the held descriptor on Ok, and left alone otherwise. The caller owns it, and
/// closing it is what releases the lock.
/// @param error Set to a human-readable reason for AlreadyRunning and Failed alike.
PidFileStatus lock_file(const std::string& path, const char* what, unsigned mode, int& fd,
                        std::string& error);

/// @brief Holds the -P pidfile under an exclusive lock for the process's whole life.
///
/// The lock, not the file, is what makes "already running" decidable, and it makes stale
/// detection free: a process that crashed has its descriptors closed by the kernel, so a
/// leftover file simply has no lock on it and is re-truncated and reused. No pid is parsed
/// and no signal is sent, which is what keeps a recycled pid from ever being mistaken for a
/// live instance.
///
/// The file is unlinked again on destruction, so every exit path -- including a failed
/// start_server() -- leaves nothing behind.
class PidFile {
public:
    PidFile() = default;
    ~PidFile();

    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    /// @brief Takes the lock on `path` and writes this process's pid into it.
    ///
    /// Under -z this must run in the child, after the fork: the lock belongs to whichever
    /// process is going to do the work.
    /// @param error Set to a human-readable reason for AlreadyRunning and Failed alike.
    PidFileStatus acquire(const std::string& path, std::string& error);

private:
    std::string path_;
    int fd_{-1};
};

/// @brief Reports whether `path` could be locked, without keeping the lock.
///
/// Exists only so -z can fail at the terminal: the parent probes, the child acquires for
/// real. That split also sidesteps flock-across-fork semantics, which Linux and the BSDs
/// document differently -- Linux says a fork-duplicated descriptor shares one lock released
/// only when every duplicate closes, while the BSD wording reads as though closing any
/// duplicate can release it. Nothing here depends on either reading.
///
/// The probe's own window is harmless in the direction that matters: an instance that starts
/// between the probe and the child's acquire costs a *missed* terminal error, never a false
/// success, because the child's acquire is the authoritative one.
/// @param error Set to a human-readable reason for AlreadyRunning and Failed alike.
PidFileStatus probe_pidfile(const std::string& path, std::string& error);

/// @brief Forks, detaches from the controlling terminal, and returns only in the child.
///
/// The parent exits 0 as soon as the fork succeeds, so the shell comes back immediately.
/// Every failure after that point is the child's, and reaches the log rather than the
/// terminal -- which is why the caller must do everything cheap and fallible first.
///
/// @param discard_stderr True when nothing worth keeping is on stderr -- no -f -- so it goes
/// to /dev/null with fd 0 and fd 1. False leaves fd 2 alone, pointing at the already-opened
/// logfile.
/// @param error Set to a human-readable reason when the return value is false.
/// @return true in the child, once it is detached. Never returns in the parent.
bool daemonize(bool discard_stderr, std::string& error);

}  // namespace sendspin_cli
