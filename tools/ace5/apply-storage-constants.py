#!/usr/bin/env python3
"""Write the carved darwin constants into the loader and kernel sources.

usage: apply-storage-constants.py [local/device] [--root .] [--write]

Without --write it prints the replacements it would make. With --write it edits
xnuloader/loader.c and xnu/files/bsd/dev/redwood_ufs.c in place.

The values come from tools/ace5/storage-constants.py, which reads gpt-head.new
(or gpt-head.bin) from the directory saved by extract-hw.sh / userdata-carve.py.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIELDS = ('BLOCK_DARWIN_FIRST', 'BLOCK_DARWIN_LAST', 'BLOCK_DARWIN_UNIQ',
          'UFS_DARWIN_FIRST', 'UFS_DARWIN_BLOCKS')


def read_values(directory: str) -> dict:
    out = subprocess.check_output(
        [sys.executable, str(HERE / 'storage-constants.py'), directory], text=True)
    values = {}
    for line in out.splitlines():
        for field in FIELDS:
            if line.startswith(field):
                values[field] = line.split(None, 1)[1] if field == 'BLOCK_DARWIN_UNIQ' else line.split()[1]
                break
    missing = [f for f in FIELDS if f not in values]
    if missing:
        sys.exit('apply-storage-constants: storage-constants.py did not print ' + ', '.join(missing))
    return values


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', nargs='?', default='local/device')
    ap.add_argument('--root', type=Path, default=Path('.'))
    ap.add_argument('--write', action='store_true', help='Edit the files in place')
    args = ap.parse_args()

    values = read_values(args.directory)
    edits = [
        (args.root / 'xnuloader/loader.c', 'BLOCK_DARWIN_FIRST'),
        (args.root / 'xnuloader/loader.c', 'BLOCK_DARWIN_LAST'),
        (args.root / 'xnuloader/loader.c', 'BLOCK_DARWIN_UNIQ'),
        (args.root / 'xnu/files/bsd/dev/redwood_ufs.c', 'UFS_DARWIN_FIRST'),
        (args.root / 'xnu/files/bsd/dev/redwood_ufs.c', 'UFS_DARWIN_BLOCKS'),
    ]

    changed = {}
    for path, field in edits:
        if not path.is_file():
            sys.exit(f'apply-storage-constants: missing {path}')
        text = changed.get(path) or path.read_text()
        pattern = re.compile(r'^#define\s+' + re.escape(field) + r'\s+.*$', re.M)
        if not pattern.search(text):
            sys.exit(f'apply-storage-constants: no #define {field} in {path}')
        new = pattern.sub(f'#define {field} {values[field]}', text, count=1)
        print(f'{path}: {field} -> {values[field]}' if new != text else f'{path}: {field} already set')
        changed[path] = new

    if args.write:
        for path, text in changed.items():
            path.write_text(text)
        print('written')
    else:
        print('dry run; pass --write to edit')
    return 0


if __name__ == '__main__':
    sys.exit(main())
