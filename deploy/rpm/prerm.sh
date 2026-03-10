#!/bin/bash
# File: deploy/rpm/prerm.sh
# RPM pre-uninstall script for Kairos
# Spec reference: §28.7
set -e

if [ -d /run/systemd/system ]; then
    systemctl stop kairos.service 2>/dev/null || true
    systemctl disable kairos.service 2>/dev/null || true
    systemctl daemon-reload
fi

rm -f /run/kairos/kairos.pid
