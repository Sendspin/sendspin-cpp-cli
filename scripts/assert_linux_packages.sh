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
# Asserts each package build_linux_packages.sh made for a leg: exact files and modes, name, version,
# architecture and dependencies, with the glibc floor read off the packaged binary.
# Needs dpkg-deb, rpm, readelf and tar with zstd.
#
# Usage: scripts/assert_linux_packages.sh <leg> <version> <package-dir>

set -euo pipefail

[ "$#" -eq 3 ] || {
    printf 'usage: %s <leg> <version> <package-dir>\n' "$0" >&2
    exit 1
}

LEG=$1
VERSION=$2
DIR=$3
readonly LEG VERSION DIR
readonly NAME="sendspin-cli-$VERSION-$LEG"

status=0
error() {
    echo "::error::$*"
    status=1
}

# Each line is what a package must declare; spelled out apart from packaging/nfpm.yaml on purpose.
deb_extra=''
case "$LEG" in
    linux-x86_64) deb_arch=amd64 rpm_arch=x86_64 arch_arch=x86_64 ;;
    linux-arm64) deb_arch=arm64 rpm_arch=aarch64 arch_arch=aarch64 ;;
    linux-armv7) deb_arch=armhf rpm_arch=armv7hl arch_arch=armv7h ;;
    linux-armv6) deb_arch=armhf rpm_arch='' arch_arch=armv6h deb_extra=', libatomic1' ;;
    *)
        echo "::error::no expected packages are recorded for leg '$LEG'"
        exit 1
        ;;
esac

WORK=$(mktemp -d)
readonly WORK
trap 'rm -rf "$WORK"' EXIT

# The set of package files, so a stray or missing one fails too.
{
    echo "$NAME.deb"
    [ -z "$rpm_arch" ] || echo "$NAME.rpm"
    echo "$NAME.pkg.tar.zst"
} | sort >"$WORK/expected-set"
(cd "$DIR" && find . -maxdepth 1 ! -type d | sed 's|^\./||') | sort >"$WORK/actual-set"
diff -u "$WORK/expected-set" "$WORK/actual-set" || error "$DIR does not hold exactly this leg's packages"

# Files only, as "<mode> <path>": directories are left to the distribution.
cat >"$WORK/expected-files" <<'FILES'
-rw-r--r-- /etc/sendspin-cli.conf
-rw-r--r-- /usr/lib/systemd/system/sendspin-cli.service
-rw-r--r-- /usr/lib/systemd/user/sendspin-cli.service
-rw-r--r-- /usr/lib/sysusers.d/sendspin-cli.conf
-rw-r--r-- /usr/share/doc/sendspin-cli/LICENSE
-rw-r--r-- /usr/share/doc/sendspin-cli/README.md
-rw-r--r-- /usr/share/doc/sendspin-cli/contributors.md
-rw-r--r-- /usr/share/doc/sendspin-cli/sendspin-cli.conf.example
-rwxr-xr-x /usr/bin/sendspin-cli
FILES
sort -o "$WORK/expected-files" "$WORK/expected-files"

check_files() {
    local what=$1
    sort -o "$WORK/actual-files" "$WORK/actual-files"
    echo "What the $what holds:"
    cat "$WORK/actual-files"
    diff -u "$WORK/expected-files" "$WORK/actual-files" ||
        error "the $what is not the file list this script expects"
}

check_field() {
    [ "$2" = "$3" ] || error "$1 is '$2', not '$3'"
}

# Every package carries the same binary, and the glibc floor is read off it here, not taken from nfpm.yaml.
floor_of() {
    readelf --version-info "$1" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -uV | tail -1 | sed 's/^GLIBC_//'
}

# deb
deb="$DIR/$NAME.deb"
dpkg-deb --fsys-tarfile "$deb" | tar -tvf - | awk '$1 !~ /^d/ { sub(/^\./, "", $6); print $1, $6 }' >"$WORK/actual-files"
check_files "$NAME.deb"
mkdir "$WORK/deb"
dpkg-deb -x "$deb" "$WORK/deb"
binary="$WORK/deb/usr/bin/sendspin-cli"
floor=$(floor_of "$binary")
[ -n "$floor" ] || error "no GLIBC_ symbol version could be read off the packaged binary"
echo "The packaged binary needs glibc $floor."

grep -q '^ExecStart=/usr/bin/sendspin-cli ' "$WORK/deb/usr/lib/systemd/system/sendspin-cli.service" ||
    error 'the packaged system unit does not start /usr/bin/sendspin-cli'
grep -qx 'ExecStart=/usr/bin/sendspin-cli' "$WORK/deb/usr/lib/systemd/user/sendspin-cli.service" ||
    error 'the packaged user unit does not start /usr/bin/sendspin-cli'

