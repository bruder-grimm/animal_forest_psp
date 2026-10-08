#!/usr/bin/env python3
"""Look into a capture (capture_N.state, SELECT + R; runtime/src/capture.c).

Usage:
  tools/afstate.py info FILE...      header, parts, and whether build/psp can resume it
  tools/afstate.py rdram FILE [OUT]  write its RDRAM as a replay dump (default
                                     capture_N_<task>.bin next to FILE), for
                                     scripts/psp_replay.py, check_renders.sh, ...

A capture can only be resumed by the build that took it (build id in the
header; build/psp/build_id.txt has the current one). Its RDRAM replays with
any build.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAGIC = b"AFPSPCAP"
HEADER = struct.Struct("<8s13I32s")
FIELDS = ("version", "build_id", "code_addr", "rdram_addr", "stacks_addr", "task", "number", "polls",
          "rdram_offset", "rdram_size", "state_offset", "state_size", "uptime_ms")


def read_header(path):
    data = Path(path).read_bytes()
    magic, *values, date = HEADER.unpack_from(data)
    if magic != MAGIC:
        sys.exit(f"{path}: not a capture")
    h = dict(zip(FIELDS, values))
    h["date"] = date.split(b"\0")[0].decode()
    return h, data


def parts(h, data):
    pos = h["state_offset"]
    end = pos + h["state_size"]
    while pos + 8 <= end:
        tag, size = struct.unpack_from("<4sI", data, pos)
        yield tag.decode(errors="replace"), size
        pos += 8 + size + (-size % 4)


def info(path):
    h, data = read_header(path)
    print(f"{path}: capture {h['number']} taken {h['date']} (PSP uptime {h['uptime_ms'] / 1000:.1f} s)")
    print(f"  graphics task {h['task']:08X}, controller poll {h['polls']}")
    print(f"  build {h['build_id']:08X}: code at {h['code_addr']:08X}, RDRAM at {h['rdram_addr']:08X}, "
          f"game stacks at {h['stacks_addr']:08X}")
    current = ROOT / "build/psp/build_id.txt"
    if current.exists():
        cur = int(current.read_text().strip(), 16)
        verdict = "can resume it" if cur == h["build_id"] else f"is build {cur:08X}: it can't resume this"
        print(f"  build/psp {verdict}")
    counts = {}
    for tag, size in parts(h, data):
        counts.setdefault(tag, []).append(size)
    print("  parts: " + ", ".join(f"{tag.strip()} {sum(s)} B" + (f" x{len(s)}" if len(s) > 1 else "")
                                  for tag, s in counts.items()))


def rdram(path, out=None):
    h, data = read_header(path)
    src = Path(path)
    out = Path(out) if out else src.with_name(f"capture_{h['number']}_{h['task']:08X}.bin")
    out.write_bytes(data[h["rdram_offset"]:h["rdram_offset"] + h["rdram_size"]])
    print(out)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "rdram"):
        sys.exit(__doc__)
    if sys.argv[1] == "info":
        for path in sys.argv[2:]:
            info(path)
    else:
        rdram(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)


if __name__ == "__main__":
    main()
