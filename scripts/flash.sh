#!/usr/bin/env bash
# Build and flash firmware (and optionally the SPIFFS filesystem) to a connected
# ESP32 board. Defaults to the xiao-s3 env and auto-detects the USB serial port.
#
# Usage:
#   scripts/flash.sh                  # build + upload firmware to xiao-s3
#   scripts/flash.sh -e esp32s3       # pick a different PlatformIO env
#   scripts/flash.sh --fs             # also build + upload the SPIFFS image
#   scripts/flash.sh --fs-only        # only (re)upload the SPIFFS image
#   scripts/flash.sh -p /dev/cu.usbmodemXXXX   # explicit port
set -euo pipefail
cd "$(dirname "$0")/.."

ENV="xiao-s3"
PORT=""
DO_FW=1
DO_FS=0
while [ $# -gt 0 ]; do
  case "$1" in
    -e|--env)  ENV="$2"; shift 2;;
    -p|--port) PORT="$2"; shift 2;;
    --fs)      DO_FS=1; shift;;
    --fs-only) DO_FS=1; DO_FW=0; shift;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 1;;
  esac
done

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1 || true)
fi
[ -z "$PORT" ] && { echo "No serial port found — connect the board or pass -p <port>" >&2; exit 1; }
echo ">> env=$ENV port=$PORT firmware=$DO_FW fs=$DO_FS"

[ "$DO_FW" -eq 1 ] && pio run -e "$ENV" -t upload   --upload-port "$PORT"
[ "$DO_FS" -eq 1 ] && pio run -e "$ENV" -t uploadfs --upload-port "$PORT"
echo ">> done"
