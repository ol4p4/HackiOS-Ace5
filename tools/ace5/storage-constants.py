#!/usr/bin/env python3
"""Print the C constants for the carved darwin partition.

usage: storage-constants.py [local/device]

Reads gpt-head.new (preferred) or gpt-head.bin from the directory saved by
tools/ace5/extract-hw.sh and tools/userdata-carve.py, finds the partition named
`darwin`, and prints the values that must agree in:

  xnuloader/loader.c            BLOCK_DARWIN_FIRST / BLOCK_DARWIN_LAST / BLOCK_DARWIN_UNIQ
  xnu/files/bsd/dev/redwood_ufs.c  UFS_DARWIN_FIRST / UFS_DARWIN_BLOCKS

The type GUID is the fixed APFS-container GUID. BLOCK_WRITE_FIRST stays at the
existing 2042368 for var_store-only (var_store writes at 8 GiB); re-derive it
only for a full root-on-userdata layout.
"""
import struct
import sys
import uuid
from pathlib import Path

BS = 4096


def main() -> int:
    d = Path(sys.argv[1] if len(sys.argv) > 1 else 'local/device')
    carved = d / 'gpt-head.new'
    head = (carved if carved.exists() else d / 'gpt-head.bin').read_bytes()
    if len(head) < 2 * BS or head[BS:BS + 8] != b'EFI PART':
        sys.exit('storage-constants: no GPT header at LBA 1')

    h = head[BS:2 * BS]
    entries_lba = struct.unpack_from('<Q', h, 72)[0]
    count = struct.unpack_from('<I', h, 80)[0]
    entry_size = struct.unpack_from('<I', h, 84)[0]
    at = entries_lba * BS          # gpt-head.bin starts at LBA 0
    table = head[at:at + count * entry_size]
    if len(table) != count * entry_size:
        sys.exit('storage-constants: partition table is outside gpt-head')

    found = None
    for i in range(count):
        e = table[i * entry_size:(i + 1) * entry_size]
        if e[:16] == b'\0' * 16:
            continue
        name = e[56:128].decode('utf-16le').split('\0', 1)[0]
        if name == 'darwin':
            found = e
            break
    if found is None:
        sys.exit('storage-constants: no partition named darwin; run userdata-carve.py first')

    first, last = struct.unpack_from('<QQ', found, 32)
    blocks = last - first + 1
    u = uuid.UUID(bytes_le=found[16:32])
    b = u.bytes
    uniq = ('{ 0x%08x, 0x%04x, 0x%04x, { %s } }' %
            (u.time_low, u.time_mid, u.time_hi_version,
             ', '.join('0x%02x' % x for x in b[8:])))

    print(f'BLOCK_DARWIN_FIRST  {first}ULL')
    print(f'BLOCK_DARWIN_LAST   {last}ULL')
    print(f'BLOCK_DARWIN_BLOCKS {blocks}ULL')
    print(f'UFS_DARWIN_FIRST    {first}ULL')
    print(f'UFS_DARWIN_BLOCKS   {blocks}ULL')
    print(f'BLOCK_DARWIN_UNIQ   {uniq}')
    print('# type GUID is the fixed APFS container GUID')
    return 0


if __name__ == '__main__':
    sys.exit(main())
