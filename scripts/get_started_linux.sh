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
# Installs a released sendspin-cli on a Linux host and enables its systemd unit.
# Verifies SHA256SUMS, creates the service account, and starts the player only once an `output` is configured.
# Every root command is printed first and confirmed, or pre-authorised with --yes.
#
# Usage: scripts/get_started_linux.sh [--version <tag>] [--yes]
#
#   --version <tag>  install this release instead of the latest, e.g. --version v0.1.0
#   --yes            do not prompt before the commands that need root. Required when stdin
#                    is not a terminal, since there is nobody there to ask
#
# Environment:
#
#   SENDSPIN_CLI_TARBALL  install this payload instead of downloading a release. The escape
#                         hatch for a locally built archive -- `DESTDIR` staged and tarred,
#                         exactly as CI publishes -- and the only way to exercise this script
#                         before a release exists. It is NOT checksummed, and the script says
#                         so loudly rather than letting a skipped integrity check pass for a
#                         successful one.

set -euo pipefail

# Fixed so this script can never install someone else's binary.
readonly REPO='Sendspin/sendspin-cpp-cli'

readonly UNIT='sendspin-cli'
readonly UNIT_FILE='/usr/local/lib/systemd/system/sendspin-cli.service'
readonly SYSUSERS_FILE='/usr/local/lib/sysusers.d/sendspin-cli.conf'
readonly SERVICE_USER='sendspin-cli'
readonly BINARY='/usr/local/bin/sendspin-cli'
readonly CONFIG='/etc/sendspin-cli.conf'
readonly CONFIG_EXAMPLE='/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example'
readonly CONTROL_SOCKET='/run/sendspin-cli/control.sock'

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

# What was asked for

VERSION_TAG=''
ASSUME_YES='no'

while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)
            [ "$#" -ge 2 ] || fail '--version needs a tag, e.g. --version v0.1.0'
            VERSION_TAG=$2
            shift 2
            ;;
        --yes | -y)
            ASSUME_YES='yes'
            shift
            ;;
        -h | --help)
            # Prints the header's usage block.
            sed -n '/^# Usage:/,/^$/ s/^#\{1,2\} \{0,1\}//p' "$0"
            exit 0
            ;;
        *)
            fail "unknown argument '$1' -- see --help"
            ;;
    esac
done

readonly VERSION_TAG ASSUME_YES

# Is this a host this can work on at all

[ "$(uname -s)" = 'Linux' ] ||
    fail "Linux only -- this installs a systemd unit. On macOS take the installer .pkg from
    https://github.com/$REPO/releases instead"

for tool in tar sed grep; do
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

as_root() {
    if [ -n "$SUDO" ]; then
        "$SUDO" "$@"
    else
        "$@"
    fi
}

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR
trap 'rm -rf "$WORK_DIR"' EXIT

say 'sendspin-cli getting started'
say "  host:         Linux $MACHINE, $USERLAND userland${PI_MODEL:+  ($PI_MODEL)}"
say "  release leg:  $LEG"
if [ "$HAVE_SYSTEMD" = 'yes' ]; then
    say '  systemd:      yes'
else
    say '  systemd:      no -- the unit will be installed but nothing started'
fi

# The payload

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
        SENDSPIN_CLI_TARBALL=/tmp/sendspin-cli.tar.gz $0 --yes"
            ;;
    esac
}

if [ -n "${SENDSPIN_CLI_TARBALL:-}" ]; then
    step 'Using the payload you supplied'
    [ -f "$SENDSPIN_CLI_TARBALL" ] ||
        fail "SENDSPIN_CLI_TARBALL names '$SENDSPIN_CLI_TARBALL', which is not a file"
    # Absolute, since tar -C / would resolve a relative path against /.
    TARBALL="$(cd "$(dirname "$SENDSPIN_CLI_TARBALL")" && pwd)/$(basename "$SENDSPIN_CLI_TARBALL")"
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

    if [ -n "$VERSION_TAG" ]; then
        TAG=$VERSION_TAG
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

# What this is about to do as root

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

# Only where systemd can use the account.
CREATE_USER='no'
if [ "$HAVE_SYSTEMD" = 'yes' ] && [ "$PAYLOAD_HAS_SYSUSERS" = 'yes' ]; then
    CREATE_USER='yes'
