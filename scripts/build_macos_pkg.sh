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
# Wraps a staged install payload (`cmake --install --component sendspin-cli` DESTDIR) into an unsigned macOS .pkg.
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

# Refused here: pkgbuild accepts an empty --version.
[ -n "$VERSION" ] ||
    fail 'the version is empty -- pass the version the payload was built as, e.g. 0.1.0'

# The receipt identifier; changing it orphans existing installs' receipts.
readonly IDENTIFIER='io.github.chrisuthe.sendspin-cli'

# The payload root is usr/local, so BUILD-INFO.txt beside usr/ is never installed.
readonly PAYLOAD_PREFIX="$PAYLOAD_ROOT/usr/local"
readonly PAYLOAD_BINARY="$PAYLOAD_PREFIX/bin/sendspin-cli"

# /usr/local-only: the prefix is baked in at configure time.
[ -x "$PAYLOAD_BINARY" ] ||
    fail "no executable at '$PAYLOAD_BINARY' -- stage the payload from a build configured for
    the /usr/local prefix:
    DESTDIR='$PAYLOAD_ROOT' cmake --install build --component sendspin-cli"

WORK_DIR="$(mktemp -d)"
readonly WORK_DIR
trap 'rm -rf "$WORK_DIR"' EXIT

# productbuild finds the component by filename, so it gets a directory of its own.
readonly COMPONENT_DIR="$WORK_DIR/packages"
readonly COMPONENT_NAME='sendspin-cli.pkg'
readonly RESOURCE_DIR="$WORK_DIR/resources"
mkdir -p "$COMPONENT_DIR" "$RESOURCE_DIR"

# From the binary; lipo separates with whitespace, hostArchitectures wants commas and no trailing one.
ARCHITECTURES="$(lipo -archs "$PAYLOAD_BINARY" | tr -s '[:space:]' ',' | sed 's/,$//')"
readonly ARCHITECTURES
[ -n "$ARCHITECTURES" ] ||
    fail "lipo reported no architectures for '$PAYLOAD_BINARY'"

# Component package

# Ownership recommended installs root:wheel regardless of who staged the payload.
pkgbuild \
    --root "$PAYLOAD_PREFIX" \
    --install-location /usr/local \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --ownership recommended \
    "$COMPONENT_DIR/$COMPONENT_NAME"

# Product archive: adds the hostArchitectures gate and the unsigned-installer welcome pane.

readonly WELCOME='welcome.txt'

# The pane lists what the payload really installs. CDPATH='' stops cd printing to stdout.
INSTALLED_PATHS="$(CDPATH='' cd "$PAYLOAD_PREFIX" && find . ! -type d | sed 's|^\./|    /usr/local/|' | sort)"
readonly INSTALLED_PATHS

# Written for a reader inside Installer, so no quarantine advice.
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

# One choice, local system only; <product> refuses to downgrade a newer install.
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

printf 'build_macos_pkg: wrote %s\n' "$OUTPUT"
printf 'build_macos_pkg: identifier %s, version %s, hostArchitectures %s\n' \
    "$IDENTIFIER" "$VERSION" "$ARCHITECTURES"
