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
# Installs packages for build.yml's linux-armv7 leg once the armhf apt source is set up, first aligning
# the Multi-Arch: same systemd libraries across both architectures, and retrying while the mirrors disagree.
#
# Usage: scripts/install_armhf_deps.sh <package ...>

set -euo pipefail

[ $# -gt 0 ] || {
  echo "usage: $0 <package ...>" >&2
  exit 2
}

export LC_ALL=C DEBIAN_FRONTEND=noninteractive

# The armhf copy of each must be the exact version of the host one.
readonly aligned=(libsystemd0 libudev1)
readonly attempts=3
readonly backoff=120
# Phased updates hold the host side back from the version ports already serves.
readonly apt_opts=(-o APT::Get::Always-Include-Phased-Updates=true)

host_arch="$(dpkg --print-architecture)"
readonly host_arch

# Usage: installed <package> [arch]; prints nothing when it is not installed.
installed() {
  dpkg-query -W -f='${db:Status-Abbrev}${Version}\n' "$1:${2:-$host_arch}" 2>/dev/null |
    sed -n 's/^ii *//p' || true
}

candidate() {
  apt-cache "${apt_opts[@]}" policy "$1" | awk '/^ *Candidate:/ { print $2 }'
}

# True if some source (or dpkg) offers version $2 of $1.
offers() {
  apt-cache -o APT::Cache::AllVersions=true show "$1" 2>/dev/null |
    awk -v v="$2" '/^Version:/ && $2 == v { found = 1 } END { exit !found }'
}

report() {
  echo "Installed $host_arch:"
  dpkg-query -W "${aligned[@]/%/:$host_arch}" || true
  echo 'armhf candidates:'
  apt-cache "${apt_opts[@]}" policy "${aligned[@]/%/:armhf}"
}

# Installs every aligned library on both arches at one version both sources carry.
align() {
  local pkg have want v targets=()
  for pkg in "${aligned[@]}"; do
    have="$(installed "$pkg")"
    want="$(candidate "$pkg:armhf")"
    if [ -z "$want" ] || [ "$want" = '(none)' ]; then
      echo "$pkg:armhf has no install candidate"
      return 1
    fi
    if [ -z "$have" ] || [ "$have" = "$want" ]; then
      continue
    elif dpkg --compare-versions "$want" gt "$have" && offers "$pkg:$host_arch" "$want"; then
      v="$want"
    elif offers "$pkg:armhf" "$have"; then
      v="$have"
    else
      echo "$pkg: no version both arches can reach yet ($host_arch $have installed, armhf candidate $want)"
      return 1
    fi
    echo "Aligning $pkg at $v (was $host_arch $have, armhf candidate $want)."
    targets+=("$pkg:$host_arch=$v" "$pkg:armhf=$v")
  done
  [ ${#targets[@]} -eq 0 ] ||
    sudo apt-get install --no-install-recommends -y "${apt_opts[@]}" "${targets[@]}"
}

# Explicit returns: errexit is off inside a function called from a condition.
attempt() {
  sudo apt-get update || return 1
  report
  align || return 1
  sudo apt-get install --no-install-recommends -y "${apt_opts[@]}" "$@"
}

for ((i = 1; ; i++)); do
  attempt "$@" && exit 0
  [ "$i" -lt "$attempts" ] || break
  wait_s=$((backoff * i))
  echo "::warning::armhf dependency install failed (attempt $i of $attempts); retrying in ${wait_s}s in case the mirrors converge"
  sleep "$wait_s"
done

skewed=0
for pkg in "${aligned[@]}"; do
  have="$(installed "$pkg")"
  want="$(installed "$pkg" armhf)"
  want="${want:-$(candidate "$pkg:armhf")}"
  [ "$have" != "$want" ] || continue
  skewed=1
  echo "::error::$pkg is $host_arch ${have:-(not installed)} but armhf ${want:-(none)}; Multi-Arch: same needs them equal"
done
[ "$skewed" = 1 ] ||
  echo "::error::armhf dependency install failed after $attempts attempts, with ${aligned[*]} matching across arches; see the apt output above"
exit 1
