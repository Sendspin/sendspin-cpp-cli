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
# Installs, runs and removes one leg's packages in clean distribution containers, and checks that
# Debian bookworm refuses the deb on its glibc floor. Containers have no booted systemd, so this is
# also the no-systemd path of the maintainer scripts.
#
# Usage: scripts/test_linux_packages.sh <leg> <version> <package-dir>
#
# DOCKER names the container tool (default docker); podman works too.

# shellcheck disable=SC2016 # the single-quoted scripts expand in the container
set -euo pipefail

[ "$#" -eq 3 ] || {
    printf 'usage: %s <leg> <version> <package-dir>\n' "$0" >&2
    exit 1
}

LEG=$1
VERSION=$2
DIR=$(cd "$3" && pwd)
readonly LEG VERSION DIR
readonly NAME="sendspin-cli-$VERSION-$LEG"
readonly DOCKER="${DOCKER:-docker}"

case "$LEG" in
    linux-x86_64 | linux-arm64) ;;
    *)
        echo "::error::$LEG packages cannot be installed natively on this runner"
        exit 1
        ;;
esac

# Shared by every distribution: run after install, and around removal.
readonly CHECKS='
fail() { echo "::error::$IMAGE: $*"; exit 1; }

check_installed() {
    /usr/bin/sendspin-cli --version || fail "the installed sendspin-cli does not run"
    id sendspin-cli || fail "postinstall did not create the sendspin-cli account"
    case " $(id -nG sendspin-cli) " in
        *" audio "*) ;;
        *) fail "sendspin-cli is not in the audio group" ;;
    esac
    grep -q "^ExecStart=/usr/bin/sendspin-cli " /usr/lib/systemd/system/sendspin-cli.service ||
        fail "the installed unit does not start /usr/bin/sendspin-cli"
    # Never enabled by the package.
    [ ! -e /etc/systemd/system/multi-user.target.wants/sendspin-cli.service ] ||
        fail "installing the package enabled the unit"
}

# Enabled offline, as in a chroot, so removal must clear the symlink without a booted systemd.
enable_unit() {
    systemctl enable sendspin-cli.service || fail "systemctl could not enable the installed unit offline"
    [ -L /etc/systemd/system/multi-user.target.wants/sendspin-cli.service ] ||
        fail "enabling the unit made no multi-user.target.wants symlink"
}

# Every non-directory path the package owned must be gone, and the enablement with them.
check_removed() {
    status=0
    while read -r path; do
        if [ -e "$path" ] && [ ! -d "$path" ]; then
            echo "::error::$IMAGE: $path is still there after removal"
            status=1
        fi
    done </tmp/owned
    [ "$status" -eq 0 ] || exit 1
    [ ! -e /usr/bin/sendspin-cli ] || fail "removal left /usr/bin/sendspin-cli"
    [ ! -L /etc/systemd/system/multi-user.target.wants/sendspin-cli.service ] ||
        fail "removal left the unit enabled"
}
'

in_container() {
    local image=$1 script=$2
    echo "::group::$image"
    "$DOCKER" run --rm -v "$DIR:/packages:ro" -e "IMAGE=$image" -e "NAME=$NAME" \
        -e DEBIAN_FRONTEND=noninteractive "$image" bash -euo pipefail -c "$CHECKS$script"
    echo '::endgroup::'
}

deb_install='
apt-get update -qq
apt-get install -y "/packages/$NAME.deb"
check_installed
dpkg -L sendspin-cli >/tmp/owned
enable_unit
apt-get remove -y sendspin-cli
check_removed
'
in_container debian:trixie "$deb_install"
in_container ubuntu:24.04 "$deb_install"

# bookworm has glibc 2.36, under the floor this build declares.
in_container debian:bookworm '
apt-get update -qq
status=0
apt-get install -y "/packages/$NAME.deb" >/tmp/install.log 2>&1 || status=$?
cat /tmp/install.log
[ "$status" -ne 0 ] || fail "apt installed a deb whose glibc floor bookworm does not meet"
grep -q "Depends: libc6 (>= " /tmp/install.log || fail "apt refused the deb, but not on its libc6 dependency"
[ ! -e /usr/bin/sendspin-cli ] || fail "a refused deb left /usr/bin/sendspin-cli behind"
'

in_container fedora:latest '
dnf install -y "/packages/$NAME.rpm"
check_installed
rpm -ql sendspin-cli >/tmp/owned
enable_unit
dnf remove -y sendspin-cli
check_removed
'

# The official Arch image is x86_64 only.
if [ "$LEG" = linux-x86_64 ]; then
    in_container archlinux:latest '
pacman -Sy --noconfirm
pacman -U --noconfirm "/packages/$NAME.pkg.tar.zst"
check_installed
pacman -Qlq sendspin-cli >/tmp/owned
enable_unit
pacman -R --noconfirm sendspin-cli
check_removed
'
fi
