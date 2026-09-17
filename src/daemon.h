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

/// Detaching from the terminal (-z), and the exclusive file lock -P and the control socket use.

#pragma once

#include <string>

namespace sendspin_cli {

/// How an attempt on a lock file ended.
enum class PidFileStatus {
    Ok,              ///< the lock was free
    AlreadyRunning,  ///< another process holds the lock
    Failed,          ///< something else went wrong; `error` says what
};

/// Opens `path` and takes an exclusive, non-blocking `flock()` on it, creating it if missing.
/// Never truncates: the file must survive until the lock says whose it is.
/// @param what What the lock protects, e.g. "pidfile", for diagnostics.
/// @param mode Creation mode, before umask.
/// @param fd Set to the held descriptor on Ok; closing it releases the lock.
PidFileStatus lock_file(const std::string& path, const char* what, unsigned mode, int& fd,
                        std::string& error);

/// Holds the -P pidfile locked for the process's life, and unlinks it on destruction.
class PidFile {
public:
    PidFile() = default;
    ~PidFile();

    PidFile(const PidFile&) = delete;
    PidFile& operator=(const PidFile&) = delete;

    /// Takes the lock on `path` and writes our pid into it. Under -z, call in the child.
    PidFileStatus acquire(const std::string& path, std::string& error);

private:
    std::string path_;
    int fd_{-1};
};

/// Whether `path` could be locked, without keeping it, so -z can fail at the terminal.
PidFileStatus probe_pidfile(const std::string& path, std::string& error);

/// Forks and detaches; the parent exits 0, so do everything fallible before calling.
/// @param discard_stderr Send fd 2 to /dev/null too; false leaves it on the logfile.
/// @return true in the child, once detached. Never returns in the parent.
bool daemonize(bool discard_stderr, std::string& error);

}  // namespace sendspin_cli
