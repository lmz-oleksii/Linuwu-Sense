#!/usr/bin/env bash
# uninstall.sh — Remove linuwu-sensed fan curve daemon
set -euo pipefail

DAEMON_NAME="linuwu-sensed"
INSTALL_BIN="/usr/local/bin/${DAEMON_NAME}"
DEFAULT_CONFIG="/etc/linuwu-sense-daemon.yaml"
SERVICE_FILE="${DAEMON_NAME}.service"
SYSTEMD_DIR="/etc/systemd/system"

# ── check root ──────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (use sudo)." >&2
    exit 1
fi

echo "Stopping and disabling ${DAEMON_NAME}..."
systemctl stop    "${SERVICE_FILE}" 2>/dev/null || true
systemctl disable "${SERVICE_FILE}" 2>/dev/null || true

echo "Removing systemd service..."
rm -f "${SYSTEMD_DIR}/${SERVICE_FILE}"
systemctl daemon-reload

echo "Removing binary..."
rm -f "${INSTALL_BIN}"

echo ""
echo "Note: ${DEFAULT_CONFIG} was NOT removed."
echo "Remove it manually if you no longer need it:"
echo "  sudo rm ${DEFAULT_CONFIG}"
echo ""
echo "✅  linuwu-sensed uninstalled."
