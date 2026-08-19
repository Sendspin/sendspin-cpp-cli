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
# Installs a released sendspin-cli on a Linux host and sets its systemd unit up.
#
# One script for every Linux host, a Raspberry Pi included, because a Pi *is* an arm64
# Linux box here: it takes the same `linux-arm64` archive an arm64 server takes and
# installs it the same way. What is genuinely Pi-specific is advice -- the `audio` group,
# and that the headphone jack and HDMI are separate cards -- and that is printed at the end
# when a Pi is what this is running on. A second script would have been this one with two
# paragraphs changed, and the two would have drifted.
#
# The archive is GitHub's, verified against the release's own SHA256SUMS, and unpacked with
# the member-selected `tar` form README documents -- naming `<name>/usr` is what leaves
# BUILD-INFO.txt in the archive instead of writing it to `/`.
#
# WHAT IT DELIBERATELY DOES NOT DO IS START A PLAYER THAT CANNOT PLAY. A systemd *system*
# unit has no user session, so there is no PipeWire or PulseAudio for ALSA's `default` PCM
# to follow, and the device that opens fine from your shell usually will not open under
# `systemctl` -- README's "The systemd unit" says to expect exactly that. The unit's
# `Restart=on-failure`/`RestartSec=5` would then retry it every five seconds, forever, while
# this script printed congratulations. So the unit is *enabled* but only *started* once an
# `output` has been chosen: with one already in /etc/sendspin-cli.conf this installs and
# restarts, and without one it stops after listing the host's devices and prints the two
# commands that finish the job. That is this repo's refuse-rather-than-limp habit -- the same
# reason a bad `-s` port stops the player instead of dialling the default.
#
# Nothing here runs as root without saying so first: every privileged command is printed in
# full, and then either confirmed at a terminal or authorised up front with --yes.
#
# One asymmetry worth naming, in the spirit of the one .github/workflows/ci.yml names about
# itself: the `shellcheck` job there lints every script under scripts/, and the other two are
# also *run* on every build -- smoke_test.sh on each publishing leg, build_macos_pkg.sh on the
# macOS one. This is the first script CI lints but never executes. A CI leg for it would want
# a runner willing to take a payload into `/` and a sound card to then not find, so what it
# has instead is the container run recorded in the pull request that added it.
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

# The repository releases are taken from. A constant rather than an environment knob: a
# get-started script that can be pointed at any repository is a get-started script that can
# be pointed at somebody else's binary.
readonly REPO='chrisuthe/sendspin-cpp-cli'

readonly UNIT='sendspin-cli'
readonly UNIT_FILE='/usr/local/lib/systemd/system/sendspin-cli.service'
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

# ==============================================================================
# What was asked for
# ==============================================================================

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
            # The usage block above, printed rather than restated -- a second copy is one to
            # forget. Everything from the `# Usage:` line to the end of the header.
            sed -n '/^# Usage:/,/^$/ s/^#\{1,2\} \{0,1\}//p' "$0"
            exit 0
            ;;
        *)
            fail "unknown argument '$1' -- see --help"
            ;;
    esac
done

readonly VERSION_TAG ASSUME_YES

# ==============================================================================
# Is this a host this can work on at all
# ==============================================================================

[ "$(uname -s)" = 'Linux' ] ||
    fail "Linux only -- this installs a systemd unit. On macOS take the installer .pkg from
    https://github.com/$REPO/releases instead"

for tool in tar sed grep; do
    command -v "$tool" >/dev/null 2>&1 ||
        fail "'$tool' is not on \$PATH, and this cannot install anything without it"
done

