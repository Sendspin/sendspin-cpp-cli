#!/bin/sh
# Package preremove: stops and disables the system unit on removal, not upgrade. Account and state stay.
set -e

# deb passes "upgrade", rpm the count of versions left (0 on erase), Arch calls this on removal only.
case "${1:-}" in
    upgrade | failed-upgrade) exit 0 ;;
    '' | 0 | *[!0-9]*) ;;
    *) exit 0 ;;
esac

# Disabling only edits /etc symlinks, so it runs without a booted systemd too; stopping needs one.
if command -v systemctl >/dev/null 2>&1; then
    systemctl --no-reload disable sendspin-cli.service >/dev/null 2>&1 || true
fi
if [ -d /run/systemd/system ]; then
    systemctl stop sendspin-cli.service >/dev/null 2>&1 || true
fi
