#!/usr/bin/env bash
# install.sh — Build and install linuwu-sensed fan curve daemon
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── check libyaml ───────────────────────────────────────────────────────────
echo "Checking for libyaml..."
if ! pkg-config --exists yaml-0.1 2>/dev/null; then
    echo "libyaml development headers not found."
    echo "Install with:"
    echo "  Debian/Ubuntu: sudo apt install libyaml-dev"
    echo "  Arch Linux:    sudo pacman -S libyaml"
    echo "  Fedora/RHEL:   sudo dnf install libyaml-devel"
    exit 1
fi
echo "  libyaml found: $(pkg-config --modversion yaml-0.1)"

# ── build ────────────────────────────────────────────────────────────────────
echo "Building ${DAEMON_NAME}..."
make -C "${SCRIPT_DIR}"
echo "  Build successful."

# ── install binary ───────────────────────────────────────────────────────────
echo "Installing binary to ${INSTALL_BIN}..."
install -m 755 "${SCRIPT_DIR}/${DAEMON_NAME}" "${INSTALL_BIN}"

# ── install default config (only if not already present) ────────────────────
if [[ -f "${DEFAULT_CONFIG}" ]]; then
    echo "Config ${DEFAULT_CONFIG} already exists — not overwriting."
    echo "  Reference config: ${SCRIPT_DIR}/linuwu-sense-daemon.yaml"
else
    echo "Installing default config to ${DEFAULT_CONFIG}..."
    install -m 644 "${SCRIPT_DIR}/linuwu-sense-daemon.yaml" "${DEFAULT_CONFIG}"
fi

# ── install systemd service ──────────────────────────────────────────────────
echo "Installing systemd service..."
install -m 644 "${SCRIPT_DIR}/${SERVICE_FILE}" "${SYSTEMD_DIR}/${SERVICE_FILE}"
systemctl daemon-reload
systemctl enable "${SERVICE_FILE}"
systemctl start  "${SERVICE_FILE}"

echo ""
echo "✅  linuwu-sensed installed and started."
echo ""
echo "Useful commands:"
echo "  systemctl status  ${DAEMON_NAME}"
echo "  journalctl -u     ${DAEMON_NAME} -f"
echo "  systemctl restart ${DAEMON_NAME}   # after editing ${DEFAULT_CONFIG}"