# The release archives are named for the CI leg that built them rather than for `uname -m`,
# so both spellings of 64-bit ARM map onto the one leg that exists.
MACHINE="$(uname -m)"
case "$MACHINE" in
    x86_64 | amd64)
        LEG='linux-x86_64'
        ;;
    aarch64 | arm64)
        LEG='linux-arm64'
        ;;
    armv6l | armv7l | armhf | arm)
        # The single most common way a Pi install goes wrong, so it gets the whole answer
        # rather than "unsupported architecture". There is no 32-bit build to fall back to --
        # docs/ROADMAP.md item 12 records that the matrix has no armv7 or 32-bit Pi leg -- and
        # the fix is a 64-bit OS on hardware that is almost certainly already 64-bit capable.
        fail "this is a 32-bit ARM userland ($MACHINE), and the builds are arm64 only.
    docs/ROADMAP.md item 12 records why: the CI matrix has no armv7 or 32-bit Pi leg, so no
    such archive exists to install. A Raspberry Pi 3 or newer is 64-bit hardware, so the fix
    is a 64-bit OS: install Raspberry Pi OS (64-bit), or 'Raspberry Pi OS Lite (64-bit)' for
    a headless player, and check with 'uname -m' after -- it must say aarch64. A Pi Zero (the
    original), a Pi 1 or a Pi 2 cannot run 64-bit at all, and needs a build from source"
        ;;
    *)
        fail "no release is built for '$MACHINE' -- the archives are linux-x86_64 and
    linux-arm64. Build from source instead: https://github.com/$REPO#build"
        ;;
esac
readonly MACHINE LEG

# systemd being *booted* rather than merely installed, which is what decides whether there is
# anything to enable. A container or a chroot without it still gets the binary.
HAVE_SYSTEMD='no'
if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    HAVE_SYSTEMD='yes'
fi
readonly HAVE_SYSTEMD

# A Pi answers here and nothing else does. `/proc/device-tree/model` is a NUL-terminated
# string out of the device tree, so the NUL is stripped rather than carried into a message.
PI_MODEL=''
if [ -r /proc/device-tree/model ]; then
    model="$(tr -d '\0' </proc/device-tree/model)"
    case "$model" in
        *'Raspberry Pi'*) PI_MODEL=$model ;;
    esac
fi
readonly PI_MODEL

# Everything privileged goes through here, so there is exactly one place that decides how root
# is reached and exactly one place that could ever run something unannounced.
SUDO=''
if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 ||
        fail 'this needs root to install into /usr/local and to drive systemctl, and there is
    no sudo here -- re-run it as root'
    SUDO='sudo'
fi
readonly SUDO

# What a printed command line is prefixed with: `sudo ` where one is needed, and nothing at
# all where this is already root -- an empty $SUDO expanded inline leaves every printed
# command indented one space further than the last, and a sentence ending "that needs ."
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
say "  host:         Linux $MACHINE${PI_MODEL:+  ($PI_MODEL)}"
say "  release leg:  $LEG"
if [ "$HAVE_SYSTEMD" = 'yes' ]; then
    say '  systemd:      yes'
else
    say '  systemd:      no -- the unit will be installed but nothing started'
fi

# ==============================================================================
# The payload
# ==============================================================================

# The tag of the newest release, on stdout, or a diagnostic naming which of the ways this can
# come back empty it was.
#
# Read off the `/releases/latest` redirect rather than out of the JSON API, which matters for
# three reasons. It needs no `jq`, absent on a fresh Raspberry Pi OS Lite. It is not subject
# to the API's 60-an-hour unauthenticated rate limit, which is per source address and so is
# shared by everything behind one NAT. And it is the only form that can tell "this repository
# has published no releases" -- which redirects to the releases index -- apart from "there is
# no such repository", which is a 404: the API answers 404 to both.
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

    # curl folds the header name to lower case for `-w`, but the raw header file keeps
    # whatever the server sent, so the match is case-insensitive.
    location="$(grep -i '^location:' "$headers" | tail -n 1 | tr -d '\r' | awk '{print $2}')"
    [ -n "$location" ] ||
        fail "github.com answered $status with no Location header, which should not happen"

    case "$location" in
        */releases/tag/*)
            printf '%s\n' "${location##*/releases/tag/}"
            ;;
        *)
            # The state this repository is actually in at the time of writing, so it gets a
            # real answer rather than a shrug.
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
    # Made absolute before anything else, because `tar -C /` below would otherwise resolve a
    # relative path against `/`.
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
        # Taken as given rather than looked up: a tag that does not exist fails at the
        # download below, naming the file it could not find, which is the same answer one
        # round trip earlier would have given.
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

    # Asserted rather than assumed, in the way build.yml asserts the payload's own file list:
    # `--ignore-missing` below skips a listed file that is absent, so a SHA256SUMS that never
    # mentions this archive at all would leave `sha256sum` with nothing to check. It does exit
    # non-zero when that leaves it with no file verified -- but "the checksum file does not
    # cover this download" deserves to be said in those words rather than as `-c` failing.
    grep -q "  $ARCHIVE\$" "$WORK_DIR/SHA256SUMS" ||
        fail "the SHA256SUMS published with $TAG does not list $ARCHIVE, so there is no
    checksum to verify it against. Nothing has been installed"

    # --ignore-missing because SHA256SUMS covers every archive the release carries and this
    # host has taken one of them.
    (cd "$WORK_DIR" && sha256sum --ignore-missing -c SHA256SUMS) ||
        fail "$ARCHIVE does not match the checksum $TAG publishes for it. Nothing has been
    installed. Download it again; if it fails a second time, say so on the issue tracker
    rather than installing it anyway"

    TARBALL="$WORK_DIR/$ARCHIVE"
