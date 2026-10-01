#!/usr/bin/env python3
"""Find the game files the build needs, by content.

Usage: scripts/find_inputs.py [FILE...]

Looks at every file in roms/ (subfolders too) and at the FILEs named on the command
line, whatever they are called, and recognises:

  disc  the European GameCube Animal Crossing (GAFP01), as .iso, .gcm or .ciso
  jp    the Japanese N64 Animal Forest, in any byte order (see prepare_rom.py)
  en    the English fan translation of it, "Animal Forest (U) [!]" (optional)

Prints what it made of each file on stderr, and on stdout one line "<role> <path>"
for each file it will use ("jp" and "en" are normalized into work/af/baseroms/ first).
Exits with 1 if the disc or the Japanese ROM is missing, or a FILE is not one of the
above.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import gc_disc  # noqa: E402
import prepare_rom  # noqa: E402

GC_MAGIC = b"\xc2\x33\x9f\x3d"  # at 0x1C of every GameCube disc
TGC = "tgc/forest_Eng_Final_PAL50.tgc"
DISC_MIN_SIZE = 100 * 1024 * 1024


def classify_disc(path):
    """None if the file is not a GameCube disc image, else (usable, text)."""
    try:
        head = gc_disc.Disc(str(path)).read(0, 0x20)
    except OSError:
        return None
    if len(head) < 0x20 or head[0x1C:0x20] != GC_MAGIC:
        return None
    game = head[:6].decode("ascii", "replace")
    if game != "GAFP01":
        return False, f"a GameCube disc ({game}), but the European Animal Crossing (GAFP01) is needed"
    try:
        if TGC not in gc_disc.open_image(str(path)).files:
            return False, f"GAFP01, but it has no {TGC}: is the image complete?"
    except Exception:  # a damaged or truncated image
        return False, "GAFP01, but the image is damaged or incomplete (its file table cannot be read)"
    return True, "Animal Crossing (Europe), GAFP01"


def classify(path):
    """-> (role or None, text, image): role is disc/jp/en when the build can use the file."""
    with open(path, "rb") as f:
        head = f.read(4)
    if head in prepare_rom.MAGIC:
        result = prepare_rom.identify(path)
        return result.id, result.text, result.image
    if head == b"CISO" or path.stat().st_size >= DISC_MIN_SIZE:
        disc = classify_disc(path)
        if disc is not None:
            return ("disc" if disc[0] else None), disc[1], None
    return None, "not a game file this build uses", None


def candidates(args):
    """(path, named on the command line) for each file to look at, without duplicates."""
    seen = set()
    for arg in args:
        path = Path(arg)
        if not path.is_file():
            sys.exit(f"No such file: {arg}")
        seen.add(path.resolve())
        yield path, True
    roms = ROOT / "roms"
    for dirpath, dirnames, filenames in os.walk(roms, followlinks=True):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if name.startswith(".") or name.lower().startswith("readme"):
                continue
            path = Path(dirpath) / name
            if path.resolve() not in seen:
                seen.add(path.resolve())
                yield path, False


def show(path):
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def main():
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        sys.exit(__doc__)
    found = {}
    bad = False
    for path, named in candidates(sys.argv[1:]):
        role, text, image = classify(path)
        label = {"disc": "disc", "jp": "ROM ", "en": "ROM "}.get(role, "skip")
        if role is not None and role in found:
            print(f"  {label}  {show(path)}: {text} -- another copy, ignored", file=sys.stderr)
            continue
        print(f"  {label}  {show(path)}: {text}", file=sys.stderr)
        if role is None:
            bad = bad or named
            continue
        found[role] = (path, image)

    missing = []
    if "disc" not in found:
        missing.append("the European GameCube Animal Crossing disc image (GAFP01; .iso, .gcm or .ciso)")
    if "jp" not in found:
        missing.append("the Japanese N64 Animal Forest ROM (Doubutsu no Mori, NUS-NAFJ; .z64, .v64 or .n64)")
    if missing or bad:
        if missing:
            print("\nMissing:", *missing, sep="\n  ", file=sys.stderr)
        if bad:
            print("\nA file named on the command line is not one the build can use (see above).", file=sys.stderr)
        print("\nPut your dumps in the roms/ folder of this repository (any names, any byte order),\n"
              "or name them on the command line. See roms/README.md.", file=sys.stderr)
        sys.exit(1)

    for role in ("disc", "jp", "en"):
        if role not in found:
            continue
        path, image = found[role]
        if image is not None:
            path = prepare_rom.install(role, image)
        print(role, path)


if __name__ == "__main__":
    main()
