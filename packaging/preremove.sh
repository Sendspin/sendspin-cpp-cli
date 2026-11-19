#!/bin/sh
# Package preremove: stops and disables the system unit on removal, not upgrade. Account and state stay.
set -e

# deb passes "upgrade", rpm the count of versions left (0 on erase), Arch calls this on removal only.
case "${1:-}" in
    upgrade | failed-upgrade) exit 0 ;;
    '' | 0 | *[!0-9]*) ;;
    *) exit 0 ;;
esac

if [ -d /run/systemd/system ]; then
    systemctl disable --now sendspin-cli.service >/dev/null 2>&1 || true
fi
