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
# Wraps a staged install payload -- the DESTDIR tree that
# `cmake --install build --component sendspin-cli` writes -- into a macOS installer .pkg.
#
# The payload is the input rather than something built here, so the installer ships exactly
# the files the install() rules do and cannot drift from the tarball CI publishes beside it.
# Nothing is compiled, nothing is fetched, and nothing is installed: given a staged tree this
# runs offline in a second.
#
# Why a .pkg exists at all, since a tarball already ships these files: `xcrun stapler` staples
# a notarization ticket to a .app, a .dmg or a .pkg and refuses a bare Mach-O. Notarizing the
# loose binary would therefore still leave an offline Mac asking Apple on first run. This
# archive is the shape that makes a ticket stapleable, and so the artifact a Developer ID
# signature will attach to.
#
# What this deliberately does NOT do is sign or notarize -- see docs/ROADMAP.md item 10, which
# records that both are gated on an enrolment that has not happened. The .pkg written here is
# unsigned, and its welcome pane says so rather than leaving a reader to find out from
# Gatekeeper. A signing step slots in later without rework: `--sign` on the pkgbuild and
# productbuild calls below, or `productsign` over the finished archive.
#
# Usage: scripts/build_macos_pkg.sh <payload-root> <version> <output.pkg>
#
#   payload-root  the DESTDIR the payload was staged into, holding usr/local/...
#   version       the version the receipt is filed under, e.g. 0.1.0
#   output.pkg    where to write the installer

set -euo pipefail

fail() {
    printf 'build_macos_pkg: FAIL: %s\n' "$*" >&2
    exit 1
}

[ "$(uname -s)" = 'Darwin' ] ||
    fail "macOS only -- pkgbuild and productbuild ship with Xcode's command line tools"

[ "$#" -eq 3 ] ||
    fail "usage: $0 <payload-root> <version> <output.pkg>"

PAYLOAD_ROOT=$1
VERSION=$2
OUTPUT=$3
readonly PAYLOAD_ROOT VERSION OUTPUT

# Refused here rather than left to pkgbuild, which takes an empty --version and files a receipt
# with nothing in it. A caller reading the version out of the binary and losing it -- CI carries
# it between two steps -- would otherwise get an installer that builds, installs, and compares
# equal to the empty string it was checked against.
[ -n "$VERSION" ] ||
    fail 'the version is empty -- pass the version the payload was built as, e.g. 0.1.0'

# The reverse-DNS name the receipt is filed under, and so the string an operator types to
# `pkgutil --forget` this install and the string a later `productsign` inherits. `sendspin-cli`
# and not `sendspin-cpp-cli` for the reason the doc directory is named that way in
# CMakeLists.txt: everything anyone types is the binary's name. Changing it later orphans the
# receipt of every install before the change rather than upgrading it.
readonly IDENTIFIER='io.github.chrisuthe.sendspin-cli'

# --root names the payload's usr/local rather than its root, paired with an --install-location
# that says where that directory goes. Two things fall out of pointing it here and not one
# level up. The staged tree carries a BUILD-INFO.txt beside `usr/` for the tarball's readers,
# and a root of the whole tree would install that file at `/` -- excluded structurally, by where
# the root points, rather than by a filter somebody can drop. And the pair declares the one
# directory this package writes to, instead of nominally claiming `/usr`.
readonly PAYLOAD_PREFIX="$PAYLOAD_ROOT/usr/local"
readonly PAYLOAD_BINARY="$PAYLOAD_PREFIX/bin/sendspin-cli"

# The message names both ways this goes wrong, because they look the same from here: a tree that
# was never staged, and one staged from a build configured for some other prefix. This script is
# /usr/local-only by construction -- the prefix is baked in at configure time, so a payload built
# for anywhere else is not one it can relocate.
[ -x "$PAYLOAD_BINARY" ] ||
    fail "no executable at '$PAYLOAD_BINARY' -- stage the payload from a build configured for
    the /usr/local prefix:
    DESTDIR='$PAYLOAD_ROOT' cmake --install build --component sendspin-cli"

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR
trap 'rm -rf "$WORK_DIR"' EXIT

# productbuild resolves the pkg-ref below against --package-path by filename, so the component
# is written under a directory of its own holding nothing else.
readonly COMPONENT_DIR="$WORK_DIR/packages"
readonly COMPONENT_NAME='sendspin-cli.pkg'
readonly RESOURCE_DIR="$WORK_DIR/resources"
mkdir -p "$COMPONENT_DIR" "$RESOURCE_DIR"

# Read off the binary rather than passed in, so a universal build widens the installer by itself
# and a per-arch one stays refused on the architecture it could only fail on. `lipo -archs`
# prints them separated by whitespace and hostArchitectures wants commas, so runs are squeezed
# and a trailing separator dropped: `arm64,` is a malformed attribute, and the first anybody
# would hear of it is Installer refusing the package on a Mac that should have taken it.
ARCHITECTURES="$(lipo -archs "$PAYLOAD_BINARY" | tr -s '[:space:]' ',' | sed 's/,$//')"
readonly ARCHITECTURES
[ -n "$ARCHITECTURES" ] ||
    fail "lipo reported no architectures for '$PAYLOAD_BINARY'"

