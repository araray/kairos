#!/bin/bash
# File: deploy/rpm/postinst.sh
# RPM post-install script for Kairos
#
# Equivalent to the DEB postinst — creates user, dirs, enables service.
# Spec reference: §28.7
set -e

# Create kairos user if it doesn't exist.
if ! id -u kairos >/dev/null 2>&1; then
    useradd --system --no-create-home --shell /sbin/nologin kairos
fi

# Create required directories.
install -d -o kairos -g kairos -m 750 /var/lib/kairos
install -d -o kairos -g kairos -m 750 /var/log/kairos
install -d -o kairos -g kairos -m 750 /run/kairos
install -d -o kairos -g kairos -m 750 /etc/kairos

# Install systemd service.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    systemctl enable kairos.service || true
fi

# Install default config if none exists.
if [ ! -f /etc/kairos/kairos.toml ]; then
    if [ -f /usr/share/kairos/kairos.toml.example ]; then
        install -D -m 644 -o kairos -g kairos \
            /usr/share/kairos/kairos.toml.example \
            /etc/kairos/kairos.toml
    fi
fi
