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
# Configures a 32-bit ARM cross build of sendspin-cli against the host's armhf multiarch tree,
# for the linux-armv7 leg of .github/workflows/build.yml. It configures only: `cmake --build`
# over the directory it writes is an ordinary build with no cross-specific argument to
# remember, so there is nothing here for a second entry point to own.
#
# A script rather than lines of YAML for the reason scripts/build_macos_pkg.sh is one: the
# archive this leg publishes is the only build of this project most 32-bit Raspberry Pi owners
# will ever run, and a developer has to be able to reproduce it without a runner. It is also
# what puts the cross flags under ci.yml's `shellcheck scripts/*.sh` job.
#
# The target is an argument rather than a CMake toolchain file per architecture, so that a
# second 32-bit target is a case label here instead of a second file free to drift from this
# one.
#
# What this needs on the host, which .github/workflows/build.yml installs:
#
#   dpkg --add-architecture armhf, with an apt source that carries it -- armhf is a port, so
#     archive.ubuntu.com does not
#   crossbuild-essential-armhf, for the arm-linux-gnueabihf compilers
#   the :armhf build dependencies, so there is something for ALSA, PortAudio, PulseAudio,
#     PipeWire and dns_sd to be found in
#   qemu-user-static with binfmt registration, because gtest_discover_tests runs the test
#     binary at build time to enumerate its cases
#
# Usage: scripts/build_arm32.sh <armv6|armv7> <build-dir> [cmake option ...]
#
#   armv6|armv7  the target architecture. armv7 is what builds; armv6 is refused, and the case
#                label below says why rather than leaving it to look unsupported
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
        # A Pi 2, a Pi 3, a Pi 4 or a Pi Zero 2 running a 32-bit userland -- and any other
        # ARMv7-A machine, which is what the archive's name promises and so what these flags
        # have to hold to.
        #
        # -mfpu is the armhf ABI's own baseline rather than the Cortex-A7's NEON and VFPv4.
        # `armv7l` says nothing about either: a Cortex-A8 or a Cortex-A9 is ARMv7-A with VFPv3
        # and NEON that is optional, so a binary built for the Pi's FPU would meet an
        # instruction those machines do not have. The decoders this links are fixed-point, so
        # the baseline costs nothing on the path that matters.
        #
        # -mfloat-abi is spelled out even though the triplet above implies it, because build.yml
        # asserts the hard-float EABI off the finished binary and an assertion is worth more
        # against a declared fact than against an implied one.
        ARCH_FLAGS=(-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard)
        ;;
    armv6)
        # Refused rather than quietly built, because what comes out is not what it says. Debian
        # and Ubuntu armhf are an armv7-a port, and that is where this toolchain's own
        # crt1.o, crtbegin.o and every member of libgcc.a come from -- all of them armv7, all of
        # them linked into the binary. Our objects would be armv6 and the archive would not be,
        # and the merged Tag_CPU_arch build.yml reads back says so.
        #
        # An armv6 build needs a Raspberry Pi OS armhf sysroot, which carries an armv6 libgcc
        # and armv6 startup objects. docs/ROADMAP.md item 12 records it as owed.
        fail "armv6 cannot be built against a Debian/Ubuntu armhf toolchain: its libgcc and
    startup objects are armv7-a, so the result would trap on an ARM1176 (a Pi Zero, a Pi Zero W
    or an original Pi). That target needs a Raspberry Pi OS sysroot; see docs/ROADMAP.md item 12"
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

# The multiarch library directory, which is the whole sysroot this build has: the :armhf
# packages put their libraries here and their headers in the shared /usr/include, and Debian's
# cross compilers are configured to look in both. Checked rather than assumed, because its
# absence is what an unenabled `dpkg --add-architecture armhf` looks like from here -- and the
# configure that followed would find no backend at all and still succeed.
readonly ARMHF_LIBDIR="/usr/lib/$TRIPLE"
[ -d "$ARMHF_LIBDIR" ] ||
    fail "no $ARMHF_LIBDIR -- enable armhf multiarch and install the :armhf build dependencies:
    sudo dpkg --add-architecture armhf"

# Nothing this script runs needs an emulator; `cmake --build` over the directory it writes
# does, because gtest_discover_tests executes the freshly built test binary to enumerate its
# cases. Proven here, where the message can name the cause, rather than left to surface as a
# build failure two commands later.
PROBE_DIR="$(mktemp -d)"
readonly PROBE_DIR
trap 'rm -rf "$PROBE_DIR"' EXIT
printf 'int main(void) { return 0; }\n' >"$PROBE_DIR/probe.c"
"$TRIPLE-gcc" "${ARCH_FLAGS[@]}" -o "$PROBE_DIR/probe" "$PROBE_DIR/probe.c"
"$PROBE_DIR/probe" ||
    fail "this host will not execute a 32-bit ARM binary. Install qemu-user-static and register
    its handlers -- 'sudo systemctl restart systemd-binfmt' on a systemd host -- or the build
    over $BUILD_DIR will fail where gtest_discover_tests runs the test binary"

# LIBDIR rather than PATH, because LIBDIR *replaces* pkg-config's default search path where PATH
# only prepends to it. This is what stops a host .pc file answering for PortAudio, PulseAudio or
# PipeWire and handing the link line a library of the wrong architecture. /usr/share/pkgconfig
# stays, holding the .pc files that are architecture-independent by definition.
export PKG_CONFIG_LIBDIR="$ARMHF_LIBDIR/pkgconfig:/usr/share/pkgconfig"

# The flags go in CMAKE_C_FLAGS/CMAKE_CXX_FLAGS, which reach every target in the build, rather
# than onto sendspin-cli's own targets the way CMakeLists.txt scopes its warning flags. The
# asymmetry is deliberate: warnings are a standard held over the code that is ours to keep
# clean, while the instruction set is a property of the machine that every object in the link
# has to agree on. Each dependency here arrives through FetchContent and so through
# add_subdirectory, in the one cmake invocation, and a flag scoped to our targets alone would
# leave the decoders and ixwebsocket compiling for whatever the compiler's default is -- and
# leave a probe like micro_opus's for what the CPU can do reading the wrong answer. build.yml
# reads the merged build attributes back off the linked binary, which is what turns this from a
# claim into a check.
#
# CMAKE_LIBRARY_ARCHITECTURE is what points find_package(ALSA), find_library(dns_sd) and
# find_path(dns_sd.h) at the armhf tree. Without it CMake searches the host's own multiarch
# directory, and a find_library that answers with an x86_64 libdns_sd.so configures cleanly,
# reports mDNS as found, and links nothing usable. build.yml pins its expect_mdns assertion to a
# path under this directory for exactly that reason.
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
