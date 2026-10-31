#!/usr/bin/env python3
"""
latency.py RUN.log [MORE.log ...] [RECORDING.raw] [--finger N]: how far behind the finger the phone's screen is.
(More logs: the live ones of the same run, XL_FOLLOW=N XL_FOLLOW_KEEP=1, which keep what the run's log loses.)

A run with boot-arg touch_lat=1 writes a line at every stage a touch passes on its way to the panel,
all in microseconds of mach_absolute_time (the kernel's clock, which user space reads too):

  tl: p TYPE FINGER X Y at US              the kernel posts a sample (bsd/dev/redwood_input.c)
  tl: c before US seen US read US          the touch driver finds the interrupt line low and reads the
                                           report (bsd/dev/redwood_goodix.c; only for a real finger)
  tl: i TYPE posted US read US sent US     the window server reads it and hands it to the event system
                                           (userland/src/inject.c)
  tl: g FRAME began US ended US DRAWS      the window server draws a frame (userland/src/eagl.c)
  tl: s ID at US                           it hands the frame over (SwapEnd)
  tl: v ID ready US shown US done US       the display driver: the GPU done with it, its beat come, its
                                           pixels in the panel's memory (MiniIOMobileFramebuffer.cpp)

With rec_secs=... rec_ring=1 the frames themselves are in the screen recording, each with the time it
was put in the panel's memory. A "zigzag" of rwtouch (the finger up and down over a scrolling list
without letting go) then gives the latency from end to end: the list follows the finger, and the
time by which the list's movement lags the finger's is found by fitting the one to the other (a
constant offset -- the scroll view's slop before it starts to follow -- does not matter; the turns do).
--finger N reads a scripted finger instead of the real chip's finger 0 (a run with touch_auto=
gestures on finger 9). The axis a touch turns in is found per touch: y for a finger over a vertical
list, x for the scripted horizontal page drags -- the content follows along that axis, and so does
the fit.
The stages are averaged over the same frames and samples, and what is left between the event
system and the frame -- the application's handling and the wait for the next frame of
CoreAnimation's clock -- is the difference.
"""
import re
import struct
import sys

import numpy as np


def micros_lines(paths):
    """The tl: lines of the logs, once each (the kernel's own also come back in its log at the end)."""
    seen, lines = set(), []
    if isinstance(paths, str):
        paths = [paths]
    for path in paths:
      with open(path, 'rb') as f:
        for raw in f:
            text = raw.decode('utf-8', 'replace')
            at = text.find('tl: ')
            if at < 0:
                continue
            line = text[at:].strip()
            if line not in seen:
                seen.add(line)
                lines.append(line)
    return lines


def parse(lines):
    samples, reads, frames, swaps, shows, chip = [], [], [], {}, {}, []
    for line in lines:
        w = line.split()
        try:
            if w[1] == 'p' and len(w) >= 8:
                samples.append((int(w[7]), int(w[2]), int(w[3]), int(w[4]), int(w[5])))
            elif w[1] == 'i' and len(w) >= 9:
                reads.append((int(w[4]), int(w[6]), int(w[8]), int(w[2])))
            elif w[1] == 'g' and len(w) >= 8:
                frames.append((int(w[4]), int(w[6]), int(w[7])))
            elif w[1] == 's' and len(w) >= 5:
                swaps[int(w[2])] = int(w[4])
            elif w[1] == 'v' and len(w) >= 9:
                shows[int(w[2])] = (int(w[4]), int(w[6]), int(w[8]))
            elif w[1] == 'c' and len(w) >= 8:
                chip.append((int(w[3]), int(w[5]), int(w[7])))
        except ValueError:
            pass
    samples.sort()
    reads.sort()
    frames.sort(key=lambda f: f[1])
    return samples, reads, frames, swaps, shows, chip


def recording(path):
    data = open(path, 'rb').read()
    magic, width, height, count, oldest = struct.unpack_from('<8sIIII', data, 0)
    if magic != b'RWREC003':
        sys.exit('latency.py: not a recording of the RWREC003 kind')
    began = struct.unpack_from('<Q', data, 24)[0]
    size = 8 + width * height * 3
    out = []
    for k in range(count):
        i = (oldest + k) % count
        at = 32 + i * size
        t = struct.unpack_from('<Q', data, at)[0]
        pixels = np.frombuffer(data, np.uint8, width * height * 3, at + 8).reshape(height, width, 3)
        out.append(((began + t) // 1000, pixels))
    out.sort(key=lambda f: f[0])
    return out, width, height


def zigzags(samples, finger=0):
    """Touches with many moves on one finger: (down time, up time, axis, [(t, x, y)]).

    Finger 0 is the touch chip's own; a scripted run (touch_auto=) gestures on
    another. The axis is the coordinate the touch turns in -- y for a finger
    over a vertical list, x for the scripted horizontal page drags; the content
    follows along that axis, and so does the fit."""
    out, current = [], None
    for t, kind, f, x, y in samples:
        if f != finger:
            continue
        if kind == 1:
            current = [(t, x, y)]
        elif kind == 2 and current is not None:
            current.append((t, x, y))
        elif kind == 3 and current is not None:
            current.append((t, x, y))
            if len(current) > 30:
                for axis in (1, 0):
                    v = [p[1 + axis] for p in current]
                    turns = sum(1 for i in range(1, len(v) - 1) if (v[i] - v[i - 1]) * (v[i + 1] - v[i]) < 0)
                    if turns >= 3:
                        out.append((current[0][0], t, axis, current))
                        break
            current = None
    return out


def shift(a, b, most):
    """How far b's content has moved down from a's, in rows: the shift with the least difference."""
    h = a.shape[0]
    best, best_cost = 0, None
    for s in range(-most, most + 1):
        if s >= 0:
            pa, pb = a[0:h - s], b[s:h]
        else:
            pa, pb = a[-s:h], b[0:h + s]
        cost = np.mean(np.abs(pa - pb))
        if best_cost is None or cost < best_cost:
            best, best_cost = s, cost
    return best


def content_offsets(frames, t0, t1, height, width, axis=1):
    """The content's position in each frame between t0 and t1, relative to the first:
    in rows of the recording for a vertical list (axis 1), in columns for a
    horizontal page (axis 0)."""
    chosen = [(t, p) for t, p in frames if t0 <= t <= t1]
    out = []
    if not chosen:
        return out
    if axis == 1:
        rows = (int(height * 0.15), int(height * 0.90))
        cols = (int(width * 0.10), int(width * 0.90))
    else:
        rows = (int(height * 0.10), int(height * 0.90))
        cols = (int(width * 0.15), int(width * 0.90))
    previous, offset = None, 0
    for t, pixels in chosen:
        grey = pixels[rows[0]:rows[1], cols[0]:cols[1], :].astype(np.float32).mean(axis=2)
        if axis == 0:
            grey = grey.T                       # columns become rows: shift() finds the page's columns
        if previous is not None:
            offset += shift(previous, grey, 48)
        out.append((t, offset))
        previous = grey
    return out


def fit_lag(offsets, touch, span, axis=1):
    """The lag (us) at which the content best follows the finger's: least squares with an offset
    and a gain. axis 1 fits the finger's y over the height, axis 0 its x over the width."""
    tt = np.array([p[0] for p in touch], dtype=np.float64)
    ty = np.array([p[1 + axis] for p in touch], dtype=np.float64) / 65535.0 * span
    ft = np.array([o[0] for o in offsets], dtype=np.float64)
    fy = np.array([o[1] for o in offsets], dtype=np.float64)
    best = None
    for lag in range(0, 250001, 500):
        at = ft - lag
        keep = (at >= tt[0]) & (at <= tt[-1])
        if keep.sum() < 10:
            continue
        x = np.interp(at[keep], tt, ty)
        y = fy[keep]
        matrix = np.vstack([x, np.ones_like(x)]).T
        coefficients, residual, _, _ = np.linalg.lstsq(matrix, y, rcond=None)
        error = float(np.mean((matrix @ coefficients - y) ** 2))
        if best is None or error < best[1]:
            best = (lag, error, coefficients[0], int(keep.sum()))
    return best


def mean(values):
    return sum(values) / len(values) if values else float('nan')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    argv = sys.argv[1:]
    finger = 0
    while '--finger' in argv:
        at = argv.index('--finger')
        finger = int(argv[at + 1])
        argv = argv[:at] + argv[at + 2:]
    logs = [a for a in argv if not a.endswith('.raw')]
    raws = [a for a in argv if a.endswith('.raw')]
    samples, reads, frames, swaps, shows, chip = parse(micros_lines(logs))
    print(f'latency.py: {len(samples)} samples, {len(reads)} read by the window server, {len(frames)} frames drawn, '
          f'{len(swaps)} handed over, {len(shows)} shown; {len(chip)} reports read from the touch chip')
    if chip:
        found = [seen - before for before, seen, read in chip if before]
        took = [read - seen for before, seen, read in chip]
        print(f'  touch chip: looked at every {mean(found) / 1000:.2f} ms (a report waits half of that on average); '
              f'reading a report takes {mean(took) / 1000:.2f} ms')
    pictures, width, height = recording(raws[0]) if raws else ([], 0, 0)
    totals = []
    for number, (down, up, axis, touch) in enumerate(zigzags(samples, finger)):
        moves = [p for p in touch[1:-1]]
        print(f'zigzag {number + 1}: {len(touch)} samples over {(up - down) / 1000:.0f} ms, '
              f'{"horizontal" if axis == 0 else "vertical"}')
        result = None
        if pictures:
            offsets = content_offsets(pictures, down, up + 400000, height, width, axis)
            result = fit_lag(offsets, moves, width if axis == 0 else height, axis) \
                if len(offsets) > 10 else None
        if result is None:
            print('  (no frames of it in the recording)')
            continue
        lag, error, gain, used = result
        print(f'  END TO END: the list follows the finger {lag / 1000:.1f} ms behind, until its pixels are in the panel\'s '
              f'memory (gain {gain:.2f}, misfit {error ** 0.5:.1f} rows, {used} frames)')
        totals.append(lag)
        # the stages, over the samples and frames of this zigzag
        within = [r for r in reads if down <= r[0] <= up]
        inject = [r[1] - r[0] for r in within]
        send = [r[2] - r[1] for r in within]
        rows = []
        for sid in sorted(shows):
            if sid not in swaps or not (down <= shows[sid][2] <= up + 400000):
                continue
            swap = swaps[sid]
            drawn = [f for f in frames if f[1] <= swap]
            if not drawn:
                continue
            began, ended, draws = drawn[-1]
            ready, shown, done = shows[sid]
            rows.append((ended - began, swap - ended, ready - swap, shown - ready, done - shown))
        if rows:
            render, handover, gpu, beat, copy = (mean([r[k] for r in rows]) for k in range(5))
            rest = lag - mean(inject) - mean(send) - render - handover - gpu - beat - copy
            print(f'  kernel to window server (the touch device read):   {mean(inject) / 1000:6.2f} ms')
            print(f'  window server to event system:                      {mean(send) / 1000:6.2f} ms')
            print(f'  the application, and the wait for the next frame:   {rest / 1000:6.2f} ms  (what is left)')
            print(f'  the window server drawing the frame (GPU commands):  {render / 1000:6.2f} ms')
            print(f'  frame drawn to frame handed over (SwapEnd):         {handover / 1000:6.2f} ms')
            print(f'  waiting for the GPU to finish it:                   {gpu / 1000:6.2f} ms')
            print(f'  waiting for the display driver\'s beat:              {beat / 1000:6.2f} ms')
            print(f'  copying (scaling) it into the panel\'s memory:       {copy / 1000:6.2f} ms')
            print(f'  ({len(rows)} frames, {len(within)} samples; then the panel\'s next scan of those rows, '
                  f'0 to 8.3 ms at 120 Hz)')
    if totals:
        print(f'all zigzags: {mean(totals) / 1000:.1f} ms behind the finger on average')


if __name__ == '__main__':
    main()
