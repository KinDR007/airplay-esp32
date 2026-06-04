#!/usr/bin/env python3
"""Collect the flashable binaries for one PlatformIO env into dist/<env>/.

Usage (from repo root, after `pio run -e <env>` and `pio run -e <env> -t buildfs`):

    python tools/ci/package_firmware.py <env>

Produces dist/<env>/ containing:
  - the individual partitions (bootloader / partitions / ota_data / firmware /
    spiffs), copied straight from the build directory
  - flasher_args.json (exact offsets + flash settings from the build)
  - <env>-factory.bin — a single merged image flashable at 0x0 (best effort)
  - FLASH.md — ready-to-paste esptool commands

Offsets, chip and flash settings all come from the build's flasher_args.json,
so this stays correct across boards (ESP32 classic bootloader @0x1000,
S3/P4 @0x0, differing partition layouts, etc.).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys


def role_file(native_name: str) -> str:
    """Map an ESP-IDF flash-file name to the file PlatformIO actually emits."""
    b = os.path.basename(native_name).lower()
    if "bootloader" in b:
        return "bootloader.bin"
    if "partition" in b:
        return "partitions.bin"
    if "ota_data" in b or "otadata" in b:
        return "ota_data_initial.bin"
    if "storage" in b or "spiffs" in b or "littlefs" in b or "fatfs" in b:
        return "spiffs.bin"
    return "firmware.bin"  # the application image


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: package_firmware.py <env>", file=sys.stderr)
        return 2
    env = sys.argv[1]
    build = os.path.join(".pio", "build", env)
    dist = os.path.join("dist", env)
    os.makedirs(dist, exist_ok=True)

    with open(os.path.join(build, "flasher_args.json")) as fh:
        fa = json.load(fh)
    chip = fa.get("extra_esptool_args", {}).get("chip") or fa.get("chip") or "auto"
    settings = fa.get("flash_settings", {})
    flash_files = fa.get("flash_files", {})

    # (offset, dist_path, name) ordered by offset.
    entries: list[tuple[str, str, str]] = []
    for off, native in sorted(flash_files.items(), key=lambda kv: int(kv[0], 16)):
        name = role_file(native)
        src = os.path.join(build, name)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(dist, name))
            entries.append((off, os.path.join(dist, name), name))
        else:
            print(f"WARN: {name} missing for offset {off} (skipped)")

    shutil.copy(
        os.path.join(build, "flasher_args.json"),
        os.path.join(dist, "flasher_args.json"),
    )

    # --- merged factory image (best effort) ---------------------------------
    factory = os.path.join(dist, f"{env}-factory.bin")
    merge = [sys.executable, "-m", "esptool", "--chip", chip, "merge_bin", "-o", factory]
    if settings.get("flash_mode"):
        merge += ["--flash_mode", settings["flash_mode"]]
    if settings.get("flash_freq"):
        merge += ["--flash_freq", settings["flash_freq"]]
    if settings.get("flash_size"):
        merge += ["--flash_size", settings["flash_size"]]
    for off, path, _ in entries:
        merge += [off, path]

    merged_ok = True
    try:
        subprocess.check_call(merge)
    except Exception as err:  # noqa: BLE001 - non-fatal, individual bins remain
        merged_ok = False
        print(f"WARN: merge_bin failed ({err}); shipping individual bins only")

    # --- flash instructions -------------------------------------------------
    parts = " ".join(f"{off} {name}" for off, _, name in entries)
    lines = [
        f"# Flashing `{env}`",
        "",
        f"- Chip: **{chip}**",
        f"- Flash: `{settings.get('flash_mode')} / {settings.get('flash_size')} / "
        f"{settings.get('flash_freq')}`",
        "",
        "## Option A — one merged file (easiest)",
        "",
    ]
    if merged_ok:
        lines += [
            "Flash the whole thing (app + filesystem + bootloader) at once:",
            "",
            "```bash",
            f"esptool.py --chip {chip} write_flash 0x0 {env}-factory.bin",
            "```",
            "",
            "…or drag `"
            f"{env}-factory.bin` into the web flasher "
            "<https://espressif.github.io/esptool-js/> and flash at offset `0x0`.",
            "",
        ]
    else:
        lines += ["_(merged image unavailable for this build — use Option B)_", ""]
    lines += [
        "## Option B — individual partitions",
        "",
        "```bash",
        f"esptool.py --chip {chip} write_flash {parts}",
        "```",
        "",
    ]
    with open(os.path.join(dist, "FLASH.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"packaged {env} -> {dist} ({'merged+' if merged_ok else ''}individual)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
