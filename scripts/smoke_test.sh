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
# Boot-level checks for a built sendspin-cli, kept out of CTest because they need real processes, ports and signals.
#
# Usage: scripts/smoke_test.sh [path-to-sendspin-cli]

set -euo pipefail

BIN="${1:-build/sendspin-cli}"
readonly BIN

# High ports, one per concurrently running phase so TIME_WAIT cannot collide.
readonly PORT_FOREGROUND=39281
readonly PORT_DAEMON=39282
readonly PORT_DAEMON_SECOND=39283
readonly PORT_MDNS=39284
readonly PORT_CONTROL=39285
readonly PORT_CONTROL_SECOND=39286
readonly PORT_CONFIG=39287
readonly PORT_DELAY=39288
readonly PORT_REDACTION=39289

# Not a phase port: the address the redaction check dials, chosen so nothing answers it.
readonly PORT_NO_SERVER=39290

readonly MDNS_INSTANCE="sendspin-cli-smoke"

# Generous for loaded CI runners; every wait returns as soon as its condition holds.
readonly BOOT_TIMEOUT_S=20
readonly EXIT_TIMEOUT_S=15

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR

# A short runtime dir: macOS socket paths are limited to 104 bytes and mktemp's default is long.
CONTROL_DIR="$(mktemp -d /tmp/sscli-smoke.XXXXXX)"
readonly CONTROL_DIR

# Pinned state dir, so the suite never touches the invoking user's state.
XDG_STATE_HOME="$WORK_DIR/state-home"
export XDG_STATE_HOME
mkdir -p "$XDG_STATE_HOME"

# Every invocation names /dev/null as config: /etc/sendspin-cli.conf cannot be redirected by env.
readonly NO_CONFIG=(--config /dev/null)

# Tracked so fail() can sweep every started player.
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

# Waiting: polling loops, since timeout(1) is absent on macOS.

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

# Waits until `file` is non-empty, or `limit` seconds pass; the pidfile is created before it is written.
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

# Waits for a pid to disappear, or `limit` seconds pass; for the forked daemon.
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

# Reaps a child and returns its status; past the deadline it is killed and reported as 124.
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

# Whether an mDNS daemon is available: always on macOS, the avahi socket on Linux.
mdns_daemon_present() {
    if [ "$(uname -s)" = "Darwin" ]; then
        return 0
    fi
    [ -S /run/avahi-daemon/socket ] || [ -S /var/run/avahi-daemon/socket ]
}

# Checks

check_version_and_help() {
    local out
    out="$("$BIN" --version "${NO_CONFIG[@]}")" || fail "--version exited $?"
    printf '%s' "$out" | grep -q 'sendspin-cli' ||
        fail "--version exited 0 but named nothing: $out"
    pass "--version exits 0 and identifies the binary"

    "$BIN" --help "${NO_CONFIG[@]}" >/dev/null || fail "--help exited $?"
    pass "--help exits 0"
}

