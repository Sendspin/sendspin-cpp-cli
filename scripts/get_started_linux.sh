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
# Installs a released sendspin-cli on a Linux host, asks for a name and an output device, and
# starts it as a systemd service. Verifies SHA256SUMS.
# Every root command is printed first and confirmed, or pre-authorised with --yes.
#
# Usage: get_started_linux.sh [--version <tag>] [--name <name>] [--output <device>]
#                             [--user-service | --system-service] [--yes]
#        curl -fsSL <raw url of this file> | bash -s -- [the same flags]
#
# --help describes each flag.

set -euo pipefail

# Fixed so this script can never install someone else's binary.
readonly REPO='Sendspin/sendspin-cpp-cli'

readonly UNIT='sendspin-cli'
readonly UNIT_FILE='/usr/local/lib/systemd/system/sendspin-cli.service'
readonly USER_UNIT_FILE='/usr/local/lib/systemd/user/sendspin-cli.service'
readonly SYSUSERS_FILE='/usr/local/lib/sysusers.d/sendspin-cli.conf'
readonly SERVICE_USER='sendspin-cli'
readonly BINARY='/usr/local/bin/sendspin-cli'
readonly SYSTEM_CONFIG='/etc/sendspin-cli.conf'
readonly CONFIG_EXAMPLE='/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example'
readonly CONTROL_SOCKET='/run/sendspin-cli/control.sock'
# First line of a user unit this script wrote, so only its own file is ever replaced or removed.
readonly OWN_UNIT_MARK='# Written by get_started_linux.sh'

fail() {
    printf 'get_started_linux: FAIL: %s\n' "$*" >&2
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
Usage: get_started_linux.sh [--version <tag>] [--name <name>] [--output <device>]
                            [--user-service | --system-service] [--yes]

  --version <tag>   install this release instead of the latest, e.g. --version v0.3.0
  --name <name>     the name a server shows for this player, instead of being asked.
                    Ignored when the config already sets one
  --output <device> the output device, e.g. hw:1,0, instead of being asked.
                    Ignored when the config already sets one
  --system-service  run as the unprivileged 'sendspin-cli' account, configured in
                    /etc/sendspin-cli.conf. The default
  --user-service    run as you instead, as a systemd user service that follows your
                    session's PipeWire or PulseAudio, configured in
                    ~/.config/sendspin-cli/config
  --yes             do not ask anything: run the root commands as printed, and take the
                    name and the output only from the flags above. Required when there
                    is no terminal to ask on. Without --output and with none configured,
                    the service is enabled and not started

Piped, flags follow `bash -s --`:

  curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh | bash -s -- --yes

Environment:

  SENDSPIN_CLI_TARBALL  install this payload instead of downloading a release: a DESTDIR
                        staged `cmake --install` tree, tarred as CI publishes it. It is
                        NOT checksummed.
USAGE
}

as_root() {
    if [ -n "$SUDO" ]; then
        "$SUDO" "$@"
    else
        "$@"
    fi
}

# Runs as the account the user service belongs to, with its user manager reachable.
as_user() {
    if [ "$(id -u)" -eq 0 ]; then
        sudo -u "$TARGET_USER" env "XDG_RUNTIME_DIR=/run/user/$TARGET_UID" "$@"
    else
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$TARGET_UID}" "$@"
    fi
}

# As whoever owns the config for this mode.
as_owner() {
    if [ "$MODE" = 'user' ]; then
        as_user "$@"
    else
        as_root "$@"
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

# ask <prompt> [default]: the answer lands in REPLY.
ask() {
    printf '%s' "$1"
    # `|| fail`: set -e would otherwise exit silently on a closed terminal.
    read -r -u 3 REPLY || fail 'the terminal closed before an answer arrived'
    [ -n "$REPLY" ] || REPLY="${2:-}"
}

confirm() {
    ask "$1 [y/N] "
    case "$REPLY" in
        y | Y | yes | YES) return 0 ;;
        *) return 1 ;;
    esac
}

# config_sets <key>: does this mode's config set it. As the owner: the file may be unreadable.
config_sets() {
    as_owner test -f "$CONFIG" &&
        as_owner grep -Eq "^[[:space:]]*$1[[:space:]]*=" "$CONFIG"
}

# set_key <file> <key> <value>: replaces the example's commented line, else appends.
set_key() {
    # Through the environment: awk -v would interpret backslashes in the value.
    VALUE=$3 awk -v key="$2" '
        !done && index($0, "#" key " = ") == 1 { print key " = " ENVIRON["VALUE"]; done = 1; next }
        { print }
        END { if (!done) print key " = " ENVIRON["VALUE"] }
    ' "$1" >"$1.new"
    mv "$1.new" "$1"
}

# The newest release tag, read off the /releases/latest redirect: no jq, no API rate limit,
# and it tells "no releases" apart from "no such repository".
resolve_latest_tag() {
    local headers status location
    headers="$WORK_DIR/latest.headers"

    status="$(curl -sSI -o "$headers" -w '%{http_code}' \
        "https://github.com/$REPO/releases/latest")" ||
        fail "could not reach github.com -- check this host's network and try again"

    case "$status" in
        30[0-9]) ;;
        404)
            fail "github.com has no repository at $REPO, or it is not public. Nothing has
    been installed"
            ;;
        *)
            fail "github.com answered $status asking for the latest release of $REPO"
            ;;
    esac

    # `|| true`: under pipefail a missing location line would kill the script before the guard.
    location="$( (grep -i '^location:' "$headers" || true) | tail -n 1 | tr -d '\r' |
        awk '{print $2}')"
    [ -n "$location" ] ||
        fail "github.com answered $status with no Location header, which should not happen"

    case "$location" in
        */releases/tag/*)
            printf '%s\n' "${location##*/releases/tag/}"
            ;;
        *)
            fail "$REPO has published no releases yet, so there is nothing to download.
    Until it has, either build from source -- https://github.com/$REPO#build -- or stage a
    payload of your own and point this script at it:
        DESTDIR=/tmp/stage cmake --install build --component sendspin-cli
        tar -czf /tmp/sendspin-cli.tar.gz -C /tmp stage
        SENDSPIN_CLI_TARBALL=/tmp/sendspin-cli.tar.gz ./get_started_linux.sh --yes"
            ;;
    esac
}

