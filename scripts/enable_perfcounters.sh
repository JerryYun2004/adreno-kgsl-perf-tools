#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

ADB="${ADB:-adb}"
PERFCOUNTER_NODE="${PERFCOUNTER_NODE:-/sys/class/kgsl/kgsl-3d0/perfcounter}"

if [[ ! "$PERFCOUNTER_NODE" =~ ^/[A-Za-z0-9._/-]+$ ]]; then
  echo "[host] ERROR: PERFCOUNTER_NODE contains unsupported characters: $PERFCOUNTER_NODE" >&2
  exit 2
fi

if ! "$ADB" get-state >/dev/null 2>&1; then
  echo "[host] ERROR: No single usable ADB device is connected." >&2
  exit 1
fi

echo "[host] Enabling KGSL performance-counter reads through $PERFCOUNTER_NODE ..."

if ! "$ADB" shell "su -c 'test -e \"$PERFCOUNTER_NODE\"'"; then
  echo "[host] ERROR: Node is unavailable on the device: $PERFCOUNTER_NODE" >&2
  exit 1
fi

"$ADB" shell "su -c 'echo 1 > \"$PERFCOUNTER_NODE\"'"
STATE=$("$ADB" shell "su -c 'cat \"$PERFCOUNTER_NODE\"'" | tr -d '\r\n')

if [[ "$STATE" != "1" ]]; then
  echo "[host] ERROR: Expected $PERFCOUNTER_NODE to read 1; got: $STATE" >&2
  exit 1
fi

echo "[host] KGSL performance-counter reads enabled (state=1)."
echo "[host] Repeat this command after a device reboot if the node resets."
