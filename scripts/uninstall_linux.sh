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
# Removes what get_started_linux.sh installed: the service, in either mode, and the payload.
# Config, remembered state and the service account are kept unless --purge.
# Every command is printed first and confirmed, or pre-authorised with --yes.
#
# Usage: uninstall_linux.sh [--purge] [--yes]
#        curl -fsSL <raw url of this file> | bash -s -- [the same flags]
#
# --help describes each flag.

set -euo pipefail

readonly UNIT='sendspin-cli'
readonly UNIT_FILE='/usr/local/lib/systemd/system/sendspin-cli.service'
readonly USER_UNIT_FILE='/usr/local/lib/systemd/user/sendspin-cli.service'
readonly SYSUSERS_FILE='/usr/local/lib/sysusers.d/sendspin-cli.conf'
readonly SERVICE_USER='sendspin-cli'
readonly BINARY='/usr/local/bin/sendspin-cli'
readonly DOC_DIR='/usr/local/share/doc/sendspin-cli'
readonly SYSTEM_CONFIG='/etc/sendspin-cli.conf'
readonly SYSTEM_STATE='/var/lib/sendspin-cli'
# Keep in step with get_started_linux.sh: only a user unit carrying it is removed.
readonly OWN_UNIT_MARK='# Written by get_started_linux.sh'

fail() {
    printf 'uninstall_linux: FAIL: %s\n' "$*" >&2
    exit 1
}

say() {
    printf '%s\n' "$*"
}

step() {
    printf '\n==> %s\n' "$*"
}

usage() {
    cat <<'USAGE'
Usage: uninstall_linux.sh [--purge] [--yes]

  --purge  also remove what is otherwise kept: /etc/sendspin-cli.conf and
           ~/.config/sendspin-cli, the remembered state (pairings, volume) in
           /var/lib/sendspin-cli and ~/.local/state/sendspin-cli, and the 'sendspin-cli'
           account
  --yes    do not ask anything: run the commands as printed, and keep config, state and
           the account unless --purge says otherwise. Required when there is no terminal
           to ask on

Piped, flags follow `bash -s --`:

  curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/uninstall_linux.sh | bash -s -- --yes

Packages the installer added with apt, dnf or pacman are left installed.
USAGE
}

as_root() {
    if [ -n "$SUDO" ]; then
        "$SUDO" "$@"
    else
        "$@"
    fi
}

# Runs as the account a user service belongs to, with its user manager reachable.
as_user() {
    if [ "$(id -u)" -eq 0 ]; then
        sudo -u "$TARGET_USER" env "XDG_RUNTIME_DIR=/run/user/$TARGET_UID" "$@"
    else
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$TARGET_UID}" "$@"
    fi
}

# Answers come from fd 3: stdin, or the terminal when the script itself arrived on stdin.
open_prompt_fd() {
    CAN_ASK='no'
    if [ -t 0 ]; then
        exec 3<&0
        CAN_ASK='yes'
    elif { exec 3</dev/tty; } 2>/dev/null; then
        CAN_ASK='yes'
    fi
}

confirm() {
    printf '%s [y/N] ' "$1"
    # `|| fail`: set -e would otherwise exit silently on a closed terminal.
    read -r -u 3 REPLY || fail 'the terminal closed before an answer arrived'
    case "$REPLY" in
        y | Y | yes | YES) return 0 ;;
        *) return 1 ;;
    esac
}

# Queues a command for root, or for the user, to be printed and then run in order.
PLAN=()
plan_root() {
    PLAN+=("root$(printf ' %q' "$@")")
    NEED_ROOT='yes'
}
plan_user() {
    PLAN+=("user$(printf ' %q' "$@")")
}

