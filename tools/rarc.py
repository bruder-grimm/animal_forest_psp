#!/usr/bin/env python3
"""Unpack a (optionally Yaz0-compressed) RARC archive: rarc.py in.arc outdir"""
import os
import struct
import sys


def yaz0(d):
    if d[:4] != b"Yaz0":
        return d
    size = struct.unpack_from(">I", d, 4)[0]
    out = bytearray()
    p = 16
    while len(out) < size:
        code = d[p]; p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(d[p]); p += 1
            else:
                b1, b2 = d[p], d[p + 1]; p += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[p] + 0x12; p += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def unpack(d, outdir):
    d = yaz0(d)
    assert d[:4] == b"RARC"
    data_off = struct.unpack_from(">I", d, 0xC)[0] + 0x20
    info = 0x20
    nnodes, node_off, nents, ent_off, _, str_off = struct.unpack_from(">IIIIII", d, info)
    node_off += info; ent_off += info; str_off += info

    def name(o):
        return d[str_off + o:d.index(b"\0", str_off + o)].decode("shift_jis")

    def walk(ni, path):
        _, noff, _, cnt, first = struct.unpack_from(">4sIHHI", d, node_off + ni * 16)
        for e in range(first, first + cnt):
            _, _, flags, _, n, a, b, _ = struct.unpack_from(">HHBBHIII", d, ent_off + e * 20)
            nm = name(n)
            if nm in (".", ".."):
                continue
            if flags & 2:
                walk(a, os.path.join(path, nm))
            else:
                os.makedirs(path, exist_ok=True)
                open(os.path.join(path, nm), "wb").write(d[data_off + a:data_off + a + b])

    walk(0, os.path.join(outdir, name(struct.unpack_from(">I", d, node_off + 4)[0])))


if __name__ == "__main__":
    unpack(open(sys.argv[1], "rb").read(), sys.argv[2])
