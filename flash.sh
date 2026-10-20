#!/usr/bin/env bash
# Build the firmware and flash it, pausing the bridge service so it isn't
# holding the serial port.
#
# Needs ESP-IDF v5.5. If idf.py isn't already on PATH, the script sources
# $IDF_EXPORT (default ~/esp/esp-idf-v5.5.3/export.sh). Set DISTROBOX=<name>
# to build inside a distrobox/toolbox container instead. Put local overrides
# in flash.env (git-ignored).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
[ -f flash.env ] && source flash.env

IDF_EXPORT=${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.3/export.sh}
DISTROBOX=${DISTROBOX:-}
PORT=${PORT:-$(ls /dev/serial/by-id/usb-Espressif* 2>/dev/null | head -1 || true)}

build_cmd="cd '$ROOT/firmware' && idf.py build"
if [ -n "$DISTROBOX" ]; then
  distrobox enter "$DISTROBOX" -- bash -lc "source '$IDF_EXPORT' >/dev/null && $build_cmd"
elif command -v idf.py >/dev/null; then
  bash -c "$build_cmd"
else
  bash -c "source '$IDF_EXPORT' >/dev/null && $build_cmd"
fi

[ -n "$PORT" ] || { echo "No Espressif board found; set PORT=/dev/..." >&2; exit 1; }

was_active=0
if command -v systemctl >/dev/null && systemctl --user is-active --quiet claude-usage-bridge; then
  was_active=1
  systemctl --user stop claude-usage-bridge
fi
trap '[ $was_active = 1 ] && systemctl --user start claude-usage-bridge' EXIT

cd firmware/build
if command -v esptool.py >/dev/null; then ESPTOOL=(esptool.py); else ESPTOOL=(uvx --from esptool esptool); fi
"${ESPTOOL[@]}" --chip esp32c6 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset write-flash @flash_args
