#!/bin/bash
# Regenerate devices/ace5/soc.args from the phone's own device tree.
# Read-only on the phone: /sys/firmware/fdt is world-readable.
#
# usage: ACE5_SERIAL=<serial> devices/ace5/pull-soc.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

SERIAL=${ACE5_SERIAL:?set ACE5_SERIAL to the Android/adb serial}
export MSYS_NO_PATHCONV=1

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb -s "$SERIAL" exec-out cat /sys/firmware/fdt > "$tmp/fdt"
python3 tools/ace5/soc-args.py "$tmp/fdt" > devices/ace5/soc.args
cat devices/ace5/soc.args
echo
echo "soc-args: wrote devices/ace5/soc.args"
echo "  check: python3 tools/ace5/soc-args.py local/device/giulia.dtb --check"
