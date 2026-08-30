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
# Builds sendspin-cli for ARMv6 inside an emulated Raspbian container, for the linux-armv6 leg
# of .github/workflows/build.yml. A Pi Zero, a Pi Zero W and an original Pi are ARM1176.
#
# Not a cross build, and that is not a preference. Debian and Ubuntu armhf are an ARMv7-A port,
# so a cross toolchain's own crt1.o, crtbegin.o and every member of libgcc.a are ARMv7 and are
# linked into the binary whatever -march said -- which is why scripts/build_arm32.sh refuses
# armv6 by name. Raspbian's really are ARMv6, its gcc being configured --with-arch=armv6
# --with-float=hard, so this passes no -march at all; and Raspbian publishes no cross toolchain.
# Emulating the whole build is therefore the only route to an ARMv6 archive rather than the
# slower of two, which is the opposite of the trade the linux-armv7 leg makes.
#
# A script rather than lines of YAML for the reason scripts/build_arm32.sh and
# scripts/build_macos_pkg.sh are: the archive this leg publishes is the only build of this
# project a Pi Zero owner will ever run, and a developer has to be able to reproduce it without
# a runner -- which takes the image digest, --init, the uid and QEMU_CPU together, and a
# workflow file is not somewhere anybody can run. It is also what puts them under ci.yml's
# `shellcheck scripts/*.sh` job.
#
# Four verbs where build_arm32.sh has none, and the difference is what each owns. A cross build
# is a *configuration*: once cmake has the flags, `cmake --build` over the directory is an
# ordinary build and there is nothing left to remember. A container is a *location*, so it
# colours every step after configure too -- the suite, the smoke test, the install that stages
# the archive -- and each of those needs a way in.
#
# What this needs on the host:
#
#   docker
#   a registered binfmt handler for 32-bit ARM, which .github/workflows/build.yml installs with
#     docker/setup-qemu-action -- without it the container starts and nothing in it can run
#
# Usage: scripts/build_armv6_container.sh start <image>
#        scripts/build_armv6_container.sh configure <build-dir> [cmake option ...]
#        scripts/build_armv6_container.sh run <command> [argument ...]
#        scripts/build_armv6_container.sh stop
#
#   start      pull <image>, start the container over the current directory, and prove it runs
#              ARMv6. <image> is pinned by digest by the caller
#   configure  configure <build-dir> in the container; remaining arguments reach cmake verbatim,
#              which is how the caller keeps owning the options that are not about this target
#   run        run one command in the container, in the same directory, as the same user
#   stop       remove the container, reporting rather than failing when there is none. A runner
#              is discarded whole, so this is for a developer's own machine, which is not -- and
#              for the workflow to call with `if: always()` without a failed run failing twice

set -euo pipefail

fail() {
    printf 'build_armv6_container: FAIL: %s\n' "$*" >&2
    exit 1
}

# One container per checkout is enough -- the legs of a matrix run on runners of their own, and a
# developer builds one thing at a time -- so the name is a constant rather than an argument to
# thread through every verb.
readonly CONTAINER='sendspin-cli-armv6'

# Written rather than left to the image's own environment, which happens to set it too. The
# emulator defaults to a Cortex-A15-class core and will execute ARMv7 instructions perfectly
# happily, which would make it *more* permissive than the hardware this archive is for: an
# illegal instruction would surface on a Pi Zero rather than in the suite. Everything this
# script runs gets it, not just the tests, because a cmake try_run probe asking what the CPU can
# do runs at configure time and would otherwise be answered for the wrong CPU.
readonly QEMU_CPU_MODEL='arm1176'

# A home the build owns, so that anything writing under $HOME does not meet the image's root-owned
# one and fail on EACCES for a reason that has nothing to do with this build.
readonly BUILD_HOME='/home/build'

command -v docker >/dev/null 2>&1 ||
    fail "docker is not on \$PATH, and this runs the build inside a container"

[ "$#" -ge 1 ] ||
    fail "usage: $0 <start|configure|run|stop> [argument ...]"

VERB=$1
shift
readonly VERB

running() {
    [ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null)" = 'true' ]
}

# The workspace is bind-mounted at the path it already has rather than at some /work of its own,
# which is what lets every absolute path survive the boundary: FETCHCONTENT_BASE_DIR and DESTDIR
# are handed in by the caller, and CMakeCache.txt records the build directory it was configured
# in. A container mounting it elsewhere would configure at one path and build at another.
in_container() {
    running || fail "no running $CONTAINER container -- run '$0 start <image>' first"

    local env_args=(
        --env "HOME=$BUILD_HOME"
        --env "QEMU_CPU=$QEMU_CPU_MODEL"
    )

    # Forwarded explicitly because `docker exec` inherits nothing from the caller. This one is
    # workflow-level `env:` in build.yml, so losing it silently would leave an already emulated
    # build compiling one translation unit at a time.
    if [ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]; then
        env_args+=(--env "CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL")
    fi

    # As the invoking user rather than as root, and both halves matter. Three StateStore cases
    # assert that an unwritable directory is refused, which root is refused nothing by -- they
    # fail as root for a reason that is not about the code. And everything written into the
    # bind-mounted workspace has to be owned by the user the runner's own steps read it back as,
    # the cache save among them.
    docker exec \
        --user "$(id -u):$(id -g)" \
        --workdir "$PWD" \
        "${env_args[@]}" \
        "$CONTAINER" \
        "$@"
}

case "$VERB" in
    start)
        [ "$#" -eq 1 ] || fail "usage: $0 start <image>"
        IMAGE=$1
        readonly IMAGE

        # Pulled as its own command so that an unreachable registry or a digest that no longer
        # resolves says so, rather than surfacing as a failure to start a container.
        #
        # --platform is spelled out even though the digest names a single-architecture manifest,
        # because without it docker reports the mismatch against the host as a warning on every
        # command and a warning nobody can act on is noise in a log that is read when something
        # is wrong.
        docker pull --platform linux/arm/v6 "$IMAGE"

        # Removed rather than reused: a container left by an earlier run holds that run's
        # packages and that run's user, and a build that quietly inherits them is not the build
        # this describes. `|| true` is safe here because the run below fails on a name clash.
        docker rm --force "$CONTAINER" >/dev/null 2>&1 || true

        # --init is load-bearing, not hygiene. Without a reaping PID 1 a daemon that has already
        # exited stays a zombie, `kill -0` keeps answering for it, and scripts/smoke_test.sh
        # reports a player that outlived SIGTERM -- a failure that looks like ARMv6 and is not.
        #
        # --entrypoint because the image's own is balena's device-provisioning script, which has
        # nothing to answer for in a build container. `docker exec` bypasses an entrypoint
        # anyway, so this only settles what the container holds open -- but settling it is worth
        # a flag when the alternative is a build that depends on what that script does next.
        docker run \
            --detach \
            --init \
            --platform linux/arm/v6 \
            --name "$CONTAINER" \
            --volume "$PWD:$PWD" \
            --workdir "$PWD" \
            --entrypoint sleep \
            "$IMAGE" \
            infinity >/dev/null

        # Proven here, where the message can name the cause, rather than left to surface as a
        # `docker exec` complaining that the container is not running. `docker run` succeeds
        # whether or not the kernel can execute what it started: with no handler registered the
        # `sleep` above is an ARM binary that cannot exec, and the container is gone by the time
        # anything else looks at it. That is by far the likeliest way this fails.
        running || {
            docker logs "$CONTAINER" 2>&1 || true
            fail "the container exited as soon as it started. The usual cause is no binfmt
    handler for 32-bit ARM: .github/workflows/build.yml registers one with
    docker/setup-qemu-action, and on a developer's own machine 'docker run --privileged --rm
    tonistiigi/binfmt --install arm' does the same"
        }

        # A passwd entry at the invoking user's own id, so that in_container() above has a user
        # to be. Reused where the image already carries one at that id rather than replaced:
        # what this needs is an entry and a home, not a particular name.
        docker exec "$CONTAINER" sh -c '
            set -eu
            getent group "$2" >/dev/null || groupadd --gid "$2" build
            getent passwd "$1" >/dev/null ||
                useradd --uid "$1" --gid "$2" --home-dir "$3" --no-create-home build
            install -d -o "$1" -g "$2" -m 0755 "$3"
        ' sh "$(id -u)" "$(id -g)" "$BUILD_HOME"

        # git and ca-certificates for FetchContent, which clones what the cache did not restore;
        # pkg-config because it is what finds PortAudio, PipeWire and PulseAudio, so a change in
        # the image should fail here rather than produce a player with no audio backend.
        docker exec "$CONTAINER" sh -c '
            set -eu
            apt-get update
            apt-get install --no-install-recommends -y \
                build-essential \
                cmake \
                git \
                ca-certificates \
                pkg-config \
                libasound2-dev \
                portaudio19-dev \
                libpulse-dev \
                libpipewire-0.3-dev \
                libavahi-compat-libdnssd-dev
        '

        # Proven here, where the message can name the cause, rather than left to surface further
        # down wearing cmake's name. Two facts carry this whole leg, and neither is a flag: that
        # the container really executes ARM under the emulator, and that its compiler targets
        # ARMv6 by configuration -- which is why nothing here passes -march.
        machine="$(in_container uname -m)"
        echo 'What the container reports:'
        printf '%s\n' "$machine"
        in_container gcc --version
        in_container cmake --version

        [ "$machine" = 'armv6l' ] ||
            fail "the container reports '$machine', not armv6l -- either the emulator is not
    registered for 32-bit ARM or this image is not the Raspbian armhf one"

        # gcc -v writes its configuration to stderr.
        gcc_config="$(in_container gcc -v 2>&1)"
        case "$gcc_config" in
            *--with-arch=armv6*) ;;
            *)
                printf '%s\n' "$gcc_config" >&2
                fail "this image's gcc is not configured --with-arch=armv6, so it would build
    ARMv7 objects with an ARMv7 libgcc beside them -- which is the whole reason this leg is not
    a cross build"
                ;;
        esac

        printf 'build_armv6_container: %s is up, building as %s:%s\n' \
            "$CONTAINER" "$(id -u)" "$(id -g)"
        ;;

    configure)
        [ "$#" -ge 1 ] || fail "usage: $0 configure <build-dir> [cmake option ...]"
        BUILD_DIR=$1
        shift
        readonly BUILD_DIR

        # Two facts about this toolchain, both of them properties of the machine and its
        # compiler rather than of this project -- which is why they belong here, for the reason
        # build_arm32.sh owns the armv7 -march rather than leaving it to the caller.
        #
        # -latomic: ARMv6 has no LDREXD, so every 64-bit atomic becomes a libatomic call -- from
        # src/pulse_sink.cpp, src/player_listener.cpp and sendspin-cpp's own
        # connection_manager.cpp -- and the link fails on __atomic_load_8 without it. The ARMv7
        # leg needs nothing of the kind because ARMv7 has LDREXD. It goes in
        # CMAKE_CXX_STANDARD_LIBRARIES rather than CMAKE_EXE_LINKER_FLAGS, which places it ahead
        # of the objects that reference it and leaves the linker to discard it as unused.
        #
        # -Wno-error=restrict: Raspbian's gcc 12.2.0 has a -Wrestrict false positive on
        # `line += " " + std::to_string(...)`, at src/control_common.cpp:391 and :401 and at
        # tests/cli_test.cpp:1133 -- the third site 32-bit-specific, the warning turning on a
        # memcpy size the compiler folds differently where size_t is `unsigned int`. This
        # demotes that one diagnostic and leaves -Werror standing over everything else, so an
        # ARMv6-only warning nobody has met yet still fails the leg.
        #
        # The `-Wno-error=` form is what makes that possible, and it is not interchangeable with
        # `-Wno-restrict`. CMAKE_CXX_FLAGS lands before the `-Wall -Wextra -Wpedantic -Werror`
        # CMakeLists.txt adds with target_compile_options, and a later -Wall turns a warning
        # disabled earlier back on -- where a later blanket -Werror does not re-promote a
        # diagnostic an earlier -Wno-error= has already exempted.
        in_container cmake -B "$BUILD_DIR" \
            -DCMAKE_CXX_STANDARD_LIBRARIES=-latomic \
            -DCMAKE_CXX_FLAGS=-Wno-error=restrict \
            "$@"

        printf 'build_armv6_container: configured %s for armv6 in %s\n' "$BUILD_DIR" "$CONTAINER"
        ;;

    run)
        [ "$#" -ge 1 ] || fail "usage: $0 run <command> [argument ...]"
        in_container "$@"
        ;;

    stop)
        [ "$#" -eq 0 ] || fail "usage: $0 stop"

        # Nothing to remove is reported rather than refused. This is the one verb a caller runs
        # unconditionally to tidy up -- .github/workflows/build.yml runs it with `if: always()`
        # -- so a run that failed before the container existed must not fail again here, wearing
        # a message about the wrong thing.
        if docker inspect "$CONTAINER" >/dev/null 2>&1; then
            docker rm --force "$CONTAINER" >/dev/null
            printf 'build_armv6_container: removed %s\n' "$CONTAINER"
        else
            printf 'build_armv6_container: no %s container to remove\n' "$CONTAINER"
        fi
        ;;

    *)
        fail "unknown verb '$VERB' -- this takes start, configure, run or stop"
        ;;
esac