check_foreground_signal() {
    local log="$WORK_DIR/foreground.log"
    "$BIN" --no-mdns --no-control -o null --port "$PORT_FOREGROUND" "${NO_CONFIG[@]}" >"$log" 2>&1 &
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

    # -z returns once forked; the pidfile and log prove the daemon is up.
    "$BIN" -z -P "$pidfile" -f "$log" --no-mdns --no-control -o null --port "$PORT_DAEMON" "${NO_CONFIG[@]}" ||
        fail "-z exited $? instead of forking"

    wait_for_nonempty_file "$pidfile" "$BOOT_TIMEOUT_S" ||
        fail "no pidfile at $pidfile within ${BOOT_TIMEOUT_S}s"

    # Read now: the daemon unlinks its pidfile on exit.
    local pid
    pid="$(cat "$pidfile")"
    STARTED_PIDS+=("$pid")

    kill -0 "$pid" 2>/dev/null || fail "pidfile names pid $pid, which is not running"
    wait_for_line "$log" "listening on port $PORT_DAEMON" "$BOOT_TIMEOUT_S" ||
        fail "daemon never logged that it was listening. Log: $(cat "$log")"
    pass "-z forks and -P writes a pidfile holding a live pid"

    # A different port, so only the pidfile lock can refuse it.
    if "$BIN" -z -P "$pidfile" --no-mdns --no-control -o null --port "$PORT_DAEMON_SECOND" "${NO_CONFIG[@]}" \
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
    "$BIN" -o null --no-control --port "$PORT_MDNS" --mdns-name "$MDNS_INSTANCE" "${NO_CONFIG[@]}" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_MDNS" "$BOOT_TIMEOUT_S" ||
        fail "no ready log from the default configuration within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    # The expected outcome depends on the build and the host, so read both.
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
check_control_socket() {
    local log="$WORK_DIR/control.log"
    local socket="$CONTROL_DIR/sendspin-cli-$PORT_CONTROL.sock"
    local status_out="$WORK_DIR/control-status.out"
    local second_err="$WORK_DIR/control-second.err"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null --port "$PORT_CONTROL" "${NO_CONFIG[@]}" \
        >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONTROL" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    # 0600 despite the daemon's 0022 umask.
    local mode
    case "$(uname -s)" in
        Darwin) mode="$(stat -f '%Lp' "$socket")" ;;
        *) mode="$(stat -c '%a' "$socket")" ;;
    esac
    [ "$mode" = "600" ] || fail "the control socket is mode $mode, not 600"
    pass "the control socket appears at its default path, mode 0600"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_CONTROL" "${NO_CONFIG[@]}" >"$status_out" 2>&1 ||
        fail "status exited $? against a running player. Output: $(cat "$status_out")"
    grep -q '^name: ' "$status_out" || fail "status printed no name: $(cat "$status_out")"
    grep -q '^server: not connected' "$status_out" ||
        fail "status did not report the missing server connection: $(cat "$status_out")"
    grep -q '^group volume: ' "$status_out" ||
        fail "status printed no group volume line: $(cat "$status_out")"
    grep -q '^player volume: ' "$status_out" ||
        fail "status printed no player volume line: $(cat "$status_out")"
    grep -q '^static delay: [0-9]* ms$' "$status_out" ||
        fail "status printed no static delay line: $(cat "$status_out")"
    pass "status round-trips over the control socket, naming both volumes and the static delay"

    # Must say "not connected", not "unsupported".
    local pause_err="$WORK_DIR/control-pause.err"
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" pause --port "$PORT_CONTROL" "${NO_CONFIG[@]}" \
        >/dev/null 2>"$pause_err"; then
        fail "pause reported success with no server connected"
    fi
    grep -q 'not connected' "$pause_err" ||
        fail "pause blamed something other than the missing connection: $(cat "$pause_err")"
    pass "a transport command with no server reports the connection, not the command"

    # Refused before opening a device or port, in -P's words.
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null \
        --port "$PORT_CONTROL_SECOND" --control-socket "$socket" "${NO_CONFIG[@]}" \
        >/dev/null 2>"$second_err"; then
        fail "a second instance on the same control socket was allowed to start"
    fi
    grep -q 'already running' "$second_err" ||
        fail "the second instance was refused without saying why: $(cat "$second_err")"
    pass "a second instance on the same control socket is refused"

    # Refused at the terminal under -z, via the pre-fork probe.
    local second_z_err="$WORK_DIR/control-second-z.err"
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" -z --no-mdns -o null \
        --port "$PORT_CONTROL_SECOND" --control-socket "$socket" "${NO_CONFIG[@]}" \
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

# A socket left by a SIGKILLed daemon needs no manual cleanup.
check_stale_control_socket() {
    local socket="$CONTROL_DIR/stale.sock"
    local first="$WORK_DIR/stale-first.log"
    local second="$WORK_DIR/stale-second.log"

    "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --control-socket "$socket" "${NO_CONFIG[@]}" >"$first" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$first")"

    # Reaped at once so bash prints no "Killed" notice.
    kill -KILL "$pid"
    wait "$pid" 2>/dev/null || true
    [ -e "$socket" ] || fail "expected a stale socket file at $socket after SIGKILL"

    "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --control-socket "$socket" "${NO_CONFIG[@]}" >"$second" 2>&1 &
    local restarted=$!
    STARTED_PIDS+=("$restarted")
    wait_for_line "$second" "control: Listening on $socket" "$BOOT_TIMEOUT_S" ||
        fail "the restart did not take over the stale socket. Log: $(cat "$second")"
    "$BIN" status --control-socket "$socket" "${NO_CONFIG[@]}" >/dev/null 2>&1 ||
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

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null --port "$PORT_CONTROL" --no-control "${NO_CONFIG[@]}" \
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

# With no $XDG_RUNTIME_DIR: a platform fallback (macOS) or no socket, and audio either way.
check_missing_runtime_dir() {
    local log="$WORK_DIR/no-runtime-dir.log"

    env -u XDG_RUNTIME_DIR "$BIN" --no-mdns -o null --port "$PORT_CONTROL" "${NO_CONFIG[@]}" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONTROL" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    if grep -q '^I control: Listening on ' "$log"; then
        local socket
        socket="$(sed -n 's/^I control: Listening on //p' "$log" | head -1)"
        wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
            fail "the platform fallback logged $socket but bound nothing"

        local mode owner
        case "$(uname -s)" in
            Darwin)
                mode="$(stat -f '%Lp' "$socket")"
                owner="$(stat -f '%u' "$(dirname "$socket")")"
                ;;
            *)
                mode="$(stat -c '%a' "$socket")"
                owner="$(stat -c '%u' "$(dirname "$socket")")"
                ;;
        esac
        [ "$mode" = "600" ] || fail "the fallback socket is mode $mode, not 600"
        [ "$owner" = "$(id -u)" ] ||
            fail "the fallback directory $(dirname "$socket") is owned by uid $owner, not $(id -u)"
        case "$socket" in
            /tmp/*) fail "the platform fallback put the socket under /tmp: $socket" ;;
        esac

        env -u XDG_RUNTIME_DIR "$BIN" status --port "$PORT_CONTROL" "${NO_CONFIG[@]}" >/dev/null 2>&1 ||
            fail "a subcommand could not reach the fallback socket at $socket"
        pass "with no \$XDG_RUNTIME_DIR the platform's own private directory is used, 0600"
    else
        wait_for_line "$log" '^W control: .*XDG_RUNTIME_DIR' "$BOOT_TIMEOUT_S" ||
            fail "no control socket and no warning about why. Log: $(cat "$log")"
        # Never a /tmp fallback.
        if grep -q '/tmp' "$log"; then
            fail "the missing-runtime-dir path mentions /tmp. Log: $(cat "$log")"
        fi
        pass "with no \$XDG_RUNTIME_DIR and no platform fallback, it warns instead of guessing"
    fi

    kill -0 "$pid" 2>/dev/null ||
        fail "the player exited rather than carrying on. Log: $(cat "$log")"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status. Log: $(cat "$log")"
    pass "it keeps serving audio either way and exits 0 on SIGTERM"
}

# A subcommand with no player to talk to fails distinctly, and without becoming one.
check_subcommand_without_a_player() {
    local err="$WORK_DIR/no-daemon.err"
    local socket="$CONTROL_DIR/absent.sock"

    if "$BIN" status --control-socket "$socket" "${NO_CONFIG[@]}" >/dev/null 2>"$err"; then
        fail "status succeeded against a socket nothing is listening on"
    fi
    # 3, distinct from a bad command line.
    local status=0
    "$BIN" status --control-socket "$socket" "${NO_CONFIG[@]}" >/dev/null 2>&1 || status=$?
    [ "$status" -eq 3 ] || fail "expected exit 3 for a missing player, got $status"
    grep -q 'no sendspin-cli is listening' "$err" ||
        fail "the missing player was not named as such: $(cat "$err")"
    [ ! -e "$socket" ] || fail "a subcommand created $socket instead of only connecting to it"
    pass "a subcommand with no player exits 3 and starts nothing"
}

# A player configured from a file, found by a subcommand that repeats no flags.
check_config_file() {
    local config_home="$WORK_DIR/config-home"
    local config="$config_home/sendspin-cli/config"
    local log="$WORK_DIR/config.log"
    local status_out="$WORK_DIR/config-status.out"
    local socket="$CONTROL_DIR/sendspin-cli-$PORT_CONFIG.sock"

    mkdir -p "$config_home/sendspin-cli"
    # Covers a comment, a blank line, a long alias key, a boolean, and the port.
    cat >"$config" <<EOF
# a player configured entirely from a file
name = smoke-from-config

port = $PORT_CONFIG
output = null
no-mdns = true
buffer-ms = 250
EOF

    # The real search order, via $XDG_CONFIG_HOME, for both processes.
    XDG_CONFIG_HOME="$config_home" XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_CONFIG" "$BOOT_TIMEOUT_S" ||
        fail "no ready log in ${BOOT_TIMEOUT_S}s -- the config port did not take: $(cat "$log")"
    grep -q "Config file: $config" "$log" ||
        fail "the startup log did not name the config file in use: $(cat "$log")"
    grep -q 'as "smoke-from-config"' "$log" ||
        fail "the config's name did not take: $(cat "$log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    XDG_CONFIG_HOME="$config_home" XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status \
        >"$status_out" 2>&1 ||
        fail "status exited $? against a config-configured player. Output: $(cat "$status_out")"
    grep -q '^name: smoke-from-config' "$status_out" ||
        fail "status reached a different player than the config describes: $(cat "$status_out")"
    if grep -q 'a subcommand reads only' "$status_out"; then
        fail "status warned about daemon-only flags that came from a config file"
    fi
    pass "a player configured from a file comes up, and a subcommand finds it with no flags"

    local broken="$WORK_DIR/broken.conf"
    local broken_err="$WORK_DIR/broken.err"
    printf '# fine\nbuffer-ms = 5\n' >"$broken"
    if "$BIN" --config "$broken" >/dev/null 2>"$broken_err"; then
        fail "a config with an out-of-range buffer-ms started a player"
    fi
    # -F: the mktemp path contains a '.'.
    grep -qF "$broken:2:" "$broken_err" ||
        fail "the refusal did not name the file and line: $(cat "$broken_err")"
    grep -q 'expected 10-2000' "$broken_err" ||
        fail "the refusal did not use --buffer-ms's own message: $(cat "$broken_err")"
    "$BIN" --config "$broken" --help >/dev/null 2>&1 ||
        fail "--help stopped working because of a broken config"
    pass "a broken config is refused naming file and line, and --help still explains itself"

    kill -TERM "$pid" 2>/dev/null || true
    await_child "$pid" "$EXIT_TIMEOUT_S" >/dev/null 2>&1 || true
}

# `delay` reaches the role, shows in `status`, and survives a restart under an explicit --state-dir.
check_static_delay() {
    local log="$WORK_DIR/delay.log"
    local relog="$WORK_DIR/delay-restart.log"
    local state_dir="$WORK_DIR/delay-state"
    local socket="$CONTROL_DIR/sendspin-cli-$PORT_DELAY.sock"
    local out="$WORK_DIR/delay-status.out"

    mkdir -p "$state_dir"

    local -a player=(--no-mdns -o null --port "$PORT_DELAY" --state-dir "$state_dir")

    # --static-delay on a first run: the only check that the flag reaches the role before add_player().
    local seeded_dir="$WORK_DIR/delay-seeded"
    local seeded_out="$WORK_DIR/delay-seeded.out"
    mkdir -p "$seeded_dir"
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" --no-mdns -o null --port "$PORT_DELAY" \
        --state-dir "$seeded_dir" --static-delay 65 "${NO_CONFIG[@]}" \
        >"$WORK_DIR/delay-seeded.log" 2>&1 &
    local seeded_pid=$!
    STARTED_PIDS+=("$seeded_pid")
    wait_for_line "$WORK_DIR/delay-seeded.log" "listening on port $PORT_DELAY" "$BOOT_TIMEOUT_S" ||
        fail "the --static-delay player never came up. Log: $(cat "$WORK_DIR/delay-seeded.log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "the --static-delay player bound no control socket"
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$seeded_out" 2>&1 || fail "status exited $?. Output: $(cat "$seeded_out")"
    grep -q '^static delay: 65 ms$' "$seeded_out" ||
        fail "--static-delay 65 did not reach the player role on a first run: $(cat "$seeded_out")"
    pass "--static-delay seeds the delay on a first run, with nothing remembered"
    kill -TERM "$seeded_pid" 2>/dev/null || true
    await_child "$seeded_pid" "$EXIT_TIMEOUT_S" >/dev/null 2>&1 || true
    wait_for_absent "$socket" "$EXIT_TIMEOUT_S" ||
        fail "the --static-delay player exited but left $socket behind"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" "${player[@]}" "${NO_CONFIG[@]}" >"$log" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "listening on port $PORT_DELAY" "$BOOT_TIMEOUT_S" ||
        fail "no ready log within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "no control socket at $socket within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$out" 2>&1 || fail "status exited $?. Output: $(cat "$out")"
    grep -q '^static delay: 0 ms$' "$out" ||
        fail "a player with nothing remembered did not report a zero delay: $(cat "$out")"

    # Settable with no server connected.
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" delay 250 --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >/dev/null 2>&1 || fail "delay 250 exited $? against a player with no server"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$out" 2>&1 || fail "status exited $? after delay. Output: $(cat "$out")"
    grep -q '^static delay: 250 ms$' "$out" ||
        fail "delay 250 did not reach the player role: $(cat "$out")"
    pass "delay reaches the player role with no server connected, and status shows it"

    # Refused at the client; the library would clamp.
    local refusal="$WORK_DIR/delay-refusal.err"
    if XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" delay 5001 --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >/dev/null 2>"$refusal"; then
        fail "delay 5001 was accepted rather than refused"
    fi
    grep -q '0 to 5000' "$refusal" ||
        fail "the refusal did not name the bound: $(cat "$refusal")"
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$out" 2>&1 || fail "status exited $? after a refused delay. Output: $(cat "$out")"
    grep -q '^static delay: 250 ms$' "$out" ||
        fail "a refused delay changed the player anyway: $(cat "$out")"
    pass "delay 5001 is refused naming the bound, and leaves the player's delay alone"

    kill -TERM "$pid"
    local status=0
    await_child "$pid" "$EXIT_TIMEOUT_S" || status=$?
    [ "$status" -eq 0 ] ||
        fail "SIGTERM left exit status $status (124 means it never exited). Log: $(cat "$log")"

    # Persisted across a restart.
    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" "${player[@]}" "${NO_CONFIG[@]}" >"$relog" 2>&1 &
    pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$relog" "listening on port $PORT_DELAY" "$BOOT_TIMEOUT_S" ||
        fail "the restarted player never came up. Log: $(cat "$relog")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "the restarted player bound no control socket. Log: $(cat "$relog")"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$out" 2>&1 || fail "status exited $? after a restart. Output: $(cat "$out")"
    grep -q '^static delay: 250 ms$' "$out" ||
        fail "the delay did not survive a restart: $(cat "$out")"
    pass "a locally set delay survives a restart through the state store"

    # A remembered delay beats --static-delay.
    local flagged="$WORK_DIR/delay-flag-status.out"
    kill -TERM "$pid"
    await_child "$pid" "$EXIT_TIMEOUT_S" >/dev/null 2>&1 || true

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" "${player[@]}" --static-delay 40 "${NO_CONFIG[@]}" \
        >"$WORK_DIR/delay-flag.log" 2>&1 &
    pid=$!
    STARTED_PIDS+=("$pid")
    wait_for_line "$WORK_DIR/delay-flag.log" "listening on port $PORT_DELAY" "$BOOT_TIMEOUT_S" ||
        fail "the --static-delay player never came up. Log: $(cat "$WORK_DIR/delay-flag.log")"
    wait_for_socket "$socket" "$BOOT_TIMEOUT_S" ||
        fail "the --static-delay player bound no control socket"

    XDG_RUNTIME_DIR="$CONTROL_DIR" "$BIN" status --port "$PORT_DELAY" "${NO_CONFIG[@]}" \
        >"$flagged" 2>&1 || fail "status exited $?. Output: $(cat "$flagged")"
    grep -q '^static delay: 250 ms$' "$flagged" ||
        fail "--static-delay overrode a remembered delay, which it must not: $(cat "$flagged")"
    pass "--static-delay loses to a remembered delay, as a first-run default should"

    kill -TERM "$pid" 2>/dev/null || true
    await_child "$pid" "$EXIT_TIMEOUT_S" >/dev/null 2>&1 || true
}

# A -s URL carrying credentials is logged with them masked, and no line of ours prints them.
# The library's own `sendspin.*` lines are excluded: v0.7.0+ logs the dialled URL through a bare
# fprintf with no sink hook, so nothing here can redact them -- see docs/ROADMAP.md.
check_credential_redaction() {
    local log="$WORK_DIR/redaction.log"
    local out="$WORK_DIR/redaction.out"
    # Not a real credential, and it never leaves this host: PORT_NO_SERVER answers nothing, so the
    # dial fails before a byte is sent. On the command line because that is the leak being tested.
    local secret="s3cr3t-not-a-real-password"

    "$BIN" --no-mdns --no-control -o null --port "$PORT_REDACTION" "${NO_CONFIG[@]}" \
        -f "$log" -s "ws://smoke:$secret@127.0.0.1:$PORT_NO_SERVER/sendspin" >"$out" 2>&1 &
    local pid=$!
    STARTED_PIDS+=("$pid")

    wait_for_line "$log" "Connecting to" "$BOOT_TIMEOUT_S" ||
        fail "no dial line within ${BOOT_TIMEOUT_S}s. Log: $(cat "$log")"

    # -F, because the mask and the address are both regex metacharacters written literally.
    grep -qF "Connecting to ws://smoke:***@127.0.0.1:$PORT_NO_SERVER/sendspin" "$log" ||
        fail "the dial line did not mask the -s userinfo: $(grep 'Connecting to' "$log")"

    local leaked
    leaked="$(grep -hv ' sendspin\.[^ :]*:' "$log" "$out" | grep -F "$secret" || true)"
    [ -z "$leaked" ] ||
        fail "a sendspin-cli log line printed the -s password: $leaked"
    pass "the dial line masks a -s URL's userinfo, and no line of ours written by then holds it"

    kill -TERM "$pid" 2>/dev/null || true
    await_child "$pid" "$EXIT_TIMEOUT_S" >/dev/null 2>&1 || true
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
    check_static_delay
    check_config_file
    check_credential_redaction
    printf 'smoke: every check passed\n'
}

main
