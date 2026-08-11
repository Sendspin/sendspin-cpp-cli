#!/usr/bin/env bash
#
# Copyright 2026 sendspin-cpp-cli Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Boot-level checks for a built sendspin-cli: that it runs, comes up on its port, forks under
# -z, refuses a second instance holding the same -P, survives an mDNS daemon it cannot reach,
# exits 0 on SIGTERM, and answers its control socket.
#
# Deliberately outside the CTest suite rather than an addition to it. Nothing in tests/ opens
# a device, a socket or the mDNS daemon, which is what keeps `ctest --test-dir build` runnable
# on any bare machine; everything here needs a real process -- a fork, a file lock held across
# two of them, a listening port, a signal -- and registering it as a test would erode that
# boundary rather than respect it. This checks that the binary *boots*; what it does once a
# server talks to it is the unit suite's subject, not this one's.
#
# Usage: scripts/smoke_test.sh [path-to-sendspin-cli]

set -euo pipefail

BIN="${1:-build/sendspin-cli}"
readonly BIN

# High enough to be clear of anything a developer is plausibly running. One per phase for the
# checks that run concurrently with each other, so a socket still in TIME_WAIT from the phase
# before cannot fail the next one; the control-socket checks share PORT_CONTROL because they run
# strictly in sequence and each waits for its own player to exit.
readonly PORT_FOREGROUND=39281
readonly PORT_DAEMON=39282
readonly PORT_DAEMON_SECOND=39283
readonly PORT_MDNS=39284
readonly PORT_CONTROL=39285
readonly PORT_CONTROL_SECOND=39286

readonly MDNS_INSTANCE="sendspin-cli-smoke"

# Sized for a loaded shared CI runner rather than for a developer's laptop: the cost of being
# generous is paid only when something is already wrong, and every wait below returns as soon
# as its condition holds.
readonly BOOT_TIMEOUT_S=20
readonly EXIT_TIMEOUT_S=15

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR

# A second, deliberately *short* directory, used only as $XDG_RUNTIME_DIR for the control-socket
# checks. A Unix socket address holds 104 bytes on macOS, and macOS's own `mktemp -d` already
# spends 63 of them -- so the default leaf (`sendspin-cli-<port>.sock`, 23 more) would leave the
# check measuring the path limit rather than the feature. Created by mktemp, so it is 0700 and
# owned by this user, which is the property the daemon expects of $XDG_RUNTIME_DIR.
CONTROL_DIR="$(mktemp -d /tmp/sscli-smoke.XXXXXX)"
readonly CONTROL_DIR

# Every check stops what it started on the way through, but fail() exits from wherever it is
# called -- so the pids are tracked and swept here as well, to leave no player holding a port
# after a failure.
STARTED_PIDS=()

cleanup() {
    local pid
    for pid in ${STARTED_PIDS[@]+"${STARTED_PIDS[@]}"}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK_DIR" "$CONTROL_DIR"
}
trap cleanup EXIT

fail() {
    printf 'smoke: FAIL: %s\n' "$*" >&2
    exit 1
}

pass() {
    printf 'smoke: ok -- %s\n' "$*"
}

# ==============================================================================
# Waiting
# ==============================================================================
#
# Polling loops rather than timeout(1), which is coreutils and so is absent on macOS. They are
# also what a fixed `sleep N` cannot be: correct on a runner slow enough to need the whole
# deadline, and immediate on one that is not.