fi
readonly CREATE_USER

# An `output` in /etc/sendspin-cli.conf decides whether the player starts at the end.
# Read as root: the file may be unreadable, and grep's exit 2 would look like "no output".
CONFIG_HAS_OUTPUT='no'
if as_root test -f "$CONFIG" &&
    as_root grep -Eq '^[[:space:]]*output[[:space:]]*=' "$CONFIG"; then
    CONFIG_HAS_OUTPUT='yes'
fi
readonly CONFIG_HAS_OUTPUT

SEED_CONFIG='no'
if [ ! -e "$CONFIG" ]; then
    SEED_CONFIG='yes'
fi
readonly SEED_CONFIG

step 'These are the commands that need root'
say ''
say "  ${SUDO_P}tar -xzf $TARBALL --strip-components=1 -C / $PAYLOAD_ROOT/usr"
if [ "$SEED_CONFIG" = 'yes' ]; then
    say "  ${SUDO_P}cp $CONFIG_EXAMPLE $CONFIG"
fi
if [ "$CREATE_USER" = 'yes' ]; then
    say "  ${SUDO_P}systemd-sysusers"
fi
if [ "$HAVE_SYSTEMD" = 'yes' ]; then
    say "  ${SUDO_P}systemctl daemon-reload"
    say "  ${SUDO_P}systemctl enable $UNIT"
    if [ "$CONFIG_HAS_OUTPUT" = 'yes' ]; then
        say "  ${SUDO_P}systemctl restart $UNIT"
    else
        say "  ${SUDO_P}$BINARY -l"
    fi
fi
say ''
say "Naming '$PAYLOAD_ROOT/usr' is what keeps the archive's BUILD-INFO.txt out of /."
if [ "$CREATE_USER" = 'yes' ]; then
    say "'systemd-sysusers' creates the unprivileged '$SERVICE_USER' account the unit runs as,"
    say "reading the declaration the line above it installs at $SYSUSERS_FILE."
    say 'It adds nothing else and is idempotent. Without it the unit does not start at all.'
fi
if [ "$SEED_CONFIG" = 'yes' ]; then
    say "There is no $CONFIG yet, so the installed example is copied there for you to edit."
    say 'Every line in it is commented out, so it chooses nothing on its own.'
fi
if [ "$HAVE_SYSTEMD" = 'yes' ] && [ "$CONFIG_HAS_OUTPUT" != 'yes' ]; then
    say "No 'output' is set in $CONFIG, so the unit is enabled but NOT started: under"
    say "systemd there is no PipeWire for ALSA's 'default' to follow, and starting it now"
    say 'would usually mean a player failing and being retried every five seconds. The'
    say 'device list is printed instead, and starting it is the last thing you do.'
fi
say 'Everything else this script does is reading.'

if [ "$ASSUME_YES" != 'yes' ]; then
    [ -t 0 ] ||
        fail 'stdin is not a terminal, so there is nobody to confirm those commands with.
    Re-run with --yes if you have read them and want them run'
    printf '\nRun them? [y/N] '
    # `|| fail`: set -e would otherwise exit silently on a closed stdin.
    read -r answer || fail 'stdin closed before an answer arrived; nothing was installed'
    case "$answer" in
        y | Y | yes | YES) ;;
        *) fail 'nothing was installed' ;;
    esac
fi

# Install

step 'Installing'
# Idempotent, so re-running upgrades.
as_root tar -xzf "$TARBALL" --strip-components=1 -C / "$PAYLOAD_ROOT/usr"

# A missing unit means a macOS archive; say so before the loader does.
[ -f "$UNIT_FILE" ] ||
    fail "the payload installed no unit at $UNIT_FILE -- a macOS archive on a Linux host would
    look exactly like this. Take the $LEG one"

say "  $BINARY"
"$BINARY" --version | sed 's/^/  /'

if [ "$SEED_CONFIG" = 'yes' ]; then
    as_root cp "$CONFIG_EXAMPLE" "$CONFIG"
    say "  $CONFIG  (from the installed example; everything in it is commented out)"
fi