# package_for <soname>: this host's package for a library the binary links, or nothing.
package_for() {
    local names
    case "$1" in
        libasound.so.*) names='libasound2 alsa-lib alsa-lib' ;;
        libportaudio.so.*) names='libportaudio2 portaudio portaudio' ;;
        libpulse.so.*) names='libpulse0 pulseaudio-libs libpulse' ;;
        libpipewire-0.3.so.*) names='libpipewire-0.3-0 pipewire-libs libpipewire' ;;
        libdns_sd.so.*) names='libavahi-compat-libdnssd1 avahi-compat-libdns_sd avahi' ;;
        libatomic.so.*) names='libatomic1 libatomic libatomic' ;;
        *) return 0 ;;
    esac
    # shellcheck disable=SC2086  # three words: apt, dnf, pacman
    set -- $names
    case "$PKG_MANAGER" in
        apt-get)
            # Since the 64-bit time_t transition some of these carry a t64 suffix, per release and arch.
            if apt-cache show "${1}t64" >/dev/null 2>&1; then
                printf '%s\n' "${1}t64"
            else
                printf '%s\n' "$1"
            fi
            ;;
        dnf) printf '%s\n' "$2" ;;
        pacman) printf '%s\n' "$3" ;;
    esac
}

# Fills PKG_INSTALL with the command that installs what the staged binary cannot resolve.
find_missing_packages() {
    local ldd_out lib pkg
    PKG_INSTALL=()
    command -v ldd >/dev/null 2>&1 || return 0
    # ldd exits non-zero on a binary it cannot load at all; the text is what is read.
    ldd_out="$(ldd "$STAGED_BINARY" 2>&1 || true)"

    if grep -q 'GLIBC_.*not found' <<<"$ldd_out"; then
        fail "this build needs a newer glibc than this host has:
$(grep 'GLIBC_.*not found' <<<"$ldd_out" | sed 's/^[[:space:]]*/    /')
    Nothing has been installed. Move to a newer release of your distribution, or build from
    source: https://github.com/$REPO#build"
    fi

    mapfile -t MISSING_LIBS < <(awk '/=> not found/ { print $1 }' <<<"$ldd_out" | sort -u)
    [ "${#MISSING_LIBS[@]}" -gt 0 ] || return 0

    PKG_MANAGER=''
    for candidate in apt-get dnf pacman; do
        if command -v "$candidate" >/dev/null 2>&1; then
            PKG_MANAGER=$candidate
            break
        fi
    done
    [ -n "$PKG_MANAGER" ] ||
        fail "sendspin-cli cannot load on this host: ${MISSING_LIBS[*]} missing, and there is
    no apt-get, dnf or pacman here to install them with. Nothing has been installed. Install
    the packages that provide those libraries, then re-run this script"

    local packages=()
    for lib in "${MISSING_LIBS[@]}"; do
        pkg="$(package_for "$lib")"
        [ -n "$pkg" ] ||
            fail "sendspin-cli cannot load on this host: $lib is missing, and this script does
    not know which $PKG_MANAGER package provides it. Nothing has been installed. Install it,
    then re-run this script"
        packages+=("$pkg")
    done

    case "$PKG_MANAGER" in
        apt-get) PKG_INSTALL=(apt-get install -y "${packages[@]}") ;;
        dnf) PKG_INSTALL=(dnf install -y "${packages[@]}") ;;
        pacman) PKG_INSTALL=(pacman -S --needed --noconfirm "${packages[@]}") ;;
    esac
}

# The unit a payload older than the shipped user unit gets instead.
write_own_user_unit() {
    as_user mkdir -p "$USER_UNIT_DIR"
    as_user tee "$USER_UNIT_DIR/$UNIT.service" >/dev/null <<UNIT
$OWN_UNIT_MARK: the installed release carries no user unit of its own.

[Unit]
Description=Sendspin audio player
Documentation=https://github.com/$REPO
After=pipewire.service pipewire-pulse.service pulseaudio.service

[Service]
Type=simple
NoNewPrivileges=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
RestrictSUIDSGID=yes
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK
SystemCallArchitectures=native
SystemCallFilter=@system-service
ExecStart=$BINARY
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
UNIT
}

# is_own_unit <path>: did this script write it.
is_own_unit() {
    as_user test -f "$1" && as_user head -n 1 "$1" | grep -Fq "$OWN_UNIT_MARK"
}

# ctl <systemctl args>: this mode's service manager.
ctl() {
    if [ "$MODE" = 'user' ]; then
        as_user systemctl --user "$@"
    else
        as_root systemctl "$@"
    fi
}

show_devices() {
    step 'What this host can play through'
    say ''
    say '  (ALSA and PortAudio narrate their own enumeration on stderr -- a "jack server is'
    say "  not running\" here is those libraries talking, not this player failing.)"
    say ''
    # Only the output half of the listing.
    as_owner "$BINARY" -l 2>&1 | sed -e '/^Input devices/,$d' -e 's/^/  /'
}

