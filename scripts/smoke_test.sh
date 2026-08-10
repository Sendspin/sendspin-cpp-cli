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
# and exits 0 on SIGTERM.
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

# High enough to be clear of anything a developer is plausibly running, and one per phase so a
# socket still in TIME_WAIT from the phase before cannot fail the next one.
readonly PORT_FOREGROUND=39281
readonly PORT_DAEMON=39282
readonly PORT_DAEMON_SECOND=39283
readonly PORT_MDNS=39284

readonly MDNS_INSTANCE="sendspin-cli-smoke"

# Sized for a loaded shared CI runner rather than for a developer's laptop: the cost of being
# generous is paid only when something is already wrong, and every wait below returns as soon
# as its condition holds.
readonly BOOT_TIMEOUT_S=20
readonly EXIT_TIMEOUT_S=15

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR

# Every check stops what it started on the way through, but fail() exits from wherever it is
# called -- so the pids are tracked and swept here as well, to leave no player holding a port
# after a failure.
STARTED_PIDS=()

cleanup() {
    local pid
    for pid in ${STARTED_PIDS[@]+"${STARTED_PIDS[@]}"}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK_DIR"
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

# Whether this host has something implementing DNS-SD for the player to register with.
# mDNSResponder is always up on macOS; on Linux avahi-daemon is there only if the image or the
# workflow put it there.
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
    "$BIN" --no-mdns -o null --port "$PORT_FOREGROUND" >"$log" 2>&1 &
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
    "$BIN" -z -P "$pidfile" -f "$log" --no-mdns -o null --port "$PORT_DAEMON" ||
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
    if "$BIN" -z -P "$pidfile" --no-mdns -o null --port "$PORT_DAEMON_SECOND" \
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
    "$BIN" -o null --port "$PORT_MDNS" --mdns-name "$MDNS_INSTANCE" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_MDNS" "$BOOT_TIMEOUT_S" ||
        fail "no ready log from the default configuration within ${BOOT_TIMEOUT_S}s. " \
            "Log: $(cat "$log")"

    # Which of the two outcomes is the correct one is a property of the host rather than of the
    # build, so it is read off the host instead of assumed. Asserting only the branch this
    # happens to take would mean the check quietly starts testing the other one the day a
    # runner image gains -- or loses -- an mDNS daemon.
    if mdns_daemon_present; then
        wait_for_line "$log" 'advertising _sendspin\._tcp' "$BOOT_TIMEOUT_S" ||
            fail "an mDNS daemon is running but nothing was advertised. Log: $(cat "$log")"
        pass "the default configuration advertises where an mDNS daemon is running"
    else
        wait_for_line "$log" '^W mdns: .*retrying' "$BOOT_TIMEOUT_S" ||
            fail "no mDNS daemon is running, and no retry warning was logged. " \
                "Log: $(cat "$log")"
        pass "the default configuration warns and retries where no mDNS daemon is running"
    fi

    # The whole point of that warning: an advertisement that cannot be made is not fatal, so
    # the player must still be serving its port.
    kill -0 "$pid" 2>/dev/null ||
        fail "the player exited rather than carrying on without an advertisement. " \
            "Log: $(cat "$log")"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status (124 means it never exited). Log: $(cat "$log")"
    pass "the default configuration boots and exits 0 on SIGTERM"
}

main() {
    [ -x "$BIN" ] ||
        fail "no executable at '$BIN' -- pass the path to sendspin-cli as the first argument"

    printf 'smoke: testing %s\n' "$BIN"
    check_version_and_help
    check_foreground_signal
    check_daemon_pidfile
    check_default_mdns_boot
    printf 'smoke: every check passed\n'
}

main
