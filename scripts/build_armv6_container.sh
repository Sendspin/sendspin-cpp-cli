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
# Builds sendspin-cli for ARMv6 in an emulated Raspbian container, for build-armv6.yml.
# Not a cross build: Debian armhf's libgcc and crt objects are ARMv7. Host needs docker and an ARM binfmt handler.
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

readonly CONTAINER='sendspin-cli-armv6'

# Pinned so the emulator refuses ARMv7 instructions, at configure-time probes as well as in tests.
readonly QEMU_CPU_MODEL='arm1176'

# A build-owned $HOME, so writes there do not hit the image's root-owned one.
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

# The workspace mounts at its own path, so absolute paths recorded in the build stay valid.
in_container() {
    running || fail "no running $CONTAINER container -- run '$0 start <image>' first"

    local env_args=(
        --env "HOME=$BUILD_HOME"
        --env "QEMU_CPU=$QEMU_CPU_MODEL"
    )

    # docker exec inherits nothing from the caller.
    if [ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]; then
        env_args+=(--env "CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL")
    fi

    # As the invoking user: StateStore tests need a non-root user, and the runner reads outputs back.
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

        # Pulled separately so a registry failure says so; --platform silences a mismatch warning.
        docker pull --platform linux/arm/v6 "$IMAGE"

        # Never reuse a previous run's container.
        docker rm --force "$CONTAINER" >/dev/null 2>&1 || true

        # --init reaps exited daemons, or smoke_test.sh sees zombies as live; --entrypoint skips balena's.
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

        # docker run succeeds even when binfmt cannot exec ARM, so check the container is running.
        running || {
            docker logs "$CONTAINER" 2>&1 || true
            fail "the container exited as soon as it started. The usual cause is no binfmt
    handler for 32-bit ARM: .github/workflows/build-armv6.yml registers one with
    docker/setup-qemu-action, and on a developer's own machine 'docker run --privileged --rm
    tonistiigi/binfmt --install arm' does the same"
        }

        # A passwd entry at the invoking uid, reusing one the image already has.
        docker exec "$CONTAINER" sh -c '
            set -eu
            getent group "$2" >/dev/null || groupadd --gid "$2" build
            getent passwd "$1" >/dev/null ||
                useradd --uid "$1" --gid "$2" --home-dir "$3" --no-create-home build
            install -d -o "$1" -g "$2" -m 0755 "$3"
        ' sh "$(id -u)" "$(id -g)" "$BUILD_HOME"

        # git/ca-certificates for FetchContent, pkg-config to find the audio backends.
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

        # The container must really run ARM, and its compiler must target ARMv6 by default.
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

        # -latomic in CMAKE_CXX_STANDARD_LIBRARIES: ARMv6 lacks LDREXD, and linker flags would place it too early.
        # -Wno-error=restrict (not -Wno-restrict): works around a gcc 12.2 false positive while a later -Wall stays on.
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

        # Nothing to remove is not an error: this runs under if: always().
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