main() {
    local purge='ask' assume_yes='no'

    while [ "$#" -gt 0 ]; do
        case "$1" in
            --purge)
                purge='yes'
                shift
                ;;
            --yes | -y)
                assume_yes='yes'
                shift
                ;;
            -h | --help)
                usage
                exit 0
                ;;
            *)
                fail "unknown argument '$1' -- see --help"
                ;;
        esac
    done

    [ "$(uname -s)" = 'Linux' ] || fail 'Linux only -- this removes a systemd service'

    # Booted systemd, not merely installed.
    HAVE_SYSTEMD='no'
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        HAVE_SYSTEMD='yes'
    fi
    readonly HAVE_SYSTEMD

    # Whose user service and config: the caller, or whoever sudo'd to root.
    TARGET_USER=''
    if [ "$(id -u)" -ne 0 ]; then
        TARGET_USER="$(id -un)"
    elif [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != 'root' ]; then
        TARGET_USER=$SUDO_USER
    fi
    TARGET_UID=''
    TARGET_HOME=''
    USER_P=''
    if [ -n "$TARGET_USER" ]; then
        TARGET_UID="$(id -u "$TARGET_USER")"
        TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
        [ "$(id -u)" -ne 0 ] || USER_P="sudo -u $TARGET_USER "
    fi
    readonly TARGET_USER TARGET_UID TARGET_HOME USER_P

    SUDO=''
    if [ "$(id -u)" -ne 0 ]; then
        SUDO='sudo'
    fi
    readonly SUDO
    readonly SUDO_P="${SUDO:+$SUDO }"

    open_prompt_fd
    readonly CAN_ASK

    # What is here to keep or remove
    local kept=()
    [ ! -e "$SYSTEM_CONFIG" ] || kept+=("$SYSTEM_CONFIG")
    [ ! -e "$SYSTEM_STATE" ] || kept+=("$SYSTEM_STATE")
    if [ -n "$TARGET_USER" ]; then
        for path in "$TARGET_HOME/.config/sendspin-cli" "$TARGET_HOME/.local/state/sendspin-cli"; do
            ! as_user test -e "$path" || kept+=("$path")
        done
    fi
    local have_account='no'
    if getent passwd "$SERVICE_USER" >/dev/null; then
        have_account='yes'
    fi

    if [ "$purge" = 'ask' ]; then
        purge='no'
        if [ "$assume_yes" != 'yes' ] && [ "$CAN_ASK" = 'yes' ] &&
            { [ "${#kept[@]}" -gt 0 ] || [ "$have_account" = 'yes' ]; }; then
            step 'Your config and what the player remembered'
            say ''
            for path in "${kept[@]}"; do
                say "  $path"
            done
            [ "$have_account" != 'yes' ] || say "  the '$SERVICE_USER' account"
            say ''
            say '  These are kept by default, so installing again picks up where this left off.'
            say ''
            ! confirm '  Remove them too?' || purge='yes'
        fi
    fi

    NEED_ROOT='no'

    if [ "$HAVE_SYSTEMD" = 'yes' ]; then
        if systemctl is-enabled --quiet "$UNIT" 2>/dev/null ||
            systemctl is-active --quiet "$UNIT" 2>/dev/null; then
            plan_root systemctl disable --now "$UNIT"
        fi
        if [ -n "$TARGET_USER" ] &&
            { as_user systemctl --user is-enabled --quiet "$UNIT" 2>/dev/null ||
                as_user systemctl --user is-active --quiet "$UNIT" 2>/dev/null; }; then
            plan_user systemctl --user disable --now "$UNIT"
        fi
    fi

    local removed_units='no' removed_user_unit='no'
    for path in "$BINARY" "$UNIT_FILE" "$USER_UNIT_FILE" "$SYSUSERS_FILE"; do
        if [ -e "$path" ]; then
            plan_root rm -f "$path"
            removed_units='yes'
        fi
    done
    [ ! -e "$DOC_DIR" ] || plan_root rm -rf "$DOC_DIR"

    local own_unit="$TARGET_HOME/.config/systemd/user/$UNIT.service" foreign_unit='no'
    if [ -n "$TARGET_USER" ] && as_user test -f "$own_unit"; then
        if as_user head -n 1 "$own_unit" | grep -Fq "$OWN_UNIT_MARK"; then
            plan_user rm -f "$own_unit"
            removed_user_unit='yes'
        else
            foreign_unit='yes'
        fi
    fi

    if [ "$purge" = 'yes' ]; then
        for path in "${kept[@]}"; do
            case "$path" in
                "$TARGET_HOME"/*) plan_user rm -rf "$path" ;;
                *) plan_root rm -rf "$path" ;;
            esac
        done
        [ "$have_account" != 'yes' ] || plan_root userdel "$SERVICE_USER"
    fi

    if [ "$HAVE_SYSTEMD" = 'yes' ]; then
        [ "$removed_units" != 'yes' ] || plan_root systemctl daemon-reload
        # Only where a user manager is running to be told.
        if { [ "$removed_units" = 'yes' ] || [ "$removed_user_unit" = 'yes' ]; } &&
            [ -n "$TARGET_USER" ] && as_user systemctl --user show-environment >/dev/null 2>&1; then
            plan_user systemctl --user daemon-reload
        fi
    fi

    if [ "${#PLAN[@]}" -eq 0 ]; then
        say 'sendspin-cli is not installed here; there is nothing to remove.'
        if [ "${#kept[@]}" -gt 0 ]; then
            say "Left in place (--purge removes them): ${kept[*]}"
        fi
        exit 0
    fi

    if [ "$NEED_ROOT" = 'yes' ] && [ -n "$SUDO" ]; then
        command -v sudo >/dev/null 2>&1 ||
            fail 'removing files from /usr/local needs root, and there is no sudo here --
    re-run it as root'
    fi

    step 'These are the commands this will run'
    say ''
    local entry
    for entry in "${PLAN[@]}"; do
        case "$entry" in
            root\ *) say "  ${SUDO_P}${entry#root }" ;;
            user\ *) say "  ${USER_P}${entry#user }" ;;
        esac
    done
    say ''
    [ "$have_account" != 'yes' ] || kept+=("the '$SERVICE_USER' account")
    if [ "$purge" != 'yes' ] && [ "${#kept[@]}" -gt 0 ]; then
        say "Kept: ${kept[*]}"
        say '--purge removes those as well.'
    fi

    if [ "$assume_yes" != 'yes' ]; then
        [ "$CAN_ASK" = 'yes' ] ||
            fail 'there is no terminal to confirm those commands on. Re-run with --yes if you
    have read them and want them run; nothing was removed'
        say ''
        confirm 'Run them?' || fail 'nothing was removed'
    fi

    step 'Removing'
    for entry in "${PLAN[@]}"; do
        # The entries were quoted with %q when queued, so eval restores the exact words.
        case "$entry" in
            root\ *) eval "as_root ${entry#root }" ;;
            user\ *) eval "as_user ${entry#user }" ;;
        esac
    done
    say '  done'

    if [ "$foreign_unit" = 'yes' ]; then
        say ''
        say "  $own_unit was not written by the installer, so it is left."
    fi
    if [ -n "$TARGET_USER" ] && [ -e "/var/lib/systemd/linger/$TARGET_USER" ]; then
        say ''
        say "  $TARGET_USER still lingers, which the user service needed and other user services"
        say "  may too. To undo it:  ${SUDO_P}loginctl disable-linger $TARGET_USER"
    fi
    say ''
}

# Braces, so a download cut short anywhere is a syntax error and runs nothing.
{ main "$@"; }