if [ "$HAVE_SYSTEMD" != 'yes' ]; then
    step 'Not touching a service'
    say '  systemd is not running here, so there is nothing to enable. The binary is'
    say '  installed and runs in the foreground:'
    say ''
    say "    $BINARY -l                       # what this host can play through"
    say "    $BINARY -o hw:1,0 -n \"\$(hostname)\""
    exit 0
fi

step 'Setting the service up'

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
as_root systemctl enable "$UNIT"
say "  enabled: $UNIT starts on boot"

# Start it, or say what is still owed

if [ "$CONFIG_HAS_OUTPUT" = 'yes' ]; then
    # restart, not start, so an upgrade replaces the running binary.
    as_root systemctl restart "$UNIT"

    # Type=simple returns immediately; a failing device takes about a second to show.
    sleep 2
    if systemctl is-active --quiet "$UNIT"; then
        step "$UNIT is running"
        say "  output = $(sed -n 's/^[[:space:]]*output[[:space:]]*=[[:space:]]*//p' "$CONFIG" | tail -n 1)"
    else
        step "$UNIT was started and is NOT running"
        say ''
        say "  $CONFIG names an output, so this is that device failing to open rather"
        say "  than the usual first-install case. What it said:"
        say ''
        # Empty output counts as failure: users outside systemd-journal get nothing and exit 0.
        journal="$(as_root journalctl -u "$UNIT" --no-pager -n 15 2>/dev/null || true)"
        if [ -n "$journal" ]; then
            printf '%s\n' "$journal" | sed 's/^/    /'
        else
            say "    (nothing readable in the journal; try: ${SUDO_P}journalctl -u $UNIT -n 50)"
        fi
        say ''
        say "  '${SUDO_P}$BINARY -l' lists what this host really has."
    fi
else
    step 'What this host can play through'
    say ''
    say '  (ALSA and PortAudio narrate their own enumeration on stderr -- a "jack server is'
    say "  not running\" here is those libraries talking, not this player failing.)"
    say ''
    as_root "$BINARY" -l 2>&1 | sed 's/^/  /'
fi

# What to do next

step 'Next'
if [ "$CONFIG_HAS_OUTPUT" != 'yes' ]; then
    say ''
    say "  1. Pick a device from that list and put it in $CONFIG. Keys there are the"
    say '     long flag names without their dashes, and the file is annotated:'
    say ''
    say "       ${SUDO_P}nano $CONFIG        # output = hw:1,0"
    say ''
    say '  2. Start it:'
    say ''
    say "       ${SUDO_P}systemctl start $UNIT"
    say "       systemctl status $UNIT"
    say ''
    say '  3. Then:'
else
    say ''
fi
say ''
say '     Watch it:'
say ''
say "       journalctl -u $UNIT -f"
say ''
say '     Ask it what it is doing:'
say ''
say "       ${SUDO_P}$BINARY status --control-socket $CONTROL_SOCKET"
say ''
say "     The socket is mode 0600 and belongs to the '$SERVICE_USER' account the service"
say '     runs as, so reading it means being root -- which is not subject to the mode.'
say "     Put 'control-socket = $CONTROL_SOCKET' in the config to stop"
say '     repeating the flag.'
say ''
say '     Nothing else. The player advertises itself over mDNS and waits for a Sendspin'
say '     server to find it, so it should appear in your controller once it is playing'
say "     through a device. To dial a server instead, set 'server' in the config."

if [ -n "$PI_MODEL" ]; then
    say ''
    say "  On this $PI_MODEL:"
    say ''
    say '  - The headphone jack and each HDMI output are separate cards. The list above'
    say "    names them; 'output = hw:X,Y' picks one, with X and Y the numbers it printed."
    say "  - 'output = default' is what usually leaves a system unit silent, because there"
    say '    is no user session for it to follow. Name a card.'
    say "  - The service runs as the unprivileged '$SERVICE_USER' account, which the"
    say '    declaration that created it also put in the audio group -- so it reaches /dev/snd'
    say '    (root:audio 0660) with nothing for you to arrange.'
    say '  - To run it from your own shell rather than as a service, put yourself in that'
    say "    group once and log back in:  ${SUDO_P}usermod -aG audio \"\$USER\""
fi

say ''
