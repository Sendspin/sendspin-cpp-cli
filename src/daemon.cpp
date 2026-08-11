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

/// The mode a pidfile is created with, before umask: readable by anything that wants to know
/// which process to signal, writable only by whoever started the daemon.
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
    // Both spellings are compared even though EWOULDBLOCK and EAGAIN are the same value on
    // Linux and macOS alike: POSIX permits them to differ, and this is the one distinction the
    // whole scheme rests on.
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
    // Unlinked before the descriptor closes, so the lock is still held while the name goes
    // away. One race is left and is accepted rather than fixed: this process can unlink the
    // file while a second instance holds a lock on the now-nameless inode, after which a
    // third creates and locks a fresh file and two instances run. Every pidfile this simple
    // has it, the mitigations are worse than the disease, and no supervisor starts two
    // instances at once.
    ::unlink(this->path_.c_str());
    ::close(this->fd_);
}

PidFileStatus PidFile::acquire(const std::string& path, std::string& error) {
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", PIDFILE_MODE, fd, error);
    if (locked != PidFileStatus::Ok) {
        return locked;
    }

    // Only now, with the lock held, is the old content ours to drop -- and it must be
    // dropped: writing "99\n" over a leftover "123456\n" without this leaves "99\n56\n", and
    // a supervisor reads a pid that never existed.
    if (::ftruncate(fd, 0) != 0) {
        error = "cannot truncate pidfile " + path + ": " + std::strerror(errno);
        ::close(fd);
        return PidFileStatus::Failed;
    }

    const std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
    // write() rather than a FILE* for the convenience of fprintf: fclose() closes the
    // descriptor and drops the lock with it, so the wrapper would cost the only thing this
    // file is for.
    const ssize_t written = ::write(fd, pid.data(), pid.size());
    if (written != static_cast<ssize_t>(pid.size())) {
        error = "cannot write pidfile " + path + ": " +
                (written < 0 ? std::strerror(errno) : "short write");
        ::close(fd);
        return PidFileStatus::Failed;
    }

    // Recorded last, so a failure above leaves the destructor with nothing to unlink -- this
    // process never owned the file.
    this->path_ = path;
    this->fd_ = fd;
    return PidFileStatus::Ok;
}

PidFileStatus probe_pidfile(const std::string& path, std::string& error) {
    // Created rather than merely opened, so the probe also proves the file *can* be made:
    // -P in a directory that does not exist then fails at the terminal instead of in a log
    // the operator has not thought to look at yet.
    //
    // Two consequences of creating it here, both accepted. The file's mode comes from the
    // umask this process was started with, since daemonize()'s own umask runs later and the
    // child then opens a file that already exists. And a pre-fork failure after this point --
    // an unopenable -f, a fork() that fails -- leaves an empty, unlocked file behind, which by
    // the lock's own rules reads correctly as "nothing is running" and is reused on the next
    // start.
    int fd = -1;
    const PidFileStatus locked = lock_file(path, "pidfile", PIDFILE_MODE, fd, error);
    if (locked == PidFileStatus::Ok) {
        // Released explicitly rather than leaning on close(), so the intent is on the page:
        // this is a look, and the child after the fork owns the real lock.
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
    return locked;
}

bool daemonize(bool discard_stderr, std::string& error) {
    // THE INVARIANT, and the reason this call sits where it does: fork() must happen before
    // this process acquires *any* resource -- a device, a socket, a thread, a connection to
    // another daemon. Only the forking thread survives a fork, so anything opened above this
    // line leaves the child holding a half-dead copy of it. Concretely, and all three are
    // live in this binary: make_audio_sink() probes the device and PortAudioSink then holds a
    // PortAudioGuard, which on macOS brings up the CoreAudio HAL's mach ports and helper
    // threads; start_server() starts the sync task's std::thread; and a DNSServiceRef is a
    // per-process connection to mDNSResponder or avahi-daemon that an inherited copy cannot
    // use. Only work that is cheap, deterministic and resource-free belongs above the fork --
    // which is exactly what lets an unopenable -f and a locked -P still fail at the terminal.
    const pid_t child = ::fork();
    if (child < 0) {
        error = std::string("cannot fork: ") + std::strerror(errno);
        return false;
    }
    if (child > 0) {
        // _exit() rather than exit(): the parent shares every buffered stream and every
        // atexit handler with the child, so unwinding here would flush and run them twice.
        ::_exit(0);
    }

    // One fork is enough. setsid() already leaves the child a session leader with no
    // controlling terminal; the classic second fork exists only to stop a session leader
    // re-acquiring one by opening a tty, which this daemon never does.
    //
    // daemon(3) would do the fork, the setsid, the chdir and the stream redirection in one
    // call, and is deliberately not used: it is deprecated on macOS, so it cannot compile
    // clean under the -Wall -Wextra -Wpedantic this repo builds with, and it gives no way to
    // keep fd 2 on an already-opened logfile.
    if (::setsid() == static_cast<pid_t>(-1)) {
        error = std::string("cannot start a new session: ") + std::strerror(errno);
        return false;
    }

    // Nothing here is a relative path once the options are parsed, and a daemon sitting in
    // the directory it was started from pins whatever that is mounted on.
    if (::chdir("/") != 0) {
        error = std::string("cannot change directory to /: ") + std::strerror(errno);
        return false;
    }

    // 0022 rather than whatever the shell had, so nothing this daemon goes on to create is
    // group- or world-writable. Deliberately not a claim about the pidfile's own mode: under
    // -z the parent's probe has usually created that file already, so its mode came from the
    // umask in force before this line ran.
    ::umask(S_IWGRP | S_IWOTH);

    // O_RDWR so a read on fd 0 and a write on fd 1 are both legal, rather than some
    // component meeting EBADF at an unpredictable moment.
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        error = std::string("cannot open /dev/null: ") + std::strerror(errno);
        return false;
    }
    // fd 2 is left alone when -f already pointed it at a logfile. That is the whole reason
    // the logfile is opened above the fork; without -f there is nothing to keep, and leaving
    // it on the terminal would both scribble on a shell that has moved on and leave the
    // daemon writing to a pty that can go away.
    const bool redirected = ::dup2(null_fd, STDIN_FILENO) >= 0 &&
                            ::dup2(null_fd, STDOUT_FILENO) >= 0 &&
                            (!discard_stderr || ::dup2(null_fd, STDERR_FILENO) >= 0);
    // Read before the close(): a call that succeeds is still allowed to set errno, so closing
    // first would let a clean close overwrite the reason dup2() failed.
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
