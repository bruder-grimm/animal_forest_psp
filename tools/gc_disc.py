#!/usr/bin/env python3
"""GameCube disc helpers. Reads .iso/.gcm and .ciso images directly.

  gc_disc.py list <disc> [tgc path]           list files (of an embedded .tgc image too)
  gc_disc.py extract <disc> <outdir> [tgc path] extract every file (+ main.dol)
  gc_disc.py unciso <in.ciso> <out.iso>         write a plain disc image

The European Animal Crossing disc holds one embedded image per language
(tgc/forest_Eng_Final_PAL50.tgc, ...); pass its path to work inside it.
"""
import os
import struct
import sys

GC_DISC_SIZE = 1459978240


class Disc:
    """Random access to a plain or CISO disc image."""

    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x8000)
        self.ciso = hdr[:4] == b"CISO"
        if self.ciso:
            self.block = struct.unpack_from("<I", hdr, 4)[0]
            self.map = []
            n = 0
            for used in hdr[8:0x8000]:
                self.map.append(n if used else None)
                n += used

    def read(self, off, size):
        if not self.ciso:
            self.f.seek(off)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            i, o = divmod(off, self.block)
            n = min(size, self.block - o)
            pos = self.map[i] if i < len(self.map) else None
            if pos is None:
                out += bytes(n)
            else:
                self.f.seek(0x8000 + pos * self.block + o)
                out += self.f.read(n)
            off += n
            size -= n
        return bytes(out)


class Image:
    """A disc, or a TGC image embedded in one at offset base."""

    def __init__(self, disc, base=0):
        self.disc, self.base = disc, base
        hdr = disc.read(base, 0x40)
        if hdr[:4] == b"\xae\x0f\x38\xa2":  # TGC
            self.fst_off, self.fst_size = struct.unpack_from(">II", hdr, 0x10)
            self.dol_off, self.dol_size = struct.unpack_from(">II", hdr, 0x1C)
            real, virt = struct.unpack_from(">I", hdr, 0x24)[0], struct.unpack_from(">I", hdr, 0x34)[0]
            self.shift = real - virt
        else:
            boot = disc.read(base, 0x440)
            self.dol_off = struct.unpack_from(">I", boot, 0x420)[0]
            self.fst_off, self.fst_size = struct.unpack_from(">II", boot, 0x424)
            dh = disc.read(base + self.dol_off, 0x100)
            self.dol_size = max(struct.unpack_from(">I", dh, i * 4)[0] + struct.unpack_from(">I", dh, 0x90 + i * 4)[0]
                                for i in range(18))
            self.shift = 0
        self.files = self._fst()

    def _fst(self):
        fst = self.disc.read(self.base + self.fst_off, self.fst_size)
        count = struct.unpack_from(">I", fst, 8)[0]
        names = fst[count * 12:]
        files, stack = {}, [(count, "")]
        for i in range(1, count):
            while stack and i >= stack[-1][0]:
                stack.pop()
            w0, a, b = struct.unpack_from(">III", fst, i * 12)
            noff = w0 & 0xFFFFFF
            path = stack[-1][1] + names[noff:names.index(b"\0", noff)].decode("shift_jis")
            if w0 >> 24:
                stack.append((b, path + "/"))
            else:
                files[path] = (a + self.shift, b)
        return files

    def read(self, path):
        off, size = self.files[path]
        return self.disc.read(self.base + off, size)

    def sub(self, path):
        return Image(self.disc, self.base + self.files[path][0])


def open_image(path, tgc=None):
    img = Image(Disc(path))
    return img.sub(tgc) if tgc else img


def main():
    cmd = sys.argv[1]
    if cmd == "unciso":
        d = Disc(sys.argv[2])
        with open(sys.argv[3], "wb") as o:
            for off in range(0, GC_DISC_SIZE, 1 << 22):
                o.write(d.read(off, min(1 << 22, GC_DISC_SIZE - off)))
    elif cmd == "list":
        img = open_image(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
        for path, (off, size) in img.files.items():
            print(f"{off:08X} {size:9d} {path}")
    elif cmd == "extract":
        img = open_image(sys.argv[2], sys.argv[4] if len(sys.argv) > 4 else None)
        out = sys.argv[3]
        for path in img.files:
            p = os.path.join(out, path)
            os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
            open(p, "wb").write(img.read(path))
        open(os.path.join(out, "main.dol"), "wb").write(img.disc.read(img.base + img.dol_off, img.dol_size))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