# Restarts the service and says whether it stayed up.
start_and_report() {
    local journal
    # restart, not start, so an upgrade replaces the running binary.
    ctl restart "$UNIT"

    # Type=simple returns immediately; a failing device takes about a second to show.
    sleep 2
    if ctl is-active --quiet "$UNIT"; then
        step "$UNIT is running"
        say "  output = $(as_owner sed -n 's/^[[:space:]]*output[[:space:]]*=[[:space:]]*//p' "$CONFIG" | tail -n 1)"
        return 0
    fi

    step "$UNIT was started and is NOT running"
    say ''
    say "  $CONFIG names an output, so this is most likely that device failing to"
    say '  open. What it said:'
    say ''
    # Empty output counts as failure: users outside systemd-journal get nothing and exit 0.
    if [ "$MODE" = 'user' ]; then
        journal="$(as_user journalctl --user -u "$UNIT" --no-pager -n 15 2>/dev/null || true)"
    else
        journal="$(as_root journalctl -u "$UNIT" --no-pager -n 15 2>/dev/null || true)"
    fi
    if [ -n "$journal" ]; then
        printf '%s\n' "$journal" | sed 's/^/    /'
    else
        say "    (nothing readable in the journal; try: $JOURNAL_P -n 50)"
    fi
    say ''
    say "  '$OWNER_P$BINARY -l' lists what this host really has. Change 'output' in"
    say "  $CONFIG, then: ${CTL_P} restart $UNIT"
}

