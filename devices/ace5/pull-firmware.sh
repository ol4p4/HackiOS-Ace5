#!/bin/bash
# Pull the firmware the kernel loads from the phone's own vendor image.
#
# `adb pull /vendor/firmware/...` is denied to the shell user, so every file is
# read through a root shell. The firmware is the vendor's: it stays under fw-*/
# (git-ignored) and is never committed.
#
# usage: ACE5_SERIAL=<serial> devices/ace5/pull-firmware.sh [outdir]
set -euo pipefail
cd "$(dirname "$0")/../.."

SERIAL=${ACE5_SERIAL:?set ACE5_SERIAL to the Android/adb serial}
OUT=${1:-fw-gpu}
export MSYS_NO_PATHCONV=1
mkdir -p "$OUT"

pull() {
	local src=/vendor/firmware/$1
	adb -s "$SERIAL" exec-out su -c "cat $src" > "$OUT/$1"
	local size
	size=$(wc -c < "$OUT/$1" | tr -d ' ')
	[ "$size" -gt 0 ] || { echo "pull-firmware: $src is empty" >&2; exit 1; }
	echo "$OUT/$1  $size bytes"
}

# GPU: GMU, command processor and the board's signed ZAP image.
for f in gmu_gen70900.bin gen70900_sqe.fw gen70900_aqe.fw gen70900_zap.mbn; do
	pull "$f"
done

echo
echo "pull-firmware: the kernel reads these from fw/ in the small root image"
echo "  python3 tools/apfs/mkapfs.py -z --plain fw -o local/root-gmu.apfs redwood=local/root-gmu"
