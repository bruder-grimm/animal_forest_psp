#!/usr/bin/env python3
"""Rename the town and/or a player in an Animal Forest (N64) save, i.e. the
port's flash.bin.

Save layout (worked out from the game's m_flashrom code and real saves):
  * flash.bin is 128 KB: two identical 64 KB copies (the game keeps a backup).
  * Each copy is valid when its big-endian 16-bit words add up to 0 (mod 2^16);
    the 16-bit checksum word lives at +0x12 of the copy (header: +4 "NAFJ",
    +8 town id, +0xA save time, +0x12 checksum).
  * Names are 6 bytes, padded with spaces; letters are plain ASCII in the
    game's font (tools/afcharset.py). The player/town pair (PersonalID_c:
    name, town, player id, town id) is copied all over the save (letters,
    villagers' memories of the player, house owner...), and the game compares
    those copies, so EVERY occurrence has to change, not just the first.

Usage: af_save_rename.py IN.bin OUT.bin [--town OLD=NEW] [--player OLD=NEW]
Nothing is written unless every old name is found and both copies were valid.
"""
import argparse
import struct
import sys

COPY = 0x10000
CHECK_AT = 0x12
NAME_LEN = 6
NO_ASCII = set("#$*+/;[\\]^`{|}~")  # the font keeps kana/symbols here


def words_sum(buf):
    return sum(struct.unpack(">%dH" % (len(buf) // 2), buf)) & 0xFFFF


def fix_checksum(copy):
    copy[CHECK_AT:CHECK_AT + 2] = b"\0\0"
    copy[CHECK_AT:CHECK_AT + 2] = struct.pack(">H", -words_sum(copy) & 0xFFFF)


def name_bytes(name):
    if not 1 <= len(name) <= NAME_LEN:
        sys.exit("'%s': names are 1-%d characters" % (name, NAME_LEN))
    bad = [c for c in name if not (0x20 <= ord(c) < 0x7F) or c in NO_ASCII]
    if bad:
        sys.exit("'%s': %s not in the game's font as ASCII" % (name, "".join(bad)))
    return name.ljust(NAME_LEN).encode("ascii")


def replace_all(copy, old, new):
    """Replaces every occurrence of the padded name; returns their offsets."""
    offsets, pos = [], copy.find(old)
    while pos >= 0:
        copy[pos:pos + NAME_LEN] = new
        offsets.append(pos)
        pos = copy.find(old, pos + NAME_LEN)
    return offsets


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("src")
    parser.add_argument("dst")
    parser.add_argument("--town", metavar="OLD=NEW")
    parser.add_argument("--player", metavar="OLD=NEW")
    args = parser.parse_args()

    image = bytearray(open(args.src, "rb").read())
    if len(image) != 2 * COPY:
        sys.exit("%s is %d bytes, expected %d" % (args.src, len(image), 2 * COPY))
    copies = [image[:COPY], image[COPY:]]
    for i, c in enumerate(copies):
        if words_sum(c) != 0 or c[4:8] != b"NAFJ":
            sys.exit("copy %d of %s is not a valid save (sum %04x, tag %r)"
                     % (i, args.src, words_sum(c), bytes(c[4:8])))
    if copies[0] != copies[1]:
        print("warning: the two copies differ; the game uses the newer one", file=sys.stderr)

    for what, spec in (("town", args.town), ("player", args.player)):
        if not spec:
            continue
        old, _, new = spec.partition("=")
        old_b, new_b = name_bytes(old), name_bytes(new)
        for i, c in enumerate(copies):
            offsets = replace_all(c, old_b, new_b)
            if not offsets:
                sys.exit("%s '%s' not found in copy %d: nothing written" % (what, old, i))
            print("%s '%s' -> '%s': %d places in copy %d (%s)" % (
                what, old, new, len(offsets), i,
                " ".join("%04x" % o for o in offsets)))

    for c in copies:
        fix_checksum(c)
        assert words_sum(c) == 0
    open(args.dst, "wb").write(bytes(copies[0]) + bytes(copies[1]))
    print("wrote %s" % args.dst)


if __name__ == "__main__":
    main()
