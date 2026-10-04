#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ADB="${ADB:-adb}"
REMOTE_ROOT="${REMOTE_ROOT:-/data/local/tmp/adreno-perf-tools/sweeps}"
DEST_ROOT="${DEST_ROOT:-$REPO_ROOT/results}"

if [[ ! "$REMOTE_ROOT" =~ ^/[A-Za-z0-9._/-]+$ ]]; then
  echo "[host] ERROR: REMOTE_ROOT contains unsupported characters: $REMOTE_ROOT" >&2
  exit 2
fi

mkdir -p "$DEST_ROOT"

echo "[host] Finding latest sweep under $REMOTE_ROOT ..."
LATEST_SWEEP=$("$ADB" shell "su -c 'ls -td $REMOTE_ROOT/sweep_* 2>/dev/null | head -n 1'" | tr -d '\r')

if [[ -z "$LATEST_SWEEP" ]]; then
  echo "[host] ERROR: No sweep directory found." >&2
  exit 1
fi

SWEEP_NAME="${LATEST_SWEEP##*/}"
LOCAL_SWEEP="$DEST_ROOT/$SWEEP_NAME"
if [[ -e "$LOCAL_SWEEP" ]]; then
  echo "[host] ERROR: Refusing to merge into existing path: $LOCAL_SWEEP" >&2
  exit 1
fi

echo "[host] Pulling $LATEST_SWEEP"
echo "[host] Destination: $LOCAL_SWEEP"
"$ADB" pull "$LATEST_SWEEP" "$DEST_ROOT/"

echo "[host] Pulled files (first 40):"
find "$LOCAL_SWEEP" -type f -print | sort | sed -n '1,40p'
echo "[host] Summary file:"
find "$LOCAL_SWEEP" -name summary.csv -type f -print
