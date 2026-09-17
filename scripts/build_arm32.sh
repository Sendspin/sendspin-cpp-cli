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
# Configures the 32-bit ARM cross build for build.yml's linux-armv7 leg; build it with plain `cmake --build`.
# Host needs: an armhf multiarch apt source, crossbuild-essential-armhf, :armhf build deps, qemu-user-static binfmt.
#
# Usage: scripts/build_arm32.sh <armv6|armv7> <build-dir> [cmake option ...]
#
#   armv6|armv7  the target architecture. armv7 is what builds; armv6 is refused, and the case
#                label below says why and where it is built instead
#   build-dir    the directory to configure into, as `cmake -B` takes it
#   cmake option every remaining argument, passed through to cmake verbatim -- which is how the
#                caller keeps owning the options that have nothing to do with cross-compiling

set -euo pipefail

fail() {
    printf 'build_arm32: FAIL: %s\n' "$*" >&2
    exit 1
}

[ "$(uname -s)" = 'Linux' ] ||
    fail 'Linux only -- this cross-compiles against the host distribution armhf multiarch tree'

[ "$#" -ge 2 ] ||
    fail "usage: $0 <armv6|armv7> <build-dir> [cmake option ...]"

TARGET=$1
BUILD_DIR=$2
shift 2
readonly TARGET BUILD_DIR

readonly TRIPLE='arm-linux-gnueabihf'

case "$TARGET" in
    armv7)
        # ARMv7-A with the armhf baseline FPU, not the Pi's NEON/VFPv4, so every ARMv7-A board runs it.
        # -mfloat-abi is explicit because build.yml asserts it off the binary.
        ARCH_FLAGS=(-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard)
        ;;
    armv6)
        # Debian/Ubuntu armhf libgcc and crt objects are armv7; armv6 is built in build-armv6.yml.
        fail "armv6 cannot be built against a Debian/Ubuntu armhf toolchain: its libgcc and
    startup objects are armv7-a, so the result would trap on an ARM1176 (a Pi Zero, a Pi Zero W
    or an original Pi). armv6 is built a different way -- natively inside an emulated Raspbian
    container, which has an armv6 libgcc; see .github/workflows/build-armv6.yml"
        ;;
    *)
        fail "unknown target '$TARGET' -- this builds armv7"
        ;;
esac
readonly -a ARCH_FLAGS

for tool in "$TRIPLE-gcc" "$TRIPLE-g++" cmake; do
    command -v "$tool" >/dev/null 2>&1 ||
        fail "'$tool' is not on \$PATH -- install crossbuild-essential-armhf and cmake"
done

# The armhf multiarch dir; missing means `dpkg --add-architecture armhf` was never run.
readonly ARMHF_LIBDIR="/usr/lib/$TRIPLE"
[ -d "$ARMHF_LIBDIR" ] ||
    fail "no $ARMHF_LIBDIR -- enable armhf multiarch and install the :armhf build dependencies:
    sudo dpkg --add-architecture armhf"

# gtest_discover_tests runs the built test binary, so an emulator must be registered.
PROBE_DIR="$(mktemp -d)"
readonly PROBE_DIR
trap 'rm -rf "$PROBE_DIR"' EXIT
printf 'int main(void) { return 0; }\n' >"$PROBE_DIR/probe.c"
"$TRIPLE-gcc" "${ARCH_FLAGS[@]}" -o "$PROBE_DIR/probe" "$PROBE_DIR/probe.c"
"$PROBE_DIR/probe" ||
    fail "this host will not execute a 32-bit ARM binary. Install qemu-user-static and register
    its handlers -- 'sudo systemctl restart systemd-binfmt' on a systemd host -- or the build
    over $BUILD_DIR will fail where gtest_discover_tests runs the test binary"

# LIBDIR replaces pkg-config's search path, so no host .pc file answers for an armhf library.
export PKG_CONFIG_LIBDIR="$ARMHF_LIBDIR/pkgconfig:/usr/share/pkgconfig"

# Arch flags go to every target, FetchContent'd dependencies included, since the whole link must agree.
# CMAKE_LIBRARY_ARCHITECTURE points find_package/find_library at the armhf tree, not the host's.
cmake -B "$BUILD_DIR" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_C_COMPILER="$TRIPLE-gcc" \
    -DCMAKE_CXX_COMPILER="$TRIPLE-g++" \
    -DCMAKE_C_FLAGS="${ARCH_FLAGS[*]}" \
    -DCMAKE_CXX_FLAGS="${ARCH_FLAGS[*]}" \
    -DCMAKE_LIBRARY_ARCHITECTURE="$TRIPLE" \
    "$@"

printf 'build_arm32: configured %s for %s (%s)\n' "$BUILD_DIR" "$TARGET" "${ARCH_FLAGS[*]}"