echo "What $NAME.deb declares:"
dpkg-deb -f "$deb"
check_field 'the deb Package' "$(dpkg-deb -f "$deb" Package)" sendspin-cli
check_field 'the deb Version' "$(dpkg-deb -f "$deb" Version)" "$VERSION-1"
check_field 'the deb Architecture' "$(dpkg-deb -f "$deb" Architecture)" "$deb_arch"
check_field 'the deb Depends' "$(dpkg-deb -f "$deb" Depends)" \
    "libc6 (>= $floor), libstdc++6, libgcc-s1, libasound2t64 | libasound2, libportaudio2, libpulse0, libpipewire-0.3-0t64 | libpipewire-0.3-0, libavahi-compat-libdnssd1$deb_extra, systemd | systemd-standalone-sysusers"
check_field 'the deb Recommends' "$(dpkg-deb -f "$deb" Recommends)" avahi-daemon
check_field 'the deb conffiles' "$(dpkg-deb -I "$deb" conffiles | paste -sd,)" /etc/sendspin-cli.conf

# rpm
if [ -n "$rpm_arch" ]; then
    rpm="$DIR/$NAME.rpm"
    rpm -qp --qf '[%{FILEMODES:perms} %{FILENAMES}\n]' "$rpm" | awk '$1 !~ /^d/' >"$WORK/actual-files"
    check_files "$NAME.rpm"
    mkdir "$WORK/rpm"
    rpm2archive - <"$rpm" | tar -xzf - -C "$WORK/rpm" ./usr/bin/sendspin-cli
    cmp -s "$binary" "$WORK/rpm/usr/bin/sendspin-cli" || error "$NAME.rpm carries a different binary from $NAME.deb"

    check_field 'the rpm name, version and arch' \
        "$(rpm -qp --qf '%{NAME} %{VERSION}-%{RELEASE} %{ARCH}' "$rpm")" "sendspin-cli $VERSION-1 $rpm_arch"
    # rpmlib() capabilities are rpm's own, not dependencies.
    rpm -qp --requires "$rpm" | grep -v '^rpmlib(' >"$WORK/rpm-requires"
    echo "What $NAME.rpm requires:"
    cat "$WORK/rpm-requires"
    check_field 'the rpm Requires' "$(paste -sd, "$WORK/rpm-requires")" \
        "glibc >= $floor,libstdc++,alsa-lib,portaudio,pulseaudio-libs,pipewire-libs,avahi-compat-libdns_sd,systemd"
    check_field 'the rpm Recommends' "$(rpm -qp --recommends "$rpm" | paste -sd,)" avahi
    # c is %config, n is noreplace.
    check_field 'the rpm config files' \
        "$(rpm -qp --qf '[%{FILEFLAGS:fflags} %{FILENAMES}\n]' "$rpm" | awk 'NF == 2 && $1 ~ /c/' | paste -sd,)" \
        'cn /etc/sendspin-cli.conf'
fi

# Arch
pkg="$DIR/$NAME.pkg.tar.zst"
tar --zstd -tvf "$pkg" | awk '$1 !~ /^d/ && $6 !~ /^\.(PKGINFO|MTREE|INSTALL)$/ { print $1, "/" $6 }' >"$WORK/actual-files"
check_files "$NAME.pkg.tar.zst"
mkdir "$WORK/arch"
tar --zstd -xf "$pkg" -C "$WORK/arch" .PKGINFO .INSTALL usr/bin/sendspin-cli
cmp -s "$binary" "$WORK/arch/usr/bin/sendspin-cli" || error "$NAME.pkg.tar.zst carries a different binary from $NAME.deb"

echo "What $NAME.pkg.tar.zst declares:"
cat "$WORK/arch/.PKGINFO"
pkginfo() {
    sed -n "s/^$1 = //p" "$WORK/arch/.PKGINFO" | paste -sd,
}
check_field 'the Arch pkgname' "$(pkginfo pkgname)" sendspin-cli
check_field 'the Arch pkgver' "$(pkginfo pkgver)" "$VERSION-1"
check_field 'the Arch arch' "$(pkginfo arch)" "$arch_arch"
check_field 'the Arch depends' "$(pkginfo depend)" \
    "glibc>=$floor,gcc-libs,alsa-lib,portaudio,libpulse,libpipewire,avahi,systemd"
check_field 'the Arch backup' "$(pkginfo backup)" etc/sendspin-cli.conf
for hook in post_install post_upgrade pre_remove post_remove; do
    grep -Eq "^(function )?$hook\(\)" "$WORK/arch/.INSTALL" || error "the Arch .INSTALL defines no $hook()"
done

exit "$status"
