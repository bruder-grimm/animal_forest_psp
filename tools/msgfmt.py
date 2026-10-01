#!/usr/bin/env python3
"""Load Animal Forest (N64) and Animal Crossing (GC BMG) message banks."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import afrom  # noqa: E402

CTRL = 0x7F
# GC control code sizes (ac-decomp mFont_cont_info_tbl), including the 0x7F byte.
GC_SIZES = [2, 2, 2, 3, 2, 5, 2, 2, 5, 5, 5, 5, 5, 2, 4, 4, 4, 4, 4, 6, 8, 10, 6, 8, 10, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 6, 3, 3, 3, 3, 2, 4, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 6, 3, 3, 4, 3, 2,
            2, 6, 2, 2, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 4, 4, 12, 14]

AF_MSG_DATA, AF_MSG_TABLE = 1883, 1884


def af_messages(rom_path):
    rom = afrom.load(rom_path)
    e = afrom.entries(rom)
    data = afrom.read(rom, e[AF_MSG_DATA])
    tbl = afrom.read(rom, e[AF_MSG_TABLE])
    ends = struct.unpack(f">{len(tbl) // 4}I", tbl)
    out, start = [], 0
    for end in ends:
        out.append(data[start:end])
        start = end
    return out


def gc_messages(path):
    d = open(path, "rb").read()
    assert d[:8] == b"MESGbmg1"
    p = 0x20
    offs = dat = None
    while p < len(d) and d[p:p + 4] in (b"INF1", b"DAT1"):
        size = struct.unpack_from(">I", d, p + 4)[0]
        if d[p:p + 4] == b"INF1":
            n, esz = struct.unpack_from(">HH", d, p + 8)
            offs = [struct.unpack_from(">I", d, p + 0x10 + i * esz)[0] for i in range(n)]
            # string.bin's INF1 size counts the file header too: find DAT1 itself
            p = d.index(b"DAT1", p + 0x10 + n * esz)
            continue
        dat = d[p + 8:p + size]
        p += size
    out = []
    for i, o in enumerate(offs):
        nxt = min((x for x in offs[i + 1:] if x > o), default=len(dat)) if o else o
        out.append(dat[o:nxt] if o or i == 0 else b"")
    return out


def tokens(msg, sizes=GC_SIZES):
    """Split a message into ('c', bytes) control codes and ('t', bytes) text runs."""
    out, i, run = [], 0, bytearray()
    while i < len(msg):
        b = msg[i]
        if b == CTRL and i + 1 < len(msg):
            if run:
                out.append(("t", bytes(run))); run = bytearray()
            c = msg[i + 1]
            n = sizes[c] if c < len(sizes) else 2
            out.append(("c", msg[i:i + n]))
            i += n
        elif b == 0x80 and sizes is GC_SIZES:
            if run:
                out.append(("t", bytes(run))); run = bytearray()
            out.append(("g", msg[i:i + 2])); i += 2
        else:
            run.append(b); i += 1
    if run:
        out.append(("t", bytes(run)))
    return out
