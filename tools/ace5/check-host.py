#!/usr/bin/env python3
"""Check the host tools needed for the Ace 5 build and phone steps.

This is a report, not a gate: missing tools on Windows are expected. Run it on
the Linux ARM64 build host to see what still has to be installed.
"""
import shutil
import subprocess
import sys
from pathlib import Path


def working(name):
    path = shutil.which(name)
    if not path:
        return None
    try:
        subprocess.run([path, '-c', 'import sys'], check=True, capture_output=True)
        return path
    except Exception:
        return None

CHECKS = [
    ('git', 'source checkout'),
    ('clang', 'XNU, loader, userland'),
    ('lld-link', 'loader'),
    ('llvm-objcopy', 'boot shim'),
    ('7z', 'IPSW/APFS images (7-Zip 23+)'),
    ('dtc', 'device tree'),
    ('adb', 'backup and hardware extraction'),
    ('fastboot', 'phone boot/flash'),
    ('make', 'XNU build'),
]


def main() -> int:
    print(f'host: {sys.platform} / python {sys.version.split()[0]}')
    missing = 0
    for name, why in CHECKS:
        found = shutil.which(name)
        print(f'[{"ok" if found else "missing"}] {name:<14} {why}')
        missing += 0 if found else 1

    python3 = working('python3') or working('python')
    print(f'[{"ok" if python3 else "missing"}] python3        scripts')
    print()

    root = Path(__file__).resolve().parents[2]
    theos = Path.home() / 'theos/sdks/iPhoneOS16.5.sdk'
    print(f'[{"ok" if theos.is_dir() else "missing"}] {theos} (XNU SDK)')
    print(f'[{"ok" if (root / "xnu-src").is_dir() else "not built"}] xnu-src (run xnu/apply.sh)')
    print()
    print('Windows can run the dry-runs and constant checks; the build and phone steps need Linux ARM64.')
    return 0 if missing == 0 else 0


if __name__ == '__main__':
    sys.exit(main())
