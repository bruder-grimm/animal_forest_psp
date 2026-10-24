#!/usr/bin/env python3
"""Summarise the hardware counter lines of a run with hwc_sections.txt (runtime/src/hwcount.c)
over the same stretches as perf_summary.py: per owner (game thread, renderer, idle) a frame's
CPU cycles, I- and D-cache misses, writebacks and memory stall cycles, and an estimate of the
stall each kind of miss accounts for (cycles a miss measured by rt_hwc_bench at 222 MHz).
Usage: hwc_summary.py LOG..."""
import re
import sys

STRETCHES = [("train", 4800, 7400), ("arrival", 11800, 13000), ("village", 13400, 15500)]
I_MISS_CYC, D_MISS_CYC, WB_CYC = 55, 68, 34  # from the benchmarks (a dirty miss ~102 = 68 + 34)

LINE = re.compile(r"hwc (\S+)\s+(\d+) kcyc/f, i_miss\s+(\d+)/f, d_miss\s+(\d+)/f, d_wb\s+(\d+)/f, "
                  r"mem stall\s+(\d+) kcyc/f")
FRAME = re.compile(r"frame (\d+) \(poll (\d+)\)")

for path in sys.argv[1:]:
    pending = []
    blocks = []  # (poll, {owner: (kcyc, im, dm, wb, stall_kcyc)})
    for l in open(path, errors="replace"):
        m = LINE.search(l)
        if m:
            pending.append((m.group(1), tuple(int(x) for x in m.groups()[1:])))
            continue
        m = FRAME.search(l)
        if m and pending:
            blocks.append((int(m.group(2)), dict(pending)))
            pending = []
    print(path)
    for name, lo, hi in STRETCHES:
        sel = [b for p, b in blocks if lo <= p <= hi]
        if not sel:
            print(f"  {name}: no windows")
            continue
        owners = sorted({o for b in sel for o in b}, key=lambda o: (o in ("render", "idle"), o))
        print(f"  {name} ({len(sel)} windows), a frame:")
        print("    owner     Mcyc   i_miss  d_miss   d_wb  stall%  of which I / D / WB (est.)")
        for o in owners:
            v = [sum(b.get(o, (0,) * 5)[i] for b in sel) / len(sel) for i in range(5)]
            kcyc, im, dm, wb, st = v
            if kcyc < 50:
                continue
            est = [im * I_MISS_CYC, dm * D_MISS_CYC, wb * WB_CYC]
            pct = lambda x: 100 * x / (kcyc * 1000) if kcyc else 0
            print(f"    {o:8s} {kcyc / 1000:6.2f} {im:8.0f} {dm:7.0f} {wb:6.0f}  {pct(st * 1000):5.1f}%  "
                  f"{pct(est[0]):4.1f} / {pct(est[1]):4.1f} / {pct(est[2]):4.1f}")
