#!/usr/bin/env bash
# Open a serial monitor on the connected board. Defaults to the xiao-s3 env and
# auto-detects the USB serial port. (Run from an interactive terminal — the
# PlatformIO monitor needs a TTY.)
#
# Usage:
#   scripts/monitor.sh                 # monitor xiao-s3 on the detected port
#   scripts/monitor.sh -e esp32s3
#   scripts/monitor.sh -p /dev/cu.usbmodemXXXX
set -euo pipefail
cd "$(dirname "$0")/.."

ENV="xiao-s3"
PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -e|--env)  ENV="$2"; shift 2;;
    -p|--port) PORT="$2"; shift 2;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 1;;
  esac
done

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1 || true)
fi
[ -z "$PORT" ] && { echo "No serial port found — connect the board or pass -p <port>" >&2; exit 1; }
exec pio device monitor -e "$ENV" --port "$PORT"
