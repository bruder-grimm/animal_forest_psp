#!/usr/bin/env python3
"""Animal Forest N64 ROM access: dmadata table and (Yaz0-decompressed) files."""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from rarc import yaz0  # noqa: E402

DMADATA = 0x19D40


def load(path):
    rom = open(path, "rb").read()
    if rom[:4] == b"\x37\x80\x40\x12":
        b = bytearray(rom); b[0::2], b[1::2] = b[1::2], b[0::2]; rom = bytes(b)
    return rom


def entries(rom):
    out = []
    p = DMADATA
    while True:
        vs, ve, ps, pe = struct.unpack_from(">IIII", rom, p)
        if vs == ve == ps == pe == 0:
            break
        out.append((vs, ve, ps, pe))
        p += 16
    return out


def read(rom, e):
    vs, ve, ps, pe = e
    if pe == 0:
        return rom[ps:ps + ve - vs]
    return yaz0(rom[ps:pe])
