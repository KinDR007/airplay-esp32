#!/usr/bin/env bash
# Tag a firmware release on the GitHub fork. The 'Firmware' GitHub Action then
# builds every env and attaches the per-board zips to the release automatically.
# Requires the gh CLI authenticated as the repo owner.
#
# Usage:
#   scripts/release.sh v0.1.1 "release notes (optional)"
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:-}"
NOTES="${2:-Firmware release $TAG}"
[ -z "$TAG" ] && { echo "usage: scripts/release.sh <vX.Y.Z> [notes]" >&2; exit 1; }

OWNER_REPO="${OWNER_REPO:-KinDR007/airplay-esp32}"
echo ">> creating release $TAG on $OWNER_REPO"
gh release create "$TAG" --repo "$OWNER_REPO" --target main --title "$TAG" --notes "$NOTES"
echo ">> release created — CI is now building and will attach the firmware zips"
