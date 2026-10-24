#!/usr/bin/env python3
"""Compares renderer A/B replay runs (scripts/gfx_ab.sh, psp_replay.py --keep-log DIR with
hwc_sections.txt): per scene the renderer's time a task and, from the counters, its
I-misses, D-misses and memory stall a frame (median of the windows after the first).
Usage: ab_compare.py LOGDIR_A LOGDIR_B [LOGDIR_A2 LOGDIR_B2 ...] (pairs)."""
import os
import re
import statistics
import sys

TASK = re.compile(r"gfx: render \d+% of wall time \((\d+) us a task")
HWC = re.compile(r"hwc render\s+(\d+) kcyc/f, i_miss\s+(\d+)/f, d_miss\s+(\d+)/f, d_wb\s+(\d+)/f, mem stall\s+(\d+) kcyc/f")


def scene(path):
    tasks, hwc = [], []
    for l in open(path, errors="replace"):
        m = TASK.search(l)
        if m:
            tasks.append(int(m.group(1)))
        m = HWC.search(l)
        if m:
            hwc.append([int(x) for x in m.groups()])
    tasks, hwc = tasks[1:], hwc[1:]  # the first window holds the start-up
    if not tasks or not hwc:
        return None
    return statistics.median(tasks), [statistics.median(c) for c in zip(*hwc)]


args = sys.argv[1:]
for a, b in zip(args[0::2], args[1::2]):
    print(f"{a} -> {b}")
    print("  scene       us/task A      B       d%   i_miss/f A     B     d%   d_miss d%  stall d%")
    tot = [0, 0, 0, 0]
    for name in sorted(os.listdir(a)):
        pa, pb = os.path.join(a, name), os.path.join(b, name)
        if not os.path.exists(pb):
            continue
        ra, rb = scene(pa), scene(pb)
        if ra is None or rb is None:
            continue
        (ta, ha), (tb, hb) = ra, rb
        pct = lambda x, y: 100.0 * (y - x) / x if x else 0.0
        print(f"  {name[:-4]:10s} {ta:8.0f} {tb:8.0f} {pct(ta, tb):+6.2f}  {ha[1]:8.0f} {hb[1]:8.0f} {pct(ha[1], hb[1]):+6.2f}"
              f"   {pct(ha[2], hb[2]):+6.2f}   {pct(ha[4], hb[4]):+6.2f}")
        tot[0] += ta; tot[1] += tb; tot[2] += ha[1]; tot[3] += hb[1]
    print(f"  {'total':10s} {tot[0]:8.0f} {tot[1]:8.0f} {100 * (tot[1] - tot[0]) / tot[0]:+6.2f}  "
          f"{tot[2]:8.0f} {tot[3]:8.0f} {100 * (tot[3] - tot[2]) / tot[2]:+6.2f}")
