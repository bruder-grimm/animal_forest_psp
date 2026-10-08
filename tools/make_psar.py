#!/usr/bin/env python3
"""Packs the port's data files into the EBOOT's DATA.PSAR section.

  make_psar.py <out> <file>...

Layout (read by runtime/src/files.c): "AFDT", u32 count, then per file a
24-byte name, u32 offset from the section's start and u32 size (little-endian),
then the files, each on a 16-byte boundary.
"""
import os
import struct
import sys


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    files = [(os.path.basename(p).encode(), open(p, "rb").read()) for p in sys.argv[2:]]
    pos = 8 + 32 * len(files)
    table, body = b"", b""
    for name, data in files:
        if len(name) > 23:
            raise SystemExit(f"{name.decode()}: name longer than 23 characters")
        pad = -pos % 16
        body += bytes(pad) + data
        table += struct.pack("<24sII", name, pos + pad, len(data))
        pos += pad + len(data)
    with open(sys.argv[1], "wb") as f:
        f.write(b"AFDT" + struct.pack("<I", len(files)) + table + body)


if __name__ == "__main__":
    main()