fi
readonly TARBALL

# ==============================================================================
# What this is about to do as root
# ==============================================================================

# Read off the archive rather than derived from its filename: a payload staged by hand is
# named whatever its author called it, and the member `tar` is asked to extract has to be one
# that is really in there.
#
# Listed once into a variable, and every question below asked of *that* rather than of a
# second `tar`. Not tidiness: `tar -tzf … | grep -q` is a pipeline whose reader exits on the
# first match, which leaves tar killed by SIGPIPE -- and under `set -o pipefail` that is a
# failed check on an archive that is perfectly fine.
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

# An `output` already chosen is what decides whether the player is started at the end, so it
# is settled here -- before anything is installed -- and reported in the plan below.
#
# /etc/sendspin-cli.conf alone, and not the two $HOME paths that also come first in the search
# order: this is about the *system* unit, whose process gets whatever HOME systemd hands root,
# and guessing at that would be worse than naming the one file the unit's own documentation
# tells an operator to edit. A line is a comment when its first non-blank character is `#`, so
# an indented `#output = …` is correctly not a match.
CONFIG_HAS_OUTPUT='no'
if [ -f "$CONFIG" ] && grep -Eq '^[[:space:]]*output[[:space:]]*=' "$CONFIG"; then
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
    read -r answer
    case "$answer" in
        y | Y | yes | YES) ;;
        *) fail 'nothing was installed' ;;
    esac
fi

# ==============================================================================
# Install
# ==============================================================================

step 'Installing'
# Idempotent by construction: this overwrites whatever is at those paths, so re-running the
# script is how you upgrade. `--strip-components=1` drops the archive's own top level, so
# every remaining path is the path the file installs to.
as_root tar -xzf "$TARBALL" --strip-components=1 -C / "$PAYLOAD_ROOT/usr"
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

[ -f "$UNIT_FILE" ] ||
    fail "the payload installed no unit at $UNIT_FILE -- a macOS archive on a Linux host
    would look exactly like this. Take the $LEG one"

step 'Setting the service up'
as_root systemctl daemon-reload
as_root systemctl enable "$UNIT"
say "  enabled: $UNIT starts on boot"

# ==============================================================================
# Start it, or say what is still owed
# ==============================================================================

if [ "$CONFIG_HAS_OUTPUT" = 'yes' ]; then
    # `restart` and not `start`: this is also the upgrade path, and an already-running player
    # would otherwise keep serving the binary that has just been replaced underneath it.
    as_root systemctl restart "$UNIT"

    # Asked once rather than polled: the unit is Type=simple, so `restart` returns before
    # systemd has decided anything, and a device that will not open takes about a second to
    # say so.
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
        journalctl -u "$UNIT" --no-pager -n 15 2>/dev/null | sed 's/^/    /' ||
            say "    (could not read the journal; try: journalctl -u $UNIT -n 50)"
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

# ==============================================================================
# What to do next
# ==============================================================================

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
say "     The socket is mode 0600 and the service is root's, so reading it needs to be"
say "     root too. Put 'control-socket = $CONTROL_SOCKET' in the config to"
say '     stop repeating the flag.'
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
    say '  - The service runs as root, which already has /dev/snd. If you change that with a'
    say "    'systemctl edit $UNIT' drop-in, the new user needs the audio group in the same"
    say "    drop-in ('SupplementaryGroups=audio') -- without it the player starts and opens"
    say '    nothing.'
    say '  - To run it from your own shell rather than as a service, put yourself in that'
    say "    group once and log back in:  ${SUDO_P}usermod -aG audio \"\$USER\""
fi

say ''
