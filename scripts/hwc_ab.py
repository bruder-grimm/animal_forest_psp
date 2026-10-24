#!/usr/bin/env python3
"""Compares two runs of the same resumed capture (A and B mode, hwc_sections.txt): per owner the
mean cycles, I-misses, D-misses and memory stall a frame over the windows after the first, and the
game's time a frame and fps from the stats lines. Usage: hwc_ab.py LOG_A LOG_B"""
import re
import statistics
import sys

HWC = re.compile(r"hwc (\S+)\s+(\d+) kcyc/f, i_miss\s+(\d+)/f, d_miss\s+(\d+)/f, d_wb\s+(\d+)/f, mem stall\s+(\d+) kcyc/f")
FRAME = re.compile(r"\[\s*([\d.]+)\] frame (\d+) \(poll (\d+)\).*?busy \d+% = (\d+) us a frame")


def load(path):
    owners, frames, cur = {}, [], {}
    for l in open(path, errors="replace"):
        m = HWC.search(l)
        if m:
            cur[m.group(1)] = [int(x) for x in m.groups()[1:]]
        m = FRAME.search(l)
        if m:
            frames.append((float(m.group(1)), int(m.group(2)), int(m.group(4))))
            if cur:
                for o, v in cur.items():
                    owners.setdefault(o, []).append(v)
            cur = {}
    owners = {o: [statistics.mean(c) for c in zip(*v[1:])] for o, v in owners.items() if len(v) > 1}
    fps = [(b[1] - a[1]) / (b[0] - a[0]) for a, b in zip(frames[1:], frames[2:])]
    busy = [f[2] for f in frames[2:]]
    return owners, statistics.mean(fps) if fps else 0, statistics.mean(busy) if busy else 0


a, b = load(sys.argv[1]), load(sys.argv[2])
print(f"fps {a[1]:.2f} -> {b[1]:.2f}   busy us a frame {a[2]:.0f} -> {b[2]:.0f}")
print("owner     Mcyc A   B      d%    i_miss A    B      d%    d_miss d%   stall d%")
pct = lambda x, y: 100.0 * (y - x) / x if x else 0.0
game = [[0] * 5, [0] * 5]
for o in sorted(set(a[0]) & set(b[0])):
    x, y = a[0][o], b[0][o]
    if x[0] < 30:
        continue
    print(f"{o:8s} {x[0] / 1000:6.2f} {y[0] / 1000:6.2f} {pct(x[0], y[0]):+6.2f}  {x[1]:8.0f} {y[1]:8.0f} {pct(x[1], y[1]):+6.2f}"
          f"  {pct(x[2], y[2]):+6.2f}  {pct(x[4], y[4]):+6.2f}")
    if o.startswith("t"):
        for i in range(5):
            game[0][i] += x[i]; game[1][i] += y[i]
x, y = game
print(f"{'game':8s} {x[0] / 1000:6.2f} {y[0] / 1000:6.2f} {pct(x[0], y[0]):+6.2f}  {x[1]:8.0f} {y[1]:8.0f} {pct(x[1], y[1]):+6.2f}"
      f"  {pct(x[2], y[2]):+6.2f}  {pct(x[4], y[4]):+6.2f}")
