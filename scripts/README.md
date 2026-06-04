# scripts/

Helper scripts for building, flashing and shipping the firmware. All default to
the **xiao-s3** env and auto-detect the USB serial port. Run from anywhere.

| Script | What it does |
|--------|--------------|
| `build.sh [-e env] [-c]` | Build firmware only. `-c` cleans the generated `sdkconfig.<env>` first (needed after editing `sdkconfig.defaults*`). |
| `flash.sh [-e env] [-p port] [--fs] [--fs-only]` | Build + upload firmware. `--fs` also uploads the SPIFFS image; `--fs-only` uploads just the filesystem. |
| `monitor.sh [-e env] [-p port]` | Serial monitor (run in an interactive terminal). |
| `push.sh "message"` | `git add -A` + commit + push to your fork (remote `kindr`, else `origin`). |
| `release.sh vX.Y.Z [notes]` | Tag a GitHub release; CI builds all envs and attaches the per-board zips. Needs `gh`. |

## Examples

```bash
scripts/build.sh -c               # clean build of xiao-s3 (after sdkconfig edits)
scripts/flash.sh --fs             # flash firmware + filesystem to xiao-s3
scripts/flash.sh -e esp32s3       # flash a different board
scripts/monitor.sh                # watch the serial log
scripts/push.sh "tweak fade timing"
scripts/release.sh v0.1.1 "Fix resume gap"
```

## Notes
- **Firmware repo:** `KinDR007/airplay-esp32` (remote `kindr`; `origin` is the
  upstream `rbouteiller/airplay-esp32`).
- **HA integration repo:** `KinDR007/hass-airplay-esp32` (separate, HACS).
- Port on this Mac usually enumerates as `/dev/cu.usbmodem21201` (XIAO native USB).
