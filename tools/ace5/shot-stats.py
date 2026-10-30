#!/usr/bin/env python3
"""Black / saturated / mean per screenshot, grouped by run and in order.

The control-centre defect is run-to-run variable (a 4.0 % black frame and a
51.4 % one came from the same build), so a single before/after screenshot
proves nothing. This prints the series a `--shots` run produces, which is what
separates "the picture drifted" from "the boot started bad".

    python3 tools/ace5/shot-stats.py logs/giulia/20261030-*screen*.png
    python3 tools/ace5/shot-stats.py logs/giulia            # every shot there

Counts use max(r,g,b) < 24 for black (the same threshold the defect reports
used) and max-min > 80 with max > 120 for "saturated", which is how the colour
noise stands out from intended colour.
"""
import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:                     # pragma: no cover - the host has both
    sys.exit('needs PIL and numpy: pip install pillow numpy')


def groups(paths):
    """Group by run: the file name up to '-screen', with the shot index kept."""
    runs = {}
    for path in paths:
        m = re.match(r'^(\d{8}-\d{6})-screen(?:-(\d{3}))?\.png$', path.name)
        key = m.group(1) if m else path.stem
        index = int(m.group(2)) if (m and m.group(2)) else 0
        runs.setdefault(key, []).append((index, path))
    for key in runs:
        runs[key].sort(key=lambda item: item[0])
    # Shots taken before phone.py named them with the run's prefix carry the time
    # of the shot itself, so each looks like its own run. One series, in order.
    if len(runs) > 1 and all(len(shots) == 1 for shots in runs.values()):
        merged = sorted((item for shots in runs.values() for item in shots), key=lambda i: i[1].name)
        return {merged[0][1].name.split('-screen')[0]: [(n + 1, p) for n, (_, p) in enumerate(merged)]}
    return runs


def stats(path):
    a = np.asarray(Image.open(path).convert('RGB')).astype(np.int16)
    dark = (a.max(axis=2) < 24).mean() * 100.0
    saturation = a.max(axis=2) - a.min(axis=2)
    saturated = ((saturation > 80) & (a.max(axis=2) > 120)).mean() * 100.0
    return dark, saturated, float(a.mean()), a


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+', help='PNG files, a directory, or a glob')
    ap.add_argument('--diff', type=float, default=8.0,
                    help='per-pixel threshold for the "differs from the first shot" column (default 8)')
    args = ap.parse_args()

    found = []
    for pattern in args.paths:
        path = Path(pattern)
        if path.is_dir():
            found += sorted(path.glob('*-screen*.png'))
        else:
            found += sorted(Path().glob(pattern)) if any(c in pattern for c in '*?[') else [path]
    found = [p for p in dict.fromkeys(found) if p.is_file()]
    if not found:
        sys.exit('no screenshots matched')

    runs = groups(found)
    for key in sorted(runs):
        shots = runs[key]
        first = None
        print('%s  (%d shot%s)' % (key, len(shots), '' if len(shots) == 1 else 's'))
        print('  %-4s %-34s %7s %7s %7s %10s' % ('#', 'file', 'black%', 'sat%', 'mean', 'vs first'))
        for index, path in shots:
            dark, saturated, mean, pixels = stats(path)
            if first is None:
                first, delta = pixels, '—'
            else:
                d = np.abs(pixels - first).max(axis=2)
                delta = '%.2f%%' % ((d > args.diff).mean() * 100.0)
            print('  %-4s %-34s %7.1f %7.2f %7.1f %10s' % (
                '%03d' % index if index else '-', path.name[-34:], dark, saturated, mean, delta))
        print()

    if len(runs) > 1:
        print('last shot of each run (the comparison "4 % vs 51 %" wanted):')
        for key in sorted(runs):
            index, path = runs[key][-1]
            dark, saturated, mean, _ = stats(path)
            print('  %s  black %5.1f%%  sat %5.2f%%  mean %5.1f  %s' % (
                key, dark, saturated, mean, path.name))


if __name__ == '__main__':
    main()
