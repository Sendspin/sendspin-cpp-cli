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
# Formats every tracked C/C++ source in place with clang-format.
#
# Usage: scripts/format.sh [--check]
#
#   --check  change nothing; exit non-zero if any file is not formatted
#
# Set CLANG_FORMAT to use a binary other than the clang-format on $PATH.

set -euo pipefail

fail() {
    printf 'format: FAIL: %s\n' "$*" >&2
    exit 1
}

case "${1:-}" in
    '') MODE=(-i) ;;
    --check) MODE=(--dry-run --Werror) ;;
    *) fail "usage: $0 [--check]" ;;
esac
readonly -a MODE

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
readonly CLANG_FORMAT

command -v "$CLANG_FORMAT" >/dev/null 2>&1 ||
    fail "'$CLANG_FORMAT' is not on \$PATH -- install clang-format or set CLANG_FORMAT"

# git ls-files lists only the current directory's subtree.
cd "$(git rev-parse --show-toplevel)"

printf 'format: using %s\n' "$("$CLANG_FORMAT" --version)"

git ls-files -z '*.cpp' '*.h' | xargs -0 "$CLANG_FORMAT" "${MODE[@]}" ||
    fail "clang-format reported unformatted files; run scripts/format.sh to fix them"

printf 'format: every file is formatted\n'
