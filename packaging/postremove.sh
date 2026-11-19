#!/bin/sh
# Package postremove: forgets the removed units on a booted systemd.
set -e

if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi
