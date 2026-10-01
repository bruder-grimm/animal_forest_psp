#!/usr/bin/env python3
"""Identify Animal Forest ROMs, normalize them to big-endian .z64 and place them where the build expects.

Usage: scripts/prepare_rom.py <rom> [<rom>...]
       scripts/prepare_rom.py --list          the dumps that are recognised

A ROM is recognised by its content, not by its name or its byte order: .z64
(big-endian), .v64 (byte-swapped) and .n64 (little-endian) dumps all give the same
image, and so does a dump with anything after the first 16 MB (tools pad some to 32 or
64 MB): only those 16 MB are used. Each is written to work/af/baseroms/<id>/baserom.z64:

  jp  the Japanese release (NUS-NAFJ, the only retail version) -- the decomp is built
      from it
  en  the English fan translation "Animal Forest (U) [!]", a data-only patch of jp --
      the ROM the port plays when it is given (without it, jp)

Prints one line "<id> <path>" per ROM.
"""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_SIZE = 16 * 1024 * 1024
MAX_SIZE = 64 * 1024 * 1024

# The MD5 of each dump the build can use, in every byte order it circulates in. The
# Japanese game was only ever released once (cartridge version 1.0), so these are all
# the hashes a good dump of it can have; a dump that matches none of them is damaged
# or modified. (md5 of the .z64 is what the decomp's baseroms/jp/checksum-compressed.md5 has.)
ROMS = {
    "jp": {
        "name": "Doubutsu no Mori (Japan)",
        "md5": {
            "z64": "a4f7c57c180297b2e7ba5a5feb44fe0b",
            "v64": "a6ef34bd225f22bbf737d61b839ae1b0",
            "n64": "5a6a8590b4a318b7374a6d216c5914c6",
        },
    },
    "en": {
        "name": "Animal Forest (U) [!], the English fan translation",
        "md5": {
            "z64": "f827d11ee513d5edde44a3a9598f0934",
            "v64": "dce0ba571c24472dbc41f6a5b5ae4317",
            "n64": "c8ecffb86bc622dbbe6a4280cb36b645",
        },
    },
}
KNOWN = {rom["md5"]["z64"]: rom_id for rom_id, rom in ROMS.items()}

# Recognised but of no use, by the MD5 of the big-endian image, with the reason.
UNUSABLE = {
    "01bdcc854d0ab798500e7bc31a24d94f":
        "the 32 MB English ROM (game code NAFE): a different build of the game, not the "
        "fan translation of the Japanese one this port needs",
    "d7ae64f2f47a9fa3f87686a3c5ce09af":
        "the decompressed ROM that the zeldaret/af decomp builds: the compressed original "
        "dump is needed",
}

MAGIC = {b"\x80\x37\x12\x40": "z64", b"\x37\x80\x40\x12": "v64", b"\x40\x12\x37\x80": "n64"}


class Result:
    """What a file is: id is "jp" or "en" for a usable ROM (image holds it), else None."""

    def __init__(self, rom_id, text, image=None):
        self.id, self.text, self.image = rom_id, text, image


def normalize(data, order):
    if order == "z64":
        return data
    b = bytearray(data)
    if order == "v64":
        b[0::2], b[1::2] = b[1::2], b[0::2]
    else:
        b[0::4], b[1::4], b[2::4], b[3::4] = b[3::4], b[2::4], b[1::4], b[0::4]
    return bytes(b)


def is_n64_rom(path):
    """Does the file start like an N64 ROM (in any byte order)?"""
    with open(path, "rb") as f:
        return f.read(4) in MAGIC


def identify(path):
    path = Path(path)
    size = path.stat().st_size
    with open(path, "rb") as f:
        order = MAGIC.get(f.read(4))
        if order is None:
            return Result(None, "not an N64 ROM")
        if size < 1024 * 1024 or size > MAX_SIZE or size % 4:
            return Result(None, f"an N64 ROM header, but {size} bytes is not the size of a ROM dump")
        f.seek(0)
        image = normalize(f.read(), order)
    title = image[0x20:0x34].decode("ascii", "replace").strip()
    md5 = hashlib.md5(image).hexdigest()
    note = f".{order} byte order"
    if md5 not in KNOWN and md5 not in UNUSABLE and size > ROM_SIZE:
        # padded out to a larger size: the ROM is the first 16 MB, whatever follows
        if hashlib.md5(image[:ROM_SIZE]).hexdigest() in KNOWN:
            image = image[:ROM_SIZE]
            md5 = hashlib.md5(image).hexdigest()
            note += f", {size // 1024 // 1024} MB with padding after the ROM"
    if md5 in KNOWN:
        rom_id = KNOWN[md5]
        return Result(rom_id, f"{ROMS[rom_id]['name']} ({note})", image)
    if md5 in UNUSABLE:
        return Result(None, f"{UNUSABLE[md5]} ({note})")
    return Result(None, f'an N64 ROM called "{title}" that this port does not know (MD5 {md5}, {note})')


def install(rom_id, image):
    """Write the image to work/af/baseroms/<id>/baserom.z64 (if it is not already there)."""
    dest = ROOT / "work/af/baseroms" / rom_id / "baserom.z64"
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists() or dest.read_bytes() != image:
        dest.write_bytes(image)
    return dest


def known_table():
    lines = []
    for rom in ROMS.values():
        lines.append(rom["name"])
        lines.extend(f"  {md5}  .{order}" for order, md5 in rom["md5"].items())
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    if args == ["--list"]:
        print(known_table())
        return
    if not args:
        sys.exit(__doc__)
    for path in args:
        result = identify(path)
        if result.id is None:
            sys.exit(f"{path}: {result.text}\nThe ROMs this port knows (MD5 of the file):\n{known_table()}")
        print(result.id, install(result.id, result.image))


if __name__ == "__main__":
    main()
