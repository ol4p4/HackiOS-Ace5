#!/bin/bash
# Extract the Ace 5 facts needed for SMP and var_store persistence.
# Read-only: every command here only reads the phone.
#
# usage: ACE5_SERIAL=<serial> tools/ace5/extract-hw.sh [outdir]
#   outdir defaults to local/device
set -euo pipefail
cd "$(dirname "$0")/../.."

SERIAL=${ACE5_SERIAL:?set ACE5_SERIAL to the Android/adb serial}
OUT=${1:-local/device}
mkdir -p "$OUT"

adb -s "$SERIAL" get-state >/dev/null
MODEL=$(adb -s "$SERIAL" shell getprop ro.product.model | tr -d '\r')
[ "$MODEL" = "PKG110" ] || { echo "extract-hw: not PKG110 (got '$MODEL')" >&2; exit 1; }

su_text() { adb -s "$SERIAL" shell su -c "$1"; }
su_bin()  { adb -s "$SERIAL" exec-out su -c "$1"; }

# CPU, GIC/PSCI and partition facts
su_text 'cat /proc/cpuinfo' > "$OUT/cpuinfo.txt"
su_text 'ls /sys/devices/system/cpu' > "$OUT/cpu-list.txt"
su_text 'dmesg' > "$OUT/dmesg.txt"
su_text 'cat /proc/interrupts' > "$OUT/interrupts.txt"
su_text 'cat /proc/cmdline' > "$OUT/cmdline.txt"
su_text 'ls -l /dev/block/by-name' > "$OUT/by-name.txt"
su_text 'cat /proc/partitions' > "$OUT/partitions.txt"
su_text 'find /vendor/firmware /vendor/firmware_mnt /odm/firmware -type f 2>/dev/null' > "$OUT/fw-list.txt"

# Device tree (binary; exec-out keeps the bytes intact)
su_bin 'cat /sys/firmware/fdt' > "$OUT/giulia.dtb"

# GPT head/tail in 4096-byte blocks, for tools/userdata-carve.py
su_bin 'dd if=/dev/block/sda bs=4096 count=8 2>/dev/null' > "$OUT/gpt-head.bin"
SECTORS=$(su_text 'cat /sys/block/sda/size' | tr -d '\r')
BLOCKS=$((SECTORS / 8))                 # 512-byte sectors -> 4096-byte blocks
SKIP=$((BLOCKS - 8))
su_bin "dd if=/dev/block/sda bs=4096 skip=$SKIP count=8 2>/dev/null" > "$OUT/gpt-tail.bin"

# userdata f2fs superblock and segment map, for the carve
su_bin 'dd if=/dev/block/by-name/userdata bs=4096 count=1 2>/dev/null' > "$OUT/userdata-meta-before.bin"
su_text 'cat /proc/fs/f2fs/*/segment_info 2>/dev/null' > "$OUT/segment_info-before.txt" || true

echo "extract-hw: wrote $OUT"
echo "  next: tools/ace5/inspect-dtb.py $OUT/giulia.dtb | grep -iE 'gic|ufshc|cpu@'"
echo "  next: python3 tools/userdata-carve.py $OUT 20 darwin   # after the gate in RUNBOOK.md"
