#!/bin/sh
# Package postinstall: creates the service account and reloads systemd; never enables or starts the unit.
set -e

if command -v systemd-sysusers >/dev/null 2>&1; then
    systemd-sysusers sendspin-cli.conf
fi

# Only on a booted systemd: a container or chroot has no manager to reload.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi
