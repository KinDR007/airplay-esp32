#!/usr/bin/env bash
# Stage everything, commit, and push to your GitHub fork.
#
# Pushes to the 'kindr' remote (KinDR007/airplay-esp32) if it exists, otherwise
# 'origin'. Override with REMOTE=... / BRANCH=... env vars.
#
# Usage:
#   scripts/push.sh "commit message"
#   REMOTE=origin scripts/push.sh "msg"
set -euo pipefail
cd "$(dirname "$0")/.."

MSG="${1:-}"
[ -z "$MSG" ] && { echo "usage: scripts/push.sh \"commit message\"" >&2; exit 1; }

REMOTE="${REMOTE:-}"
if [ -z "$REMOTE" ]; then
  if git remote | grep -qx kindr; then REMOTE=kindr; else REMOTE=origin; fi
fi
BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"

echo ">> remote=$REMOTE branch=$BRANCH"
git add -A
if git diff --cached --quiet; then
  echo ">> nothing staged to commit"
else
  git commit -m "$MSG"
fi
git push "$REMOTE" "$BRANCH"
echo ">> pushed"
