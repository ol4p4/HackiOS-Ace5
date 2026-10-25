#!/bin/bash
# Host-only checks for the Ace 5 changes: no phone, no XNU build.
# Runs on Windows Git Bash and on the Linux build host.
set -euo pipefail
cd "$(dirname "$0")/../.."

PY=${PYTHON:-}
if [ -z "$PY" ]; then
  for candidate in python3 python; do
    if "$candidate" -c 'import sys' >/dev/null 2>&1; then PY=$candidate; break; fi
  done
fi
[ -n "$PY" ] || { echo "host-selftest: no working python" >&2; exit 1; }
mkdir -p local

echo "== syntax =="
"$PY" -m py_compile tools/ace5/phone.py tools/ace5/pack-boot.py \
  tools/ace5/storage-constants.py tools/ace5/apply-storage-constants.py tools/ace5/check-host.py
bash -n tools/ace5/build-loader.sh
bash -n tools/ace5/extract-hw.sh

echo "== dry-run: cpus=2 =="
ACE5_CPUS=2 "$PY" tools/ace5/phone.py --serial X run --dry-run --seconds 60 --bootcpu 1 \
  | tee local/host-selftest-cpus.log
grep -q 'Command line: cpus=2 ' local/host-selftest-cpus.log

echo "== dry-run: cpumask + persist =="
ACE5_CPUMASK=0x7F "$PY" tools/ace5/phone.py --serial X run --dry-run --persist \
  --seconds 60 --bootcpu 1 | tee local/host-selftest-mask.log
grep -q 'Command line: cpumask=0x7F ' local/host-selftest-mask.log
grep -q 'ufs=2 ufs_write=1 var_store=1' local/host-selftest-mask.log

echo "== synthetic GPT =="
"$PY" - <<'PY'
import struct
import uuid
from pathlib import Path

BS = 4096
d = Path('local/host-selftest-gpt')
d.mkdir(parents=True, exist_ok=True)
head = bytearray(8 * BS)
h = bytearray(BS)
h[:8] = b'EFI PART'
struct.pack_into('<Q', h, 72, 2)      # entries at LBA 2
struct.pack_into('<I', h, 80, 128)    # 128 entries
struct.pack_into('<I', h, 84, 128)    # 128 bytes each
head[BS:2 * BS] = h
e = bytearray(128)
e[:16] = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac').bytes_le
e[16:32] = uuid.UUID('11111111-2222-3333-4444-555555555555').bytes_le
struct.pack_into('<QQQ', e, 32, 100000, 200000, 0)
name = 'darwin'.encode('utf-16le')
e[56:56 + len(name)] = name
head[2 * BS:3 * BS] = e
(d / 'gpt-head.bin').write_bytes(head)
PY
"$PY" tools/ace5/storage-constants.py local/host-selftest-gpt | tee local/host-selftest-const.log
grep -q 'BLOCK_DARWIN_FIRST  100000ULL' local/host-selftest-const.log
grep -q 'BLOCK_DARWIN_BLOCKS 100001ULL' local/host-selftest-const.log
grep -q '0x11111111' local/host-selftest-const.log

echo "== constant writer on a copy =="
rm -rf local/host-selftest-root
mkdir -p local/host-selftest-root/xnuloader local/host-selftest-root/xnu/files/bsd/dev
cp xnuloader/loader.c local/host-selftest-root/xnuloader/loader.c
cp xnu/files/bsd/dev/redwood_ufs.c local/host-selftest-root/xnu/files/bsd/dev/redwood_ufs.c
"$PY" tools/ace5/apply-storage-constants.py local/host-selftest-gpt \
  --root local/host-selftest-root --write
grep -q '^#define BLOCK_DARWIN_FIRST 100000ULL' local/host-selftest-root/xnuloader/loader.c
grep -q '^#define BLOCK_DARWIN_LAST 200000ULL' local/host-selftest-root/xnuloader/loader.c
grep -q '^#define UFS_DARWIN_BLOCKS 100001ULL' local/host-selftest-root/xnu/files/bsd/dev/redwood_ufs.c
grep -q '0x11111111' local/host-selftest-root/xnuloader/loader.c

echo "host selftest OK"
