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
# Builds the deb, rpm and Arch packages for one Linux leg with a pinned nfpm, from packaging/nfpm.yaml.
#
# Usage: scripts/build_linux_packages.sh <leg> <version> <stage-dir> <out-dir>
#
#   <stage-dir>  a `cmake --install` DESTDIR of a build configured with CMAKE_INSTALL_PREFIX=/usr
#   <out-dir>    receives sendspin-cli-<version>-<leg>.{deb,rpm,pkg.tar.zst}; the names go to stdout

set -euo pipefail

fail() {
    printf 'build_linux_packages: FAIL: %s\n' "$*" >&2
    exit 1
}

readonly NFPM_VERSION='2.48.0'

[ "$#" -eq 4 ] || fail "usage: $0 <leg> <version> <stage-dir> <out-dir>"

LEG=$1
VERSION=$2
STAGE=$(cd "$3" && pwd)
OUT_DIR=$4
readonly LEG VERSION STAGE

PACKAGING=$(cd "$(dirname "$0")/../packaging" && pwd)
readonly PACKAGING

# deb, rpm and Arch spellings of each leg's architecture; '-' skips that format.
LIBATOMIC=no
case "$LEG" in
    linux-x86_64) DEB_ARCH=amd64 RPM_ARCH=x86_64 ARCH_ARCH=x86_64 ;;
    linux-arm64) DEB_ARCH=arm64 RPM_ARCH=aarch64 ARCH_ARCH=aarch64 ;;
    linux-armv7) DEB_ARCH=armhf RPM_ARCH=armv7hl ARCH_ARCH=armv7h ;;
    # Raspberry Pi OS armhf is ARMv6, so the deb is armhf; no rpm distribution targets ARMv6.
    linux-armv6)
        DEB_ARCH=armhf RPM_ARCH=- ARCH_ARCH=armv6h
        LIBATOMIC=yes
        ;;
    *) fail "no package architectures are recorded for leg '$LEG'" ;;
esac

binary="$STAGE/usr/bin/sendspin-cli"
[ -x "$binary" ] || fail "$binary is missing or not executable -- stage with CMAKE_INSTALL_PREFIX=/usr"
grep -q '^ExecStart=/usr/bin/sendspin-cli ' "$STAGE/usr/lib/systemd/system/sendspin-cli.service" ||
    fail "the staged system unit does not start /usr/bin/sendspin-cli -- the stage was not configured for /usr"

GLIBC_FLOOR=$(readelf --version-info "$binary" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -uV | tail -1)
GLIBC_FLOOR=${GLIBC_FLOOR#GLIBC_}
[ -n "$GLIBC_FLOOR" ] || fail "no GLIBC_ symbol version could be read off $binary"

case "$(uname -m)" in
    x86_64) nfpm_platform=x86_64 nfpm_sha256=6d7f9a28b2f397934403950fcbba83f598b08d02edbe79a0815b3b4897341750 ;;
    aarch64) nfpm_platform=arm64 nfpm_sha256=a9a75213763783b7669719e505474c748c5ad7dc66591a195619deb50f85c2f4 ;;
    *) fail "no pinned nfpm checksum for a $(uname -m) host" ;;
esac

TOOLS=$(mktemp -d)
readonly TOOLS
trap 'rm -rf "$TOOLS"' EXIT

tarball="nfpm_${NFPM_VERSION}_Linux_${nfpm_platform}.tar.gz"
curl -fsSL -o "$TOOLS/$tarball" \
    "https://github.com/goreleaser/nfpm/releases/download/v$NFPM_VERSION/$tarball"
printf '%s  %s\n' "$nfpm_sha256" "$TOOLS/$tarball" | sha256sum -c --quiet - ||
    fail "$tarball does not match its pinned checksum"
tar -xzf "$TOOLS/$tarball" -C "$TOOLS" nfpm

# nfpm resolves stage/ and packaging/ against its working directory.
WORK="$TOOLS/work"
mkdir -p "$WORK"
ln -s "$STAGE" "$WORK/stage"
ln -s "$PACKAGING" "$WORK/packaging"
sed -e "s|@VERSION@|$VERSION|g" \
    -e "s|@GLIBC_FLOOR@|$GLIBC_FLOOR|g" \
    -e "s|@DEB_ARCH@|$DEB_ARCH|g" \
    -e "s|@RPM_ARCH@|$RPM_ARCH|g" \
    -e "s|@ARCH_ARCH@|$ARCH_ARCH|g" \
    "$PACKAGING/nfpm.yaml" >"$WORK/nfpm.yaml"
if [ "$LIBATOMIC" = no ]; then
    sed -i '/^      - libatomic1$/d' "$WORK/nfpm.yaml"
fi
! grep -n '@[A-Z_]*@' "$WORK/nfpm.yaml" || fail "packaging/nfpm.yaml has a placeholder this script does not fill"

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
name="sendspin-cli-$VERSION-$LEG"
for format in deb:deb rpm:rpm archlinux:pkg.tar.zst; do
    packager=${format%%:*}
    extension=${format#*:}
    if [ "$packager" = rpm ] && [ "$RPM_ARCH" = - ]; then
        continue
    fi
    (cd "$WORK" && "$TOOLS/nfpm" package --config nfpm.yaml --packager "$packager" \
        --target "$OUT_DIR/$name.$extension") >&2
    printf '%s\n' "$name.$extension"
done

printf 'build_linux_packages: %s, glibc >= %s\n' "$LEG" "$GLIBC_FLOOR" >&2