# ==============================================================================
# The component package
# ==============================================================================

# --ownership recommended is pkgbuild's default and is named anyway, because it is what makes
# the installed files root:wheel: the staged payload is owned by whoever ran `cmake --install`,
# and `preserve` would file a build user's uid into the receipt and install /usr/local/bin/
# sendspin-cli owned by them.
pkgbuild \
    --root "$PAYLOAD_PREFIX" \
    --install-location /usr/local \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --ownership recommended \
    "$COMPONENT_DIR/$COMPONENT_NAME"

# ==============================================================================
# The product archive
# ==============================================================================
#
# productbuild over the component package rather than shipping the component itself, for two
# things a component package cannot do. It has no architecture gate, so an arm64-only installer
# would report success on an Intel Mac and leave the user a binary that answers `Bad CPU type in
# executable`; <options hostArchitectures> is the declarative gate for that, and Installer
# refuses the package rather than writing it. And distribution panes are a product-archive
# feature, so there is otherwise nowhere to say that this installer is unsigned at the moment
# somebody is deciding whether to run it.
#
# The distribution is generated rather than checked into packaging/ because three of its fields
# -- the version, the component's filename and the architectures -- are computed above, and a
# template would be a second place to look for values this script already holds. packaging/
# stays what CMake installs.

readonly WELCOME='welcome.txt'

# Read out of the payload rather than written out here, for the reason the distribution is
# generated: a hand-kept copy of the install set is a second place to look, and this one has
# nothing checking it -- CI asserts the receipt, not the pane. `! -type d` and not `-type f` so
# a symlink into the payload shows up rather than going quietly missing, which is the same
# choice build.yml's payload assertion makes.
INSTALLED_PATHS="$(cd "$PAYLOAD_PREFIX" && find . ! -type d | sed 's|^\./|    /usr/local/|' | sort)"
readonly INSTALLED_PATHS

# Everything here has to be true for somebody reading it *in Installer*, which is the one thing
# a welcome pane can be sure of about its reader. So no quarantine advice: a reader has already
# opened the package, and the `installer -pkg` path renders no panes at all. That belongs in
# README.md and BUILD-INFO.txt, where somebody who cannot open this will actually be.
cat >"$RESOURCE_DIR/$WELCOME" <<WELCOME_TEXT
sendspin-cli $VERSION ($ARCHITECTURES)

This installer is NOT signed or notarized, and installing it does not make
sendspin-cli pass Gatekeeper. The binary inside is ad-hoc signed, so it carries
no developer identity for macOS to check. A Developer ID signature and
notarization are still owed; see docs/ROADMAP.md item 10.

Installs into /usr/local, which is the prefix the binary was built for:

$INSTALLED_PATHS

There is no uninstaller. Removing those files and running
\`sudo pkgutil --forget $IDENTIFIER\` undoes this completely.
WELCOME_TEXT

# customize="never" because there is one choice and nothing to opt out of, and
# enable_localSystem alone because a payload built for an absolute /usr/local prefix has no
# meaning installed anywhere else -- both of which take the destination pane out of a two-pane
# install. <product> carries the same identifier rather than a second one nobody types, and is
# what makes Installer refuse to put this over an install that is already newer.
cat >"$WORK_DIR/distribution.xml" <<DISTRIBUTION
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>sendspin-cli $VERSION</title>
    <product id="$IDENTIFIER" version="$VERSION"/>
    <welcome file="$WELCOME" mime-type="text/plain"/>
    <options customize="never" require-scripts="false" hostArchitectures="$ARCHITECTURES"/>
    <domains enable_localSystem="true" enable_anyVolume="false" enable_currentUserHome="false"/>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" title="sendspin-cli">
        <pkg-ref id="$IDENTIFIER"/>
    </choice>
    <pkg-ref id="$IDENTIFIER" version="$VERSION">$COMPONENT_NAME</pkg-ref>
</installer-gui-script>
DISTRIBUTION

productbuild \
    --distribution "$WORK_DIR/distribution.xml" \
    --package-path "$COMPONENT_DIR" \
    --resources "$RESOURCE_DIR" \
    "$OUTPUT"

# Echoed rather than trusted: the architectures are derived from the binary, and an arm64 runner
# can prove the declaration it produced even though it cannot prove the refusal on an Intel Mac.
printf 'build_macos_pkg: wrote %s\n' "$OUTPUT"
printf 'build_macos_pkg: identifier %s, version %s, hostArchitectures %s\n' \
    "$IDENTIFIER" "$VERSION" "$ARCHITECTURES"