main() {
    # What was asked for

    local version_tag='' assume_yes='no' want_name='' want_output='' want_mode=''

    while [ "$#" -gt 0 ]; do
        case "$1" in
            --version)
                [ "$#" -ge 2 ] || fail '--version needs a tag, e.g. --version v0.3.0'
                version_tag=$2
                shift 2
                ;;
            --name)
                { [ "$#" -ge 2 ] && [ -n "$2" ]; } || fail '--name needs a name, e.g. --name kitchen'
                want_name=$2
                shift 2
                ;;
            --output)
                { [ "$#" -ge 2 ] && [ -n "$2" ]; } || fail '--output needs a device, e.g. --output hw:1,0'
                want_output=$2
                shift 2
                ;;
            --user-service)
                want_mode='user'
                shift
                ;;
            --system-service)
                want_mode='system'
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

    case "$want_name$want_output" in
        *$'\n'*) fail '--name and --output are one line each' ;;
    esac

    # Is this a host this can work on at all

    [ "$(uname -s)" = 'Linux' ] ||
        fail "Linux only -- this installs a systemd unit. On macOS take the installer .pkg from
    https://github.com/$REPO/releases instead"

    for tool in tar sed grep awk; do
        command -v "$tool" >/dev/null 2>&1 ||
            fail "'$tool' is not on \$PATH, and this cannot install anything without it"
    done

    # Pick the archive by userland, not uname -m: a Pi with a 64-bit kernel can run a 32-bit userland.
    MACHINE="$(uname -m)"

    if command -v dpkg >/dev/null 2>&1; then
        USERLAND="$(dpkg --print-architecture)"
    elif command -v getconf >/dev/null 2>&1; then
        case "$(getconf LONG_BIT):$MACHINE" in
            64:x86_64 | 64:amd64) USERLAND='amd64' ;;
            64:aarch64 | 64:arm64) USERLAND='arm64' ;;
            32:aarch64 | 32:arm64 | 32:arm*) USERLAND='armhf' ;;
            # Unidentified userlands are refused below, never guessed from the kernel.
            *) USERLAND='' ;;
        esac
    else
        fail "neither dpkg nor getconf is on \$PATH, and one of them is needed to tell a 32-bit
    userland from the 64-bit kernel it may be running under"
    fi

    # Archives are named for the CI leg that built them.
    case "$USERLAND" in
        amd64 | x86_64)
            LEG='linux-x86_64'
            ;;
        arm64 | aarch64)
            LEG='linux-arm64'
            ;;
        armhf)
            # 32-bit ARM: the CPU decides between ARMv6 and ARMv7; anything older is refused.
            case "$MACHINE" in
                armv6*)
                    LEG='linux-armv6'
                    ;;
                armv[0-5]* | arm)
                    fail "'$MACHINE' is older than ARMv6, or names no ARM architecture at all, and
    the oldest archive built is linux-armv6 -- an ARM1176, which is a Pi Zero, a Pi Zero W or an
    original Pi. Its instructions would be illegal here, so there is nothing to install. Build
    from source instead: https://github.com/$REPO#build"
                    ;;
                *)
                    LEG='linux-armv7'
                    ;;
            esac
            ;;
        '')
            fail "this host's userland could not be identified from a $MACHINE kernel alone, and
    guessing at it is how a 32-bit userland ends up with a 64-bit binary. Install dpkg, or
    build from source: https://github.com/$REPO#build"
            ;;
        *)
            fail "no release is built for a '$USERLAND' userland -- the archives are linux-x86_64,
    linux-arm64, linux-armv7 and linux-armv6. Build from source instead:
    https://github.com/$REPO#build"
            ;;
    esac
    readonly MACHINE USERLAND LEG

    # Booted systemd, not merely installed.
    HAVE_SYSTEMD='no'
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        HAVE_SYSTEMD='yes'
    fi
    readonly HAVE_SYSTEMD

    # /proc/device-tree/model is NUL-terminated.
    PI_MODEL=''
    if [ -r /proc/device-tree/model ]; then
        model="$(tr -d '\0' </proc/device-tree/model)"
        case "$model" in
            *'Raspberry Pi'*) PI_MODEL=$model ;;
        esac
    fi
    readonly PI_MODEL

    # The one place root is reached.
    SUDO=''
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 ||
            fail 'this needs root to install into /usr/local and to drive systemctl, and there is
    no sudo here -- re-run it as root'
        SUDO='sudo'
    fi
    readonly SUDO

    # `sudo ` or nothing, so printed commands are not misindented when already root.
    readonly SUDO_P="${SUDO:+$SUDO }"

    # Who a user service would run as: the caller, or whoever sudo'd to root.
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
    readonly USER_CONFIG="$TARGET_HOME/.config/sendspin-cli/config"
    readonly USER_UNIT_DIR="$TARGET_HOME/.config/systemd/user"

    open_prompt_fd
    readonly CAN_ASK

    WORK_DIR="$(mktemp -d)"
    readonly WORK_DIR
    trap 'rm -rf "$WORK_DIR"' EXIT

    say 'sendspin-cli getting started'
    say "  host:         Linux $MACHINE, $USERLAND userland${PI_MODEL:+  ($PI_MODEL)}"
    say "  release leg:  $LEG"
    if [ "$HAVE_SYSTEMD" = 'yes' ]; then
        say '  systemd:      yes'
    else
        say '  systemd:      no -- the binary will be installed but no service set up'
    fi

    # The payload

    if [ -n "${SENDSPIN_CLI_TARBALL:-}" ]; then
        step 'Using the payload you supplied'
        [ -f "$SENDSPIN_CLI_TARBALL" ] ||
            fail "SENDSPIN_CLI_TARBALL names '$SENDSPIN_CLI_TARBALL', which is not a file"
        # Absolute, since tar -C / would resolve a relative path against /.
        TARBALL="$(cd "$(dirname "$SENDSPIN_CLI_TARBALL")" && pwd)"
        TARBALL="${TARBALL%/}/$(basename "$SENDSPIN_CLI_TARBALL")"
        say "  $TARBALL"
        say ''
        say '  !! NOT VERIFIED. This is your own archive, so there is no published SHA256SUMS to'
        say '  !! check it against, and nothing here has established that it is what you think.'
        say '  !! Take a release instead of passing SENDSPIN_CLI_TARBALL to get that check.'
    else
        step 'Finding the release to install'
        for tool in curl sha256sum; do
            command -v "$tool" >/dev/null 2>&1 ||
                fail "'$tool' is not on \$PATH -- install it (apt install curl coreutils) and
    re-run, or pass a payload with SENDSPIN_CLI_TARBALL"
        done

        if [ -n "$version_tag" ]; then
            TAG=$version_tag
        else
            TAG="$(resolve_latest_tag)"
        fi
        # The archives are named for the version, which is the tag without its `v`.
        ARCHIVE="sendspin-cli-${TAG#v}-$LEG.tar.gz"
        readonly TAG ARCHIVE
        say "  $TAG  ->  $ARCHIVE"

        step 'Downloading and verifying'
        BASE="https://github.com/$REPO/releases/download/$TAG"
        curl -fSL --progress-bar -o "$WORK_DIR/$ARCHIVE" "$BASE/$ARCHIVE" ||
            fail "no $ARCHIVE in release $TAG of $REPO. Check the tag exists and carries a build
    for this architecture: https://github.com/$REPO/releases"
        curl -fsSL -o "$WORK_DIR/SHA256SUMS" "$BASE/SHA256SUMS" ||
            fail "$TAG carries $ARCHIVE but no SHA256SUMS, so there is nothing to verify it
    against. Refusing to install an unverified binary"

        # --ignore-missing would pass a SHA256SUMS that omits this archive, so check it is listed.
        awk -v want="$ARCHIVE" '$2 == want { found = 1 } END { exit !found }' \
            "$WORK_DIR/SHA256SUMS" ||
            fail "the SHA256SUMS published with $TAG does not list $ARCHIVE, so there is no
    checksum to verify it against. Nothing has been installed"

        (cd "$WORK_DIR" && sha256sum --ignore-missing -c SHA256SUMS) ||
            fail "$ARCHIVE does not match the checksum $TAG publishes for it. Nothing has been
    installed. Download it again; if it fails a second time, say so on the issue tracker
    rather than installing it anyway"

        TARBALL="$WORK_DIR/$ARCHIVE"
    fi
    readonly TARBALL

    # Listed once: `tar | grep -q` dies of SIGPIPE, which pipefail reports as failure.
    ARCHIVE_LIST="$(tar -tzf "$TARBALL")"
    readonly ARCHIVE_LIST

    mapfile -t ARCHIVE_ROOTS < <(cut -d/ -f1 <<<"$ARCHIVE_LIST" | sort -u)
    [ "${#ARCHIVE_ROOTS[@]}" -eq 1 ] ||
        fail "'$TARBALL' has ${#ARCHIVE_ROOTS[@]} top-level entries, and a payload has one -- it
    is a staged 'cmake --install' tree, not an archive of loose files"
    PAYLOAD_ROOT="${ARCHIVE_ROOTS[0]}"
    readonly PAYLOAD_ROOT

    grep -Fqx "$PAYLOAD_ROOT/usr/local/bin/sendspin-cli" <<<"$ARCHIVE_LIST" ||
        fail "'$TARBALL' holds no $PAYLOAD_ROOT/usr/local/bin/sendspin-cli. A payload is staged
    with DESTDIR from a build configured for the /usr/local prefix:
    DESTDIR=/tmp/stage cmake --install build --component sendspin-cli"

    # Read off the archive so the printed plan matches what runs.
    PAYLOAD_HAS_SYSUSERS='no'
    if grep -Fqx "$PAYLOAD_ROOT/usr/local/lib/sysusers.d/sendspin-cli.conf" <<<"$ARCHIVE_LIST"; then
        PAYLOAD_HAS_SYSUSERS='yes'
    fi
    readonly PAYLOAD_HAS_SYSUSERS

    # An unprivileged copy, so missing libraries are known before anything is installed.
    mkdir "$WORK_DIR/staged"
    tar -xzf "$TARBALL" --strip-components=1 -C "$WORK_DIR/staged" "$PAYLOAD_ROOT/usr"
    readonly STAGED_BINARY="$WORK_DIR/staged$BINARY"
    find_missing_packages
    readonly PKG_INSTALL

    # Which service, if any

    # What an earlier run left enabled decides an upgrade's mode.
    SYSTEM_UNIT_ON='no'
    USER_UNIT_ON='no'
    if [ "$HAVE_SYSTEMD" = 'yes' ]; then
        if systemctl is-enabled --quiet "$UNIT" 2>/dev/null ||
            systemctl is-active --quiet "$UNIT" 2>/dev/null; then
            SYSTEM_UNIT_ON='yes'
        fi
        if [ -n "$TARGET_USER" ] &&
            { as_user systemctl --user is-enabled --quiet "$UNIT" 2>/dev/null ||
                as_user systemctl --user is-active --quiet "$UNIT" 2>/dev/null; }; then
            USER_UNIT_ON='yes'
        fi
    fi
    readonly SYSTEM_UNIT_ON USER_UNIT_ON

    MODE=$want_mode
    if [ "$HAVE_SYSTEMD" != 'yes' ]; then
        [ "$MODE" != 'user' ] ||
            fail '--user-service needs a running systemd, and there is none on this host'
        MODE='system'
    elif [ -z "$MODE" ]; then
        if [ "$USER_UNIT_ON" = 'yes' ] && [ "$SYSTEM_UNIT_ON" != 'yes' ]; then
            MODE='user'
        elif [ "$SYSTEM_UNIT_ON" = 'yes' ] || [ -z "$TARGET_USER" ] ||
            [ "$assume_yes" = 'yes' ] || [ "$CAN_ASK" != 'yes' ]; then
            MODE='system'
        else
            step 'How should the player run?'
            say ''
            say "  1) As a system service under its own unprivileged '$SERVICE_USER' account."
            say '     Plays straight to a sound card you name. Best for a headless box or a Pi.'
            say "  2) As you ($TARGET_USER), as a user service. Follows your session's PipeWire or"
            say "     PulseAudio, so 'default' plays wherever your desktop does."
            say ''
            ask 'Choose 1 or 2 [1]: ' 1
            case "$REPLY" in
                1) MODE='system' ;;
                2) MODE='user' ;;
                *) fail "'$REPLY' is neither 1 nor 2; nothing was installed" ;;
            esac
        fi
    fi
    if [ "$MODE" = 'user' ] && [ -z "$TARGET_USER" ]; then
        fail '--user-service needs a user to run as, and this is root with no sudo caller.
    Run it as that user instead'
    fi
    readonly MODE

    if [ "$MODE" = 'user' ]; then
        CONFIG=$USER_CONFIG
        OWNER_P=$USER_P
        CTL_P="${USER_P}systemctl --user"
        JOURNAL_P="${USER_P}journalctl --user -u $UNIT"
    else
        CONFIG=$SYSTEM_CONFIG
        OWNER_P=$SUDO_P
        CTL_P="${SUDO_P}systemctl"
        JOURNAL_P="${SUDO_P}journalctl -u $UNIT"
    fi
    readonly CONFIG OWNER_P CTL_P JOURNAL_P

    # Only the system unit runs as the account.
    CREATE_USER='no'
    if [ "$MODE" = 'system' ] && [ "$HAVE_SYSTEMD" = 'yes' ] && [ "$PAYLOAD_HAS_SYSUSERS" = 'yes' ]; then
        CREATE_USER='yes'
    fi
    readonly CREATE_USER

    CONFIG_EXISTS='no'
    if as_owner test -e "$CONFIG"; then
        CONFIG_EXISTS='yes'
    fi
    readonly CONFIG_EXISTS

    # flag, ask, or none -- a key the config already sets is never asked for or replaced.
    NAME_FROM='none'
    OUTPUT_FROM='none'
    CONFIG_HAS_OUTPUT='no'
    if [ "$HAVE_SYSTEMD" = 'yes' ]; then
        if config_sets output; then
            CONFIG_HAS_OUTPUT='yes'
            [ -z "$want_output" ] || say "  note: $CONFIG already sets an output, so --output is ignored"
        elif [ -n "$want_output" ]; then
            OUTPUT_FROM='flag'
        elif [ "$assume_yes" != 'yes' ]; then
            OUTPUT_FROM='ask'
        fi
        if config_sets name; then
            [ -z "$want_name" ] || say "  note: $CONFIG already sets a name, so --name is ignored"
        elif [ -n "$want_name" ]; then
            NAME_FROM='flag'
        # A host with an output is configured: upgrading it asks nothing.
        elif [ "$OUTPUT_FROM" = 'ask' ]; then
            NAME_FROM='ask'
        fi
    fi
    readonly NAME_FROM OUTPUT_FROM CONFIG_HAS_OUTPUT

    WRITE_CONFIG='no'
    if [ "$CONFIG_EXISTS" != 'yes' ] || [ "$NAME_FROM" != 'none' ] || [ "$OUTPUT_FROM" != 'none' ]; then
        WRITE_CONFIG='yes'
    fi
    readonly WRITE_CONFIG
    readonly NEW_CONFIG="$WORK_DIR/sendspin-cli.conf"

    # install for a new file so its mode is not the caller's umask; cp keeps an existing one's.
    if [ "$CONFIG_EXISTS" = 'yes' ]; then
        WRITE_SYSTEM_CONFIG=(cp "$NEW_CONFIG" "$SYSTEM_CONFIG")
    else
        WRITE_SYSTEM_CONFIG=(install -m 0644 "$NEW_CONFIG" "$SYSTEM_CONFIG")
    fi
    readonly WRITE_SYSTEM_CONFIG

    NEED_LINGER='no'
    NEED_AUDIO='no'
    if [ "$MODE" = 'user' ]; then
        [ -e "/var/lib/systemd/linger/$TARGET_USER" ] || NEED_LINGER='yes'
        if getent group audio >/dev/null; then
            case " $(id -nG "$TARGET_USER") " in
                *' audio '*) ;;
                *) NEED_AUDIO='yes' ;;
            esac
        fi
    fi
    readonly NEED_LINGER NEED_AUDIO

    # What this is about to do as root

    step 'These are the commands that need root'
    say ''
    if [ "${#PKG_INSTALL[@]}" -gt 0 ]; then
        say "  ${SUDO_P}${PKG_INSTALL[*]}"
    fi
    say "  ${SUDO_P}tar -xzf $TARBALL --strip-components=1 -C / $PAYLOAD_ROOT/usr"
    if [ "$HAVE_SYSTEMD" != 'yes' ]; then
        if [ "$CONFIG_EXISTS" != 'yes' ]; then
            say "  ${SUDO_P}cp $CONFIG_EXAMPLE $SYSTEM_CONFIG"
        fi
    elif [ "$MODE" = 'system' ]; then
        if [ "$CREATE_USER" = 'yes' ]; then
            say "  ${SUDO_P}systemd-sysusers"
        fi
        say "  ${SUDO_P}systemctl daemon-reload"
        say "  ${SUDO_P}systemctl enable $UNIT"
        if [ "$CONFIG_HAS_OUTPUT" != 'yes' ] && [ "$OUTPUT_FROM" != 'flag' ]; then
            say "  ${SUDO_P}$BINARY -l"
        fi
        if [ "$WRITE_CONFIG" = 'yes' ]; then
            say "  ${SUDO_P}${WRITE_SYSTEM_CONFIG[*]}"
        fi
        if [ "$CONFIG_HAS_OUTPUT" = 'yes' ] || [ "$OUTPUT_FROM" != 'none' ]; then
            say "  ${SUDO_P}systemctl restart $UNIT"
        fi
    else
        say "  ${SUDO_P}systemctl daemon-reload"
        if [ "$SYSTEM_UNIT_ON" = 'yes' ]; then
            say "  ${SUDO_P}systemctl disable --now $UNIT"
        fi
        if [ "$NEED_LINGER" = 'yes' ]; then
            say "  ${SUDO_P}loginctl enable-linger $TARGET_USER"
        fi
        if [ "$NEED_AUDIO" = 'yes' ]; then
            say "  ${SUDO_P}usermod -aG audio $TARGET_USER"
        fi
    fi
    say ''
    if [ "${#PKG_INSTALL[@]}" -gt 0 ]; then
        say "The first line installs the libraries sendspin-cli links and this host lacks:"
        say "${MISSING_LIBS[*]}."
    fi
    say "Naming '$PAYLOAD_ROOT/usr' is what keeps the archive's BUILD-INFO.txt out of /."
    if [ "$CREATE_USER" = 'yes' ]; then
        say "'systemd-sysusers' creates the unprivileged '$SERVICE_USER' account the unit runs as,"
        say "from the declaration installed at $SYSUSERS_FILE."
    fi
    if [ "$HAVE_SYSTEMD" = 'yes' ] && [ "$MODE" = 'system' ]; then
        if [ "$WRITE_CONFIG" = 'yes' ]; then
            if [ "$CONFIG_EXISTS" = 'yes' ]; then
                say "$NEW_CONFIG will be your $SYSTEM_CONFIG with"
            else
                say "$NEW_CONFIG will be the installed example config with"
            fi
            case "$NAME_FROM:$OUTPUT_FROM" in
                ask:* | *:ask)
                    say 'the name and the output device you are asked for next filled in. The lines it'
                    say 'sets are printed before it is copied.'
                    ;;
                none:none) say 'nothing changed: every line in it is commented out.' ;;
                *) say 'the --name and --output you passed filled in.' ;;
            esac
        fi
        if [ "$CONFIG_HAS_OUTPUT" != 'yes' ] && [ "$OUTPUT_FROM" = 'none' ]; then
            say "No 'output' is set in $SYSTEM_CONFIG and none was passed, so the unit is enabled"
            say "but NOT started: a system unit has no session for ALSA's 'default' to follow, and"
            say 'it would fail and be retried every five seconds. The device list is printed instead.'
        fi
        if [ "$USER_UNIT_ON" = 'yes' ]; then
            say "Your user service is stopped and disabled first, since both listen on one port:"
            say "  ${USER_P}systemctl --user disable --now $UNIT"
        fi
    elif [ "$MODE" = 'user' ]; then
        say "The rest runs as $TARGET_USER, not root: the config at $CONFIG,"
        say "and '${USER_P}systemctl --user enable $UNIT' and 'restart'."
        if [ "$SYSTEM_UNIT_ON" = 'yes' ]; then
            say 'The system service is stopped and disabled, since both listen on one port.'
        fi
        if [ "$NEED_LINGER" = 'yes' ]; then
            say "'enable-linger' keeps your user services running while you are logged out."
        fi
        if [ "$NEED_AUDIO" = 'yes' ]; then
            say "The audio group is what opens a sound card directly (hw:...); it applies from your"
            say 'next login. PipeWire and PulseAudio outputs do not need it.'
        fi
    fi
    say 'Everything else this script does is reading.'

    if [ "$assume_yes" != 'yes' ]; then
        [ "$CAN_ASK" = 'yes' ] ||
            fail 'there is no terminal to confirm those commands on. Re-run with --yes if you
    have read them and want them run; nothing was installed'
        say ''
        if ! confirm 'Run them?'; then
            [ "${#PKG_INSTALL[@]}" -eq 0 ] || say "sendspin-cli will not load here without: ${MISSING_LIBS[*]}"
            fail 'nothing was installed'
        fi
    fi

    # Install

    if [ "${#PKG_INSTALL[@]}" -gt 0 ]; then
        step 'Installing the missing libraries'
        as_root "${PKG_INSTALL[@]}"
    fi

    step 'Installing'
    # Idempotent, so re-running upgrades.
    as_root tar -xzf "$TARBALL" --strip-components=1 -C / "$PAYLOAD_ROOT/usr"

    # A missing unit means a macOS archive; say so before the loader does.
    [ -f "$UNIT_FILE" ] ||
        fail "the payload installed no unit at $UNIT_FILE -- a macOS archive on a Linux host would
    look exactly like this. Take the $LEG one"

    say "  $BINARY"
    "$BINARY" --version | sed 's/^/  /'

    if [ "$HAVE_SYSTEMD" != 'yes' ]; then
        if [ "$CONFIG_EXISTS" != 'yes' ]; then
            as_root cp "$CONFIG_EXAMPLE" "$SYSTEM_CONFIG"
            say "  $SYSTEM_CONFIG  (from the installed example; everything in it is commented out)"
        fi
        step 'Not touching a service'
        [ -z "$want_name$want_output" ] ||
            say '  --name and --output configure the service, so they were not used.'
        say '  systemd is not running here, so there is nothing to enable. The binary is'
        say '  installed and runs in the foreground:'
        say ''
        say "    $BINARY -l                       # what this host can play through"
        say "    $BINARY -o hw:1,0 -n \"\$(hostname)\""
        exit 0
    fi

    step 'Setting the service up'

    if [ "$MODE" = 'system' ]; then
        # Before daemon-reload: the unit's User= needs the account.
        if [ "$CREATE_USER" = 'yes' ]; then
            command -v systemd-sysusers >/dev/null 2>&1 ||
                fail "the unit runs as '$SERVICE_USER' and 'systemd-sysusers' is not on \$PATH to
    create the account from $SYSUSERS_FILE. Create it with your own tooling instead --
    '${SUDO_P}useradd --system --no-create-home -G audio $SERVICE_USER' is the equivalent
    README documents -- then re-run this script"

            as_root systemd-sysusers

            # sysusers exits 0 even if it read nothing, so check the account exists.
            getent passwd "$SERVICE_USER" >/dev/null ||
                fail "'systemd-sysusers' ran and there is still no '$SERVICE_USER' account, so the unit
    would report 217/USER rather than starting. $SYSUSERS_FILE is what it should have read"

            say "  user:    $SERVICE_USER (unprivileged; the unit's User=)"
        else
            # No fragment in the payload: make sure the installed unit names no User= we cannot create.
            unit_user="$(sed -n 's/^[[:space:]]*User=[[:space:]]*//p' "$UNIT_FILE" | tail -n 1)"
            if [ -n "$unit_user" ] && ! getent passwd "$unit_user" >/dev/null; then
                fail "$UNIT_FILE runs as '$unit_user' and no such account exists, while this payload
    carried no sysusers declaration to create one from. Create it -- '${SUDO_P}useradd --system
    --no-create-home -G audio $unit_user' is the equivalent README documents -- then re-run
    this script"
            fi
        fi

        as_root systemctl daemon-reload
        if [ "$USER_UNIT_ON" = 'yes' ]; then
            as_user systemctl --user disable --now "$UNIT"
            say "  stopped: $TARGET_USER's user service, which would hold the same port"
        fi
        as_root systemctl enable "$UNIT"
        say "  enabled: $UNIT starts on boot"
    else
        as_root systemctl daemon-reload
        if [ "$SYSTEM_UNIT_ON" = 'yes' ]; then
            as_root systemctl disable --now "$UNIT"
            say '  stopped: the system service, which would hold the same port'
        fi
        if [ "$NEED_LINGER" = 'yes' ]; then
            as_root loginctl enable-linger "$TARGET_USER"
            say "  linger:  $TARGET_USER's services keep running after logout"
        fi
        if [ "$NEED_AUDIO" = 'yes' ]; then
            as_root usermod -aG audio "$TARGET_USER"
            say "  audio:   $TARGET_USER joins the audio group at the next login"
        fi

        # Linger starts the user manager; give it a moment to answer.
        tries=0
        until as_user systemctl --user daemon-reload 2>/dev/null; do
            tries=$((tries + 1))
            [ "$tries" -lt 10 ] ||
                fail "$TARGET_USER's systemd user manager is not answering, so there is nothing to
    enable the service in. Log in as $TARGET_USER once and re-run this script"
            sleep 1
        done

        own_unit="$USER_UNIT_DIR/$UNIT.service"
        if [ -f "$USER_UNIT_FILE" ]; then
            # Ours would shadow the unit the payload now ships.
            if is_own_unit "$own_unit"; then
                as_user rm -f "$own_unit"
            elif as_user test -e "$own_unit"; then
                say "  note:    $own_unit is yours and takes precedence over the installed unit"
            fi
        elif is_own_unit "$own_unit" || ! as_user test -e "$own_unit"; then
            write_own_user_unit
            say "  unit:    $own_unit (this release ships none of its own)"
        fi
        as_user systemctl --user daemon-reload
        as_user systemctl --user enable "$UNIT"
        say "  enabled: $UNIT starts with $TARGET_USER's user manager, at boot"
    fi

    # Name and output

    NAME=''
    OUTPUT=''
    case "$NAME_FROM" in
        flag) NAME=$want_name ;;
        ask)
            step 'Naming the player'
            say ''
            say '  This is what a Sendspin server shows it as.'
            say ''
            ask "  Name [$(hostname)]: " "$(hostname)"
            NAME=$REPLY
            ;;
    esac
    case "$OUTPUT_FROM" in
        flag) OUTPUT=$want_output ;;
        ask)
            show_devices
            say ''
            if [ "$MODE" = 'user' ]; then
                say "  Type the name of the device to play through. 'default' follows your session."
                say ''
                ask '  Output [default]: ' default
            else
                say "  Type the name of the device to play through, e.g. hw:1,0. 'default' usually"
                say '  fails under a system service. Leave it empty to decide later.'
                say ''
                ask '  Output: '
            fi
            OUTPUT=$REPLY
            ;;
    esac
    readonly NAME OUTPUT

    if [ "$WRITE_CONFIG" = 'yes' ]; then
        if [ "$CONFIG_EXISTS" = 'yes' ]; then
            as_owner cat "$CONFIG" >"$NEW_CONFIG"
        else
            cp "$CONFIG_EXAMPLE" "$NEW_CONFIG"
        fi
        step "Writing $CONFIG"
        say ''
        if [ -n "$NAME" ]; then
            set_key "$NEW_CONFIG" name "$NAME"
            say "  name = $NAME"
        fi
        if [ -n "$OUTPUT" ]; then
            set_key "$NEW_CONFIG" output "$OUTPUT"
            say "  output = $OUTPUT"
        fi
        if [ -z "$NAME$OUTPUT" ]; then
            say '  (the annotated example; everything in it is commented out)'
        fi
        if [ "$MODE" = 'user' ]; then
            as_user mkdir -p "$(dirname "$CONFIG")"
            as_user tee "$CONFIG" <"$NEW_CONFIG" >/dev/null
        else
            say ''
            say "  ${SUDO_P}${WRITE_SYSTEM_CONFIG[*]}"
            as_root "${WRITE_SYSTEM_CONFIG[@]}"
        fi
    fi

    # Start it, or say what is still owed

    STARTED='no'
    if [ "$CONFIG_HAS_OUTPUT" = 'yes' ] || [ -n "$OUTPUT" ]; then
        STARTED='yes'
        start_and_report
    elif [ "$OUTPUT_FROM" = 'none' ]; then
        show_devices
    fi
    readonly STARTED

    step 'Next'
    say ''
    if [ "$STARTED" != 'yes' ]; then
        say "  1. Pick a device from that list and put it in $CONFIG. Keys there are the"
        say '     long flag names without their dashes, and the file is annotated:'
        say ''
        say "       ${OWNER_P}nano $CONFIG        # output = hw:1,0"
        say ''
        say '  2. Start it:'
        say ''
        say "       $CTL_P start $UNIT"
        say ''
        say '  3. Then pair it, as below.'
        say ''
    fi
    say '  Pair it with your Sendspin server. This prints the token to paste into the server:'
    say ''
    if [ "$MODE" = 'user' ]; then
        say "       ${USER_P}$BINARY pair-token"
    else
        say "       ${SUDO_P}$BINARY pair-token --control-socket $CONTROL_SOCKET"
    fi
    say ''
    say '  The player advertises itself over mDNS, so the server finds it by itself; pairing is'
    say "  what lets it play. To dial a server instead, set 'server' in $CONFIG."
    say ''
    say '  Watch it:'
    say ''
    say "       $JOURNAL_P -f"
    say ''
    say '  Ask it what it is doing:'
    say ''
    if [ "$MODE" = 'user' ]; then
        say "       ${USER_P}$BINARY status"
    else
        say "       ${SUDO_P}$BINARY status --control-socket $CONTROL_SOCKET"
        say ''
        say "  The socket is mode 0600 and belongs to the '$SERVICE_USER' account, hence the sudo."
        say "  'control-socket = $CONTROL_SOCKET' in the config saves repeating the flag."
    fi

    if [ -n "$PI_MODEL" ]; then
        say ''
        say "  On this $PI_MODEL:"
        say ''
        say '  - The headphone jack and each HDMI output are separate cards. The device list'
        say "    names them; 'output = hw:X,Y' picks one, with X and Y the numbers it printed."
        if [ "$MODE" = 'system' ]; then
            say "  - 'output = default' is what usually leaves a system unit silent, because there"
            say '    is no user session for it to follow. Name a card.'
            say "  - The service runs as the unprivileged '$SERVICE_USER' account, which is in the"
            say '    audio group already, so it reaches /dev/snd with nothing for you to arrange.'
        fi
    fi

    say ''
    say "  To remove all of this again: https://github.com/$REPO#uninstall"
    say ''
}

# Braces, so a download cut short anywhere is a syntax error and runs nothing.
{ main "$@"; }