# Waits until `pattern` (an ERE) appears in `file`, or `limit` seconds pass.
wait_for_line() {
    local file=$1 pattern=$2 limit=$3
    local waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        if [ -f "$file" ] && grep -Eq -e "$pattern" "$file"; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Waits until `file` exists *and is not empty*, or `limit` seconds pass.
#
# Non-empty rather than merely present, because the pidfile is created, then locked, then
# written: testing for existence alone would race the write and read back nothing.
wait_for_nonempty_file() {
    local file=$1 limit=$2
    local waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        if [ -s "$file" ]; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Waits until `path` is a socket, or `limit` seconds pass.
wait_for_socket() {
    local path=$1 limit=$2
    local waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        if [ -S "$path" ]; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Waits until `path` does not exist, or `limit` seconds pass.
wait_for_absent() {
    local path=$1 limit=$2
    local waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        if [ ! -e "$path" ]; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Waits for a pid to disappear, or `limit` seconds pass. Used for the daemon, which forked
# away from this shell and so is nothing this script can wait(2) on.
wait_for_gone() {
    local pid=$1 limit=$2
    local waited=0
    while [ "$waited" -lt "$((limit * 10))" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    return 1
}

# Reaps a child of this shell and returns its exit status. One that outlives the deadline is
# killed and reported as 124 -- the status timeout(1) would have used -- so a player that
# ignores SIGTERM fails the run instead of hanging it.
await_child() {
    local pid=$1 limit=$2
    if ! wait_for_gone "$pid" "$limit"; then
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 124
    fi
    local status=0
    wait "$pid" || status=$?
    return "$status"
}

# Whether this host has a daemon for the player to register with. mDNSResponder is always up
# on macOS. The socket is an exact proxy on Linux rather than an approximate one because
# libavahi-compat-libdnssd is the only dns_sd this build has there, and that socket is what it
# connects to; a daemon is present only if the image or the workflow put it there.
mdns_daemon_present() {
    if [ "$(uname -s)" = "Darwin" ]; then
        return 0
    fi
    [ -S /run/avahi-daemon/socket ] || [ -S /var/run/avahi-daemon/socket ]
}

# ==============================================================================
# Checks
# ==============================================================================

check_version_and_help() {
    local out
    out="$("$BIN" --version)" || fail "--version exited $?"
    printf '%s' "$out" | grep -q 'sendspin-cli' ||
        fail "--version exited 0 but named nothing: $out"
    pass "--version exits 0 and identifies the binary"

    "$BIN" --help >/dev/null || fail "--help exited $?"
    pass "--help exits 0"
}

check_foreground_signal() {
    local log="$WORK_DIR/foreground.log"
    "$BIN" --no-mdns --no-control -o null --port "$PORT_FOREGROUND" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_FOREGROUND" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status (124 means it never exited). Log: $(cat "$log")"
    pass "a foreground run reaches its ready log and exits 0 on SIGTERM"
}

check_daemon_pidfile() {
    local pidfile="$WORK_DIR/daemon.pid"
    local log="$WORK_DIR/daemon.log"
    local second_err="$WORK_DIR/second-instance.err"

    # -z returns in the *parent* as soon as the child is forked, so this exit status says only
    # that the fork happened. What says the daemon is really up is the pidfile and the log.
    "$BIN" -z -P "$pidfile" -f "$log" --no-mdns --no-control -o null --port "$PORT_DAEMON" ||
        fail "-z exited $? instead of forking"

    wait_for_nonempty_file "$pidfile" "$BOOT_TIMEOUT_S" ||
        fail "no pidfile at $pidfile within ${BOOT_TIMEOUT_S}s"

    # Read now and kept: the daemon unlinks its pidfile on the way out, so reading it after the
    # signal would find nothing to signal.
    local pid
    pid="$(cat "$pidfile")"
    STARTED_PIDS+=("$pid")

    kill -0 "$pid" 2>/dev/null || fail "pidfile names pid $pid, which is not running"
    wait_for_line "$log" "listening on port $PORT_DAEMON" "$BOOT_TIMEOUT_S" ||
        fail "daemon never logged that it was listening. Log: $(cat "$log")"
    pass "-z forks and -P writes a pidfile holding a live pid"

    # A different --port on purpose, so what refuses the second instance is provably the lock
    # on the pidfile and not the listening socket.
    if "$BIN" -z -P "$pidfile" --no-mdns --no-control -o null --port "$PORT_DAEMON_SECOND" \
        >/dev/null 2>"$second_err"; then
        fail "a second instance on the same -P was allowed to start"
    fi
    grep -q 'already running' "$second_err" ||
        fail "the second instance was refused without saying why: $(cat "$second_err")"
    pass "a second instance on the same -P is refused"

    kill -TERM "$pid"
    wait_for_gone "$pid" "$EXIT_TIMEOUT_S" ||
        fail "daemon $pid outlived SIGTERM by ${EXIT_TIMEOUT_S}s"
    grep -q 'Shutting down' "$log" ||
        fail "the daemon went away without logging a shutdown. Log: $(cat "$log")"
    [ ! -e "$pidfile" ] || fail "the daemon exited but left $pidfile behind"
    pass "SIGTERM shuts the daemon down cleanly and clears its pidfile"
}

check_default_mdns_boot() {
    local log="$WORK_DIR/mdns.log"
    "$BIN" -o null --no-control --port "$PORT_MDNS" --mdns-name "$MDNS_INSTANCE" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_MDNS" "$BOOT_TIMEOUT_S" ||
        fail "no ready log from the default configuration within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    # Which of the three outcomes is the correct one is a property of the build *and* of the
    # host, and both are read rather than assumed. Asserting only the branch this happens to
    # take would mean the check quietly starts testing another one the day a runner image gains
    # -- or loses -- an mDNS daemon, or the day it is pointed at a -DSENDSPIN_CLI_WITH_MDNS=OFF
    # build, which is a configuration CI really does produce.
    if grep -q 'mDNS: none' "$log"; then
        wait_for_line "$log" 'no mDNS support' "$BOOT_TIMEOUT_S" ||
            fail "this build has no mDNS and did not say so. Log: $(cat "$log")"
        pass "a build without mDNS says it cannot be discovered and names the alternatives"
    elif mdns_daemon_present; then
        wait_for_line "$log" 'advertising _sendspin\._tcp' "$BOOT_TIMEOUT_S" ||
            fail "an mDNS daemon is running but nothing was advertised. Log: $(cat "$log")"
        pass "the default configuration advertises where an mDNS daemon is running"
    else
        wait_for_line "$log" '^W mdns: .*retrying' "$BOOT_TIMEOUT_S" ||
            fail "no mDNS daemon is running, and no retry warning was logged. Log: $(cat "$log")"
        pass "the default configuration warns and retries where no mDNS daemon is running"
    fi

    # The whole point of that warning: an advertisement that cannot be made is not fatal, so
    # the player must still be serving its port.
    kill -0 "$pid" 2>/dev/null ||
        fail "the player exited rather than carrying on without an advertisement. Log: $(cat "$log")"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status (124 means it never exited). Log: $(cat "$log")"
    pass "the default configuration boots and exits 0 on SIGTERM"
}

# The control socket at its default path: created, private, answering, and gone again.
#
# Everything here needs two processes -- a listening socket in one and a connect() from another --
# which is exactly what tests/ does not do, and why this lives in the smoke test.
check_control_socket() {
    local log="$WORK_DIR/control.log"
    local socket="$CONTROL_DIR/sendspin-cli-$PORT_CONTROL.sock"
    local status_out="$WORK_DIR/control-status.out"
    local second_err="$WORK_DIR/control-second.err"

    # $XDG_RUNTIME_DIR is what the default path is built from, and it is deliberately the only
    # thing that is: there is no /tmp fallback, since a world-writable directory would let any
    # local account drive the player.
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null --port "$PORT_CONTROL" \
        >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONTROL" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    # 0600 explicitly, because bind() applies the umask and a daemon's is 0022 -- so getting this
    # wrong leaves the socket connectable by every local account rather than merely untidy.
    local mode
    case "$(uname -s)" in
        Darwin) mode="$(stat -f '%Lp' "$socket")" ;;
        *) mode="$(stat -c '%a' "$socket")" ;;
    esac
    [ "$mode" = "600" ] || fail "the control socket is mode $mode, not 600"
    pass "the control socket appears at its default path, mode 0600"

    # A `status` against a player with no server connection: it has to answer, say it is not
    # connected, and still report what it knows locally.
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_CONTROL" >"$status_out" 2>&1 ||
        fail "status exited $? against a running player. Output: $(cat "$status_out")"
    grep -q '^name: ' "$status_out" || fail "status printed no name: $(cat "$status_out")"
    grep -q '^server: not connected' "$status_out" ||
        fail "status did not report the missing server connection: $(cat "$status_out")"
    # Group and player volume are separate, labelled lines -- `vol` moves the group, so one
    # ambiguous `volume:` would leave a reader unable to tell which number it had moved.
    grep -q '^group volume: ' "$status_out" ||
        fail "status printed no group volume line: $(cat "$status_out")"
    grep -q '^player volume: ' "$status_out" ||
        fail "status printed no player volume line: $(cat "$status_out")"
    pass "status round-trips over the control socket and names both volumes"

    # A transport command with no server behind it must say *that*, and not "unsupported":
    # a dropped connection empties supported_commands, so the naive answer is the wrong one.
    local pause_err="$WORK_DIR/control-pause.err"
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" pause --port "$PORT_CONTROL" \
        >/dev/null 2>"$pause_err"; then
        fail "pause reported success with no server connected"
    fi
    grep -q 'not connected' "$pause_err" ||
        fail "pause blamed something other than the missing connection: $(cat "$pause_err")"
    pass "a transport command with no server reports the connection, not the command"

    # A second instance on the same socket is refused, in the same words -P uses -- and refused
    # before it opens a device or a port, which is why the lock is taken where it is.
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null \
        --port "$PORT_CONTROL_SECOND" --control-socket "$socket" \
        >/dev/null 2>"$second_err"; then
        fail "a second instance on the same control socket was allowed to start"
    fi
    grep -q 'already running' "$second_err" ||
        fail "the second instance was refused without saying why: $(cat "$second_err")"
    pass "a second instance on the same control socket is refused"

    # And refused at the *terminal* under -z, which is the case that needs the pre-fork probe:
    # ControlSocket::open() runs in the child, so without one the refusal would land in a log the
    # shell has stopped watching -- and with no -f, nowhere at all. -P is probed pre-fork for
    # exactly this reason, and README.md claims parity with it.
    local second_z_err="$WORK_DIR/control-second-z.err"
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" -z --no-mdns -o null \
        --port "$PORT_CONTROL_SECOND" --control-socket "$socket" \
        >/dev/null 2>"$second_z_err"; then
        fail "a second instance under -z on the same control socket was allowed to fork"
    fi
    grep -q 'already running' "$second_z_err" ||
        fail "the -z second instance said nothing at the terminal: $(cat "$second_z_err")"
    pass "a second instance under -z is refused at the terminal, not only in the log"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status (124 means it never exited). Log: $(cat "$log")"
    wait_for_absent "$socket" "$EXIT_TIMEOUT_S" ||
        fail "the player exited but left $socket behind"
    pass "the control socket is removed on SIGTERM"
}

# A socket file left by a SIGKILLed daemon needs no manual cleanup.
#
# The case the sibling flock() exists for: unlink-then-bind alone would race a *live* daemon's
# socket away, and connecting to probe is a TOCTOU -- so the lock is what makes "stale" and "in
# use" different answers rather than a guess.
check_stale_control_socket() {
    local socket="$CONTROL_DIR/stale.sock"
    local first="$WORK_DIR/stale-first.log"
    local second="$WORK_DIR/stale-second.log"

    "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --control-socket "$socket" >"$first" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$first")"

    # SIGKILL, so no cleanup runs: the file is left behind and the lock dies with the descriptor.
    # Reaped straight away rather than through wait_for_gone(), so bash does not print its own
    # "Killed: 9" job notice into the middle of the run's output.
    kill -KILL "$pid"
    wait "$pid" 2>/dev/null || true
    [ -e "$socket" ] || fail "expected a stale socket file at $socket after SIGKILL"

    "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --control-socket "$socket" >"$second" 2>&1 &
    local restarted=$!
    STARTED_PIDS+=("$restarted")
    wait_for_line "$second" "control: Listening on $socket" "$BOOT_TIMEOUT_S" ||
        fail "the restart did not take over the stale socket. Log: $(cat "$second")"
    "$BIN" status --control-socket "$socket" >/dev/null 2>&1 ||
        fail "the taken-over socket does not answer"
    pass "a stale control socket left by SIGKILL is taken over on the next start"

    kill -TERM "$restarted"
    await_child "$restarted" "$EXIT_TIMEOUT_S" ||
        fail "the restarted player did not exit on SIGTERM. Log: $(cat "$second")"
}

# --no-control binds nothing at all, and says which flag decided that.
check_no_control() {
    local log="$WORK_DIR/no-control.log"
    local socket="$CONTROL_DIR/sendspin-cli-$PORT_CONTROL.sock"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --no-control \
        >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONTROL" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"
    [ ! -e "$socket" ] || fail "--no-control still created $socket"
    grep -q 'control: Not listening' "$log" ||
        fail "--no-control did not say it was not listening. Log: $(cat "$log")"
    pass "--no-control binds no socket and names the flag"

    kill -TERM "$pid"
    await_child "$pid" "$EXIT_TIMEOUT_S" ||
        fail "SIGTERM did not stop the --no-control run. Log: $(cat "$log")"
}

# With no $XDG_RUNTIME_DIR and no --control-socket there is no default path -- and the player
# still serves audio, which is the whole point of the warning rather than a refusal.
check_missing_runtime_dir() {
    local log="$WORK_DIR/no-runtime-dir.log"

    env -u XDG_RUNTIME_DIR "$BIN" --no-mdns -o null --port "$PORT_CONTROL" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONTROL" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"
    wait_for_line "$log" '^W control: .*XDG_RUNTIME_DIR' "$BOOT_TIMEOUT_S" ||
        fail "no warning about the missing runtime directory. Log: $(cat "$log")"
    # There is deliberately no /tmp fallback: it is world-writable, and a socket there would let
    # any local account pause playback and switch this endpoint out of its group.
    if grep -q '/tmp' "$log"; then
        fail "the missing-runtime-dir path mentions /tmp. Log: $(cat "$log")"
    fi
    kill -0 "$pid" 2>/dev/null ||
        fail "the player exited rather than carrying on without a control socket"
    pass "a missing \$XDG_RUNTIME_DIR warns once and keeps serving audio"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status. Log: $(cat "$log")"
}

# A subcommand with no player to talk to fails distinctly, and without becoming one.
check_subcommand_without_a_player() {
    local err="$WORK_DIR/no-daemon.err"
    local socket="$CONTROL_DIR/absent.sock"

    if "$BIN" status --control-socket "$socket" >/dev/null 2>"$err"; then
        fail "status succeeded against a socket nothing is listening on"
    fi
    # 3 rather than 1: a script has to be able to tell "no player" from a bad command line.
    local status=0
    "$BIN" status --control-socket "$socket" >/dev/null 2>&1 || status=$?
    [ "$status" -eq 3 ] || fail "expected exit 3 for a missing player, got $status"
    grep -q 'no sendspin-cli is listening' "$err" ||
        fail "the missing player was not named as such: $(cat "$err")"
    [ ! -e "$socket" ] || fail "a subcommand created $socket instead of only connecting to it"
    pass "a subcommand with no player exits 3 and starts nothing"
}

main() {
    [ -x "$BIN" ] ||
        fail "no executable at '$BIN' -- pass the path to sendspin-cli as the first argument"

    printf 'smoke: testing %s\n' "$BIN"
    check_version_and_help
    check_foreground_signal
    check_daemon_pidfile
    check_default_mdns_boot
    check_subcommand_without_a_player
    check_control_socket
    check_stale_control_socket
    check_no_control
    check_missing_runtime_dir
    printf 'smoke: every check passed\n'
}

main
