#!/usr/bin/env python3
"""Identify Animal Forest ROMs, normalize them to big-endian .z64 and place them where the build expects.

Usage: scripts/prepare_rom.py <rom> [<rom>...]

Handles .z64 (big-endian), .v64/.n64 (byte-swapped) and little-endian dumps.
Each ROM is recognised by its MD5 and written to work/af/baseroms/<id>/baserom.z64:

  jp  the Japanese release (NUS-NAFJ) -- the decomp is built from it
  en  the English fan translation "Animal Forest (U) [!]", a data-only patch of jp --
      the ROM the port plays when it is given (without it, jp)

Prints one line "<id> <path>" per ROM.
"""
import hashlib
import sys
from pathlib import Path

KNOWN = {
    "a4f7c57c180297b2e7ba5a5feb44fe0b": "jp",  # baseroms/jp/checksum-compressed.md5
    "f827d11ee513d5edde44a3a9598f0934": "en",
}
ROOT = Path(__file__).resolve().parent.parent


def normalize(path):
    data = bytearray(Path(path).read_bytes())
    magic = bytes(data[:4])
    if magic == b"\x80\x37\x12\x40":
        pass
    elif magic == b"\x37\x80\x40\x12":  # .v64
        data[0::2], data[1::2] = data[1::2], data[0::2]
    elif magic == b"\x40\x12\x37\x80":  # little-endian
        data[0::4], data[1::4], data[2::4], data[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
    else:
        sys.exit(f"{path}: not an N64 ROM (header {magic.hex()})")
    return bytes(data)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        data = normalize(path)
        md5 = hashlib.md5(data).hexdigest()
        rom_id = KNOWN.get(md5)
        if rom_id is None:
            sys.exit(f"{path}: MD5 {md5} is not an Animal Forest ROM this port knows:\n" +
                     "\n".join(f"  {m}  {i}" for m, i in KNOWN.items()))
        dest = ROOT / "work/af/baseroms" / rom_id / "baserom.z64"
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_bytes() != data:
            dest.write_bytes(data)
        print(rom_id, dest)


if __name__ == "__main__":
    main()
