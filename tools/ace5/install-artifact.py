#!/usr/bin/env python3
"""Install a downloaded ace5-build artifact into the repository layout.

    gh run download <run-id> --repo <owner>/HackiOS-Ace5 -n ace5-build -D local/ci-artifact
    python3 tools/ace5/install-artifact.py local/ci-artifact

The artifact keeps the paths it was uploaded with (out/..., local/...).
system-metal.apfs was split into 900 MiB parts for upload; this joins them.
Existing destination files are kept unless --force is given.
"""
import argparse
import shutil
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('artifact', type=Path)
ap.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
ap.add_argument('--force', action='store_true', help='replace existing destination files')
args = ap.parse_args()

src = args.artifact.resolve()
repo = args.repo.resolve()
if not src.is_dir():
    raise SystemExit(f'not a directory: {src}')

parts = sorted(src.rglob('system-metal.apfs.part-*'))
if not parts:
    print('note   no system-metal.apfs parts (kernel-only artifact); skipping the system image')


def install(source: Path, dest: Path) -> None:
    if dest.exists() and not args.force:
        print(f'keep   {dest} (exists; --force to replace)')
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
    print(f'copy   {source.relative_to(src)} -> {dest}')


system_image = repo / 'local/system-metal.apfs'
if not parts:
    pass
elif system_image.exists() and not args.force:
    print(f'keep   {system_image} (exists; --force to replace)')
else:
    system_image.parent.mkdir(parents=True, exist_ok=True)
    temporary = system_image.with_suffix('.apfs.tmp')
    with open(temporary, 'wb') as out:
        for part in parts:
            with open(part, 'rb') as data:
                shutil.copyfileobj(data, out, 1 << 20)
    temporary.replace(system_image)
    print(f'join   {len(parts)} parts -> {system_image} ({system_image.stat().st_size} bytes)')

for item in src.rglob('*'):
    if not item.is_file() or item.name.startswith('system-metal.apfs.part-'):
        continue
    relative = item.relative_to(src)
    if relative.parts[0] not in ('out', 'local'):
        continue
    install(item, repo / relative)

MACHO = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe'}
checks = [
    ('out/giulia-kernel/mach.ace5-public', 0, MACHO, 'Mach-O', True),
    ('out/giulia-kernel/hackios-giulia-kernel.img', 0, {b'ANDROID!'}, 'Android boot', True),
    ('out/giulia/hackios-giulia-diagnostic.img', 0, {b'ANDROID!'}, 'Android boot', True),
    ('local/root-daily.apfs', 32, {b'NXSB'}, 'APFS', True),
    ('local/system-metal.apfs', 32, {b'NXSB'}, 'APFS', bool(parts)),
]
for name, offset, accepted, label, required in checks:
    path = repo / name
    if not path.is_file():
        if required:
            raise SystemExit(f'missing {path}')
        print(f'skip   {name}: not in this artifact')
        continue
    with open(path, 'rb') as handle:
        handle.seek(offset)
        magic = handle.read(len(next(iter(accepted))))
    if magic not in accepted:
        raise SystemExit(f'{path}: expected {label} magic at {offset}, got {magic!r}')
    print(f'ok     {name}: {label}')
