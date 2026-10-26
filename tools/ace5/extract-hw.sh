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

# userdata f2fs superblock and segment map, for the carve.
#
# /data is metadata-encrypted on this phone: the f2fs sits on a device-mapper
# device (dm-N) and only there is its superblock plaintext. The raw partition
# (/dev/block/by-name/userdata = sda15) holds ciphertext, so read the mapper
# device, resolved fresh every run -- the dm number changes between boots.
MAPPER=$(su_text 'readlink -f /dev/block/mapper/userdata 2>/dev/null' | tr -d '\r')
if [ -z "$MAPPER" ] || [ "${MAPPER#/}" = "$MAPPER" ]; then
	MAPPER=$(su_text 'ls -l /dev/block/mapper 2>/dev/null' | tr -d '\r' \
		| sed -n 's|.*userdata -> \(.*\)|\1|p' | head -1)
	case "$MAPPER" in
		/dev/*) ;;
		../*) MAPPER=/dev/block/${MAPPER#../} ;;
		dm-*) MAPPER=/dev/block/$MAPPER ;;
	esac
fi
[ -n "$MAPPER" ] || { echo "extract-hw: cannot resolve /dev/block/mapper/userdata" >&2; exit 1; }
DMNAME=$(basename "$MAPPER")
echo "extract-hw: /data is on $MAPPER ($DMNAME)"

[ -f "$OUT/userdata-meta-before.bin" ] && cp -f "$OUT/userdata-meta-before.bin" "$OUT/userdata-meta-before.prev.bin"
su_bin "dd if=$MAPPER bs=4096 count=1 2>/dev/null" > "$OUT/userdata-meta-before.bin"
su_text "cat /proc/fs/f2fs/$DMNAME/segment_info" > "$OUT/segment_info-before.txt" 2>/dev/null \
	|| su_text 'cat /proc/fs/f2fs/*/segment_info 2>/dev/null' > "$OUT/segment_info-before.txt" || true

# f2fs magic 0xf2f52010, little-endian at offset 1024 inside block 0
MAGIC=$(od -An -tx1 -j 1024 -N 4 "$OUT/userdata-meta-before.bin" | tr -d ' \n')
if [ "$MAGIC" != "1020f5f2" ]; then
	echo "extract-hw: WARNING: superblock at $MAPPER is not plaintext f2fs (magic '$MAGIC', want 1020f5f2)" >&2
	echo "  is /data unlocked? the carve will refuse until this reads the f2fs superblock" >&2
fi

echo "extract-hw: wrote $OUT"
echo "  next: tools/ace5/inspect-dtb.py $OUT/giulia.dtb | grep -iE 'gic|ufshc|cpu@'"
echo "  next: python3 tools/userdata-carve.py $OUT 20 darwin   # after the gate in RUNBOOK.md"
