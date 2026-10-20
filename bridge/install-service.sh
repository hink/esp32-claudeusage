#!/usr/bin/env bash
# Install the bridge as a systemd user service (Linux).
# Extra arguments are passed to the bridge, e.g. ./install-service.sh --brightness 40
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
UV=$(command -v uv || true)
[ -n "$UV" ] || { echo "uv not found: https://docs.astral.sh/uv/" >&2; exit 1; }

UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
mkdir -p "$UNIT_DIR"
sed -e "s|@UV@|$UV|" -e "s|@REPO@|$REPO|" -e "s|@ARGS@|$*|" \
  "$REPO/bridge/claude-usage-bridge.service.in" > "$UNIT_DIR/claude-usage-bridge.service"

systemctl --user daemon-reload
systemctl --user enable --now claude-usage-bridge
systemctl --user restart claude-usage-bridge
echo "Installed. Logs: journalctl --user -u claude-usage-bridge -f"
