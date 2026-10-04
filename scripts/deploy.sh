#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ADB="${ADB:-adb}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/adreno-perf-tools/bin}"

if [[ ! "$REMOTE_DIR" =~ ^/[A-Za-z0-9._/-]+$ ]]; then
  echo "[host] ERROR: REMOTE_DIR contains unsupported characters: $REMOTE_DIR" >&2
  exit 2
fi

make -C "$REPO_ROOT" all
"$ADB" shell mkdir -p "$REMOTE_DIR"
"$ADB" push "$REPO_ROOT/build/adreno_perf_stream" "$REMOTE_DIR/adreno_perf_stream"
"$ADB" push "$REPO_ROOT/build/adreno_perf_sweep" "$REMOTE_DIR/adreno_perf_sweep"
"$ADB" shell chmod 755 "$REMOTE_DIR/adreno_perf_stream" "$REMOTE_DIR/adreno_perf_sweep"

echo "[host] Installed in $REMOTE_DIR"
echo "[host] Before collection: make -C \"$REPO_ROOT\" enable"
echo "[host] Streamer help: $ADB shell su -c '$REMOTE_DIR/adreno_perf_stream --help'"
echo "[host] Sweeper plan: $ADB shell su -c '$REMOTE_DIR/adreno_perf_sweep --list-plan'"
