#!/usr/bin/env python3
"""Build text_en.bin: the GameCube (PAL) English dialogue converted for Animal Forest.

  make_text_en.py <msg.bin> <msg_color.bmc> <rom.z64> <out.bin> [--report file]

msg.bin / msg_color.bmc come from the English image on the European Animal
Crossing disc (tgc/forest_Eng_Final_PAL50.tgc -> forest_msg.arc ->
bin_msg/data). Animal Crossing kept Animal Forest's message numbers, so
message N of one is message N of the other for nearly all of the N64 game.

The PAL disc stores control codes as Nintendo BMG tags
(0x80, length, group, u16 id, args); the N64 uses the US-style codes
(0x7F, code, args) that ac-decomp lists as mFont_CONT_CODE_*. Groups whose
arguments carry values (jumps, choices, pauses, colours...) are converted by
rule below. The rest -- NPC animation and quest "demo orders", which the
PAL game split into one tag per (order, value) -- come from TAG_TABLE, which
was learned by aligning the tags of both games' messages with the same number
(each entry held in at least 80% of aligned occurrences).

A converted message is only used when its game logic matches the Japanese
original: same jumps, choices, quest/player/NPC1/NPC2 demo orders,
cancel/choice-window codes and terminator. NPC0 demo orders (expressions) and
sound/voice codes may differ. Where the English has the same logic codes in
the same order but other arguments (the localisers picked other choice strings
or jump targets, many GC messages end "close" where the N64 one continues),
the Japanese codes and terminator are put in place of the English ones: the
N64 program is what runs, so its flow wins and the English keeps the words.
Demo orders the GC added (player/NPC1/NPC2/quest) are dropped. Anything else
is left empty and the game shows the ROM's own text.

Output (little endian): "AFEN", u32 version, u32 count, u32 offset[count+1],
then the message bytes; offset[i] == offset[i+1] means "use the ROM".
"""
import json
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(__file__))
import msgfmt  # noqa: E402

MAX_LEN = 0x400  # func_8009E388_jp refuses longer messages
N64_SIZES = msgfmt.GC_SIZES[:95]

# Logic codes that must match the Japanese message (as a multiset).
LOGIC = {6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 94}
TERMINATORS = {0, 1, 88}
# Strings the program puts into a message for it (free strings 0-19, items,
# the choice just made, code 64): the N64 fills only those its own message
# uses, so the English may not use others -- they would show as whatever the
# slot last held (Alfonso's "Right now in ああああ's town").
FILLED = set(range(36, 47)) | set(range(49, 54)) | set(range(54, 65))
# Demo orders that only the GC version has may be dropped from the English.
DROPPABLE = {8, 10, 11, 12}

TAG_TABLE = os.path.join(os.path.dirname(__file__), "text_en_tags.json")
MANUAL = os.path.join(os.path.dirname(__file__), "text_en_manual.txt")

# The font's ASCII widths (runtime/src/english/dialogue.c kAsciiWidth), for checking
# that hand-written lines fit the window; codes the font draws as kana count 12.
ASCII_WIDTH = [5, 4, 6, 12, 12, 11, 9, 5, 5, 5, 12, 12, 5, 9, 5, 12, 9, 6, 9, 9, 9, 9, 9, 9, 9, 9, 4, 12, 8, 9, 8, 8,
               11, 9, 8, 9, 8, 7, 7, 9, 8, 4, 6, 8, 7, 10, 8, 9, 8, 9, 8, 8, 7, 8, 9, 11, 8, 9, 8, 12, 12, 12, 12, 9,
               12, 7, 7, 7, 7, 7, 5, 7, 7, 4, 5, 7, 5, 10, 7, 8, 7, 7, 5, 7, 5, 7, 7, 11, 7, 7, 6, 12, 12, 12, 12]
LINE_WIDTH = 192  # the message window: 16 Japanese characters of 12 pixels
PAGE_LINES = 4
# Typical widths of what the program puts in: player name, villager name,
# catchphrase, then any other string (town names, items, numbers...).
CODE_WIDTH = {0x1A: 44, 0x1B: 44, 0x1C: 40}
OTHER_WIDTH = 48
GLYPHS = {"~": 0x2A, "♥": 0x2B, "♪": 0x2F}
NOT_ASCII = set(b"#$*+;[\\]^`|{}")  # kana in this font


def encode_manual(text):
    """A text_en_manual.txt line (after "@n ") -> N64 message bytes."""
    out = bytearray()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            j = text.index("}", i)
            code = bytes.fromhex(text[i + 1:j])
            if not code or N64_SIZES[code[0]] != len(code) + 1:
                raise ValueError(f"control code {{{text[i + 1:j]}}} has the wrong length")
            out += b"\x7f" + code
            i = j + 1
            continue
        if ch == "/":
            out.append(0xCD)
        elif ch in GLYPHS:
            out.append(GLYPHS[ch])
        elif " " <= ch <= "~" and ord(ch) not in NOT_ASCII:
            out.append(ord(ch))
        else:
            raise ValueError(f"character {ch!r} is not in the font")
        i += 1
    return bytes(out)


def load_manual(path=MANUAL):
    out = {}
    if not os.path.exists(path):
        return out
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line.startswith("@"):
            continue
        num, _, text = line[1:].partition(" ")
        try:
            out[int(num)] = encode_manual(text)
        except ValueError as e:
            raise SystemExit(f"{path}:{n}: {e}")
    return out


def layout_problems(msg):
    """Lines wider than the window, pages with more lines than it has."""
    probs, w, lines = [], 0, 0  # lines = last line with text on the page, 1-based

    cur = bytearray()

    def end_line():
        nonlocal w, lines
        if w > LINE_WIDTH:
            probs.append(f"{w}px: {cur.decode('latin1')!r}")
        if w:
            lines = page_line + 1
        w = 0
        cur.clear()

    page_line = 0
    for kind, t in msgfmt.tokens(msg, N64_SIZES):
        if kind == "c":
            c = t[1]
            if c == 2:
                end_line()
                if lines > PAGE_LINES:
                    probs.append(f"page of {lines} lines")
                lines = page_line = 0
            elif c in CODE_WIDTH:
                w += CODE_WIDTH[c]
                cur.extend(b"<%02x>" % c)
            elif c in FILLED or c in (46, 47, 48, 64):
                w += OTHER_WIDTH
                cur.extend(b"<%02x>" % c)
            continue
        for b in t:
            if b == 0xCD:
                end_line()
                page_line += 1
            else:
                w += ASCII_WIDTH[b - 0x20] if 0x20 <= b < 0x7F else 12
                cur.append(b)
    end_line()
    if lines > PAGE_LINES:
        probs.append(f"page of {lines} lines")
    return probs


def u16(b, o=0):
    return b[o] << 8 | b[o + 1]


def load_colors(path):
    d = open(path, "rb").read()
    assert d[:8] == b"MGCLbmc1"
    n = struct.unpack_from(">H", d, 0x28)[0]
    return [d[0x2C + i * 4:0x2C + i * 4 + 3] for i in range(n)]


class Converter:
    def __init__(self, colors, table):
        self.colors = colors
        self.table = table
        self.unknown = Counter()

    def tag(self, t):
        """Returns N64 bytes for a PAL tag, b"" to drop it, None if unconvertible."""
        g, i, a = t[2], u16(t, 3), t[5:]
        if g == 1:
            if i == 1: return b"\x7f\x00"
            if i == 2: return b"\x7f\x01"
            if i == 3: return b"\x7f\x02"
            if i == 4: return bytes([0x7F, 3, min(u16(a), 0xFF)])
            if i == 9: return bytes([0x7F, 82, min(u16(a), 0xFF)])
            if i == 10: return b" "  # pixel space
            if i == 11: return bytes([0x7F, 88, min(u16(a), 0xFF)])
        elif g == 2:
            if i in (0, 1, 2):  # choice with 2-4 options: u16 message ids
                return bytes([0x7F, 22 + i]) + a[:4 + 2 * i]
            if 5 <= i <= 8:  # answer n -> next message
                return bytes([0x7F, 15 + i - 5]) + a[:2]
        elif g == 4:
            simple = {0: 26, 1: 27, 2: 28, 3: 46, 4: 64, 5: 47, 7: 29, 8: 30, 9: 31, 10: 32,
                      11: 33, 12: 34, 13: 35, 14: 48}
            if i in simple: return bytes([0x7F, simple[i]])
            if 18 <= i <= 27: return bytes([0x7F, 36 + i - 18])
            if 28 <= i <= 37: return bytes([0x7F, 54 + i - 28])
            if 38 <= i <= 42: return bytes([0x7F, 49 + i - 38])
            if i in (16, 17): return b""  # capitalise next / cut article: no N64 equivalent
        elif g == 5:
            simple = {0: 4, 1: 85, 2: 94, 3: 94, 4: 6, 5: 7, 6: 13, 7: 25}
            if i in simple: return bytes([0x7F, simple[i]])
            if 8 <= i <= 10: return bytes([0x7F, 83, i - 8])
            if i in (11, 12, 13, 14): return b""  # give window / cursor snapping: GC only
        elif g == 10:
            if 0 <= i <= 4: return bytes([0x7F, 75 + i])
            if i in (6, 7): return bytes([0x7F, 81, i - 6])
            if 8 <= i <= 16: return bytes([0x7F, 89, i - 8])
            if i == 17: return bytes([0x7F, 91])
            if i == 18: return bytes([0x7F, 87]) + a[:2].rjust(2, b"\0")
            if i == 19: return bytes([0x7F, 86]) + a[:2].rjust(2, b"\0")
        elif g == 12:
            if i == 0: return bytes([0x7F, 14]) + a[:2]
            if 2 <= i <= 4: return bytes([0x7F, 19 + i - 2]) + a[:2 * i]
        elif g == 255:
            if i == 0:
                return bytes([0x7F, 5]) + self.colors[a[0]]
            if i == 1:
                # character scale: GC in percent, N64 in 1/32 (the JP text uses 64 where
                # the GC has 200, 26 for 80); the old table and * 64 / 100 doubled it
                return bytes([0x7F, 84, min((u16(a) * 32 + 50) // 100, 0xFF)])
        hit = self.table["tags"].get(t.hex())
        if hit is not None:
            return bytes.fromhex(hit)
        self.unknown[(g, i)] += 1
        return None

    def text(self, b):
        c = self.table["chars"].get(str(b))
        if c is not None:
            return bytes.fromhex(c)
        if 0x20 <= b < 0x7F or b == 0xCD:
            return bytes([b])
        return None

    def convert(self, msg):
        out = bytearray()
        i = 0
        while i < len(msg):
            b = msg[i]
            if b == 0x80:
                n = msg[i + 1]
                r = self.tag(msg[i:i + n])
                i += n
            else:
                r = self.text(b)
                i += 1
            if r is None:
                return None
            out += r
        return bytes(out)


def spread_scale(msg, keep=True):
    """The GC scale tag holds until the next one; the N64's code 84 scales only
    the character after it (the font resets it once drawn, as the JP text shows
    by repeating it before every character). Repeat it the same way: before each
    character of a scaled run, newlines excepted, and drop the codes themselves.
    keep=False drops the scaling altogether (for messages that would get too long)."""
    out, scale = bytearray(), 32
    for kind, t in msgfmt.tokens(msg, N64_SIZES):
        if kind == "c" and t[1] == 84:
            scale = t[2]
        elif kind == "c":
            out += t
        else:
            for b in t:
                if keep and scale != 32 and b != 0xCD:
                    out += bytes([0x7F, 84, scale])
                out.append(b)
    return bytes(out)


DEFAULT_COLOR = bytes([50, 60, 50])  # msg_color.bmc entry 0, the window's own text colour
INSERTS = set(range(26, 65))  # codes the program replaces with a string


def spread_color(msg, keep=True):
    """The GC colour tag holds until the next one. Converter.tag() writes it as
    code 5, but on the N64 that sets the colour of the whole line it is on (the
    JP text has it at line starts): a highlighted word coloured its line, and
    the tag ending the highlight turned the line dark again as it was typed.
    Words are coloured with code 80 (colour + number of characters), which the
    font counts down within a line, so each coloured stretch becomes one code 80
    per line. Where the program puts a string into the stretch its length is
    not known: the count is then the largest there is, and a code 80 for one
    character in the default colour before the next dark character ends it.
    keep=False drops the colours (for messages that would get too long)."""
    out, col = [], DEFAULT_COLOR
    run, count, inserts = None, 0, False  # this line's open stretch: its place in out
    stop = False  # a stretch of unknown length has to be ended before the next character

    def close():
        nonlocal run, count, inserts, stop
        if run is not None and inserts:
            out[run] = bytes([0x7F, 80]) + col + b"\xff"
            stop = True
        elif run is not None and count:
            out[run] = bytes([0x7F, 80]) + col + bytes([min(count, 0xFF)])
        run, count, inserts = None, 0, False

    def before_char():
        nonlocal run, stop
        if col != DEFAULT_COLOR:
            if run is None:
                run = len(out)
                out.append(b"")
            stop = False
        elif stop:
            out.append(bytes([0x7F, 80]) + DEFAULT_COLOR + b"\x01")
            stop = False

    for kind, t in msgfmt.tokens(msg, N64_SIZES):
        if kind == "c" and t[1] == 5:
            close()
            if keep:
                col = t[2:5]
        elif kind == "c":
            if t[1] in INSERTS:
                before_char()
                inserts = run is not None
            elif t[1] == 2:
                close()
            out.append(t)
        else:
            for b in t:
                if b == 0xCD:
                    close()
                else:
                    before_char()
                    count += run is not None
                out.append(bytes([b]))
    close()
    return b"".join(out)


def logic(msg):
    codes = [t[1] for t in msgfmt.tokens(msg, N64_SIZES) if t[0] == "c"]
    body = Counter(c.hex() for c in codes if c[1] in LOGIC)
    term = codes[-1][:2] if codes and codes[-1][1] in TERMINATORS else None
    return body, term


def filled(msg):
    return {t[1] for k, t in msgfmt.tokens(msg, N64_SIZES) if k == "c" and t[1] in FILLED}


def fix_filled(en, jp):
    """Repairs two GC habits the N64 program doesn't fill for:
    - "Right now in [slot 4]'s [town]" (the continue greeting): the slot is a
      GC-only string; without it the line reads "Right now in [town]".
    - the harvest moon's date as "the [slot 15] of [slot 14]": the N64 fills
      the month in slot 16 and the day in slot 17."""
    en = re.sub(rb"(\x7f\x05...)\x7f\x28(\x7f\x05...)'s[\xcd ](\x7f\x2f)", rb"\1\3\2", en, flags=re.S)
    if {60, 61} <= filled(en) and {62, 63} <= filled(jp) and not ({60, 61} & filled(jp)):
        en = en.replace(b"\x7f\x3c", b"\x7f\x3e").replace(b"\x7f\x3d", b"\x7f\x3f")
    return en


def adopt_logic(en, jp):
    """The English message with the Japanese logic codes and terminator, or None.

    Works when the logic codes of both come in the same order (after dropping
    GC-only demo orders): each English code is replaced by the Japanese one in
    the same position."""
    toks = msgfmt.tokens(en, N64_SIZES)
    jcodes = [t for k, t in msgfmt.tokens(jp, N64_SIZES) if k == "c"]
    jlogic = [t for t in jcodes if t[1] in LOGIC]
    jnums = [t[1] for t in jlogic]
    elogic = [t for k, t in toks if k == "c" and t[1] in LOGIC]
    drop = set()
    for i, t in enumerate(elogic):
        if t[1] in DROPPABLE and t[1] not in jnums:
            drop.add(i)
    if [t[1] for i, t in enumerate(elogic) if i not in drop] != jnums:
        return None
    jterm = jcodes[-1] if jcodes and jcodes[-1][1] in TERMINATORS else None
    out, li, pending = [], 0, list(jlogic)
    last_c = max((i for i, (k, _) in enumerate(toks) if k == "c"), default=-1)
    for i, (k, t) in enumerate(toks):
        if k == "c" and t[1] in LOGIC:
            if li not in drop:
                out.append(pending.pop(0))
            li += 1
        elif k == "c" and i == last_c and t[1] in TERMINATORS:
            if jterm is None:
                return None
            out.append(jterm)
        else:
            out.append(t)
    msg = b"".join(out)
    return msg if logic(msg) == logic(jp) else None


def has_text(msg):
    return any(k == "t" and t.strip(b" \xcd") for k, t in msgfmt.tokens(msg, N64_SIZES))


def manual_problems(c, j):
    """Why a hand-written message cannot replace the Japanese one ("" if it can)."""
    if len(c) > MAX_LEN:
        return "too long"
    if logic(c) != logic(j):
        return "logic differs"
    if not filled(c) <= filled(j):
        return "uses a string the N64 does not fill"
    return ", ".join(layout_problems(c))


def check_manual(rom):
    """Prints the problems of each text_en_manual.txt entry."""
    jp = msgfmt.af_messages(rom)
    manual = load_manual()
    bad = 0
    for n, c in sorted(manual.items()):
        p = manual_problems(c, jp[n])
        if p:
            bad += 1
            print(f"@{n}: {p}")
    print(f"{len(manual)} hand-written messages, {bad} with problems")


def main():
    if sys.argv[1] == "--check-manual":
        return check_manual(sys.argv[2])
    msg_bin, bmc, rom, out = sys.argv[1:5]
    report = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
    table = json.load(open(TAG_TABLE))
    conv = Converter(load_colors(bmc), table)
    jp = msgfmt.af_messages(rom)
    en = msgfmt.gc_messages(msg_bin)
    result = []
    why = Counter()
    lines = []
    manual = load_manual()
    for n, j in enumerate(jp):
        e = en[n] if n < len(en) else b""
        c = conv.convert(e) if e else None
        if c is not None:
            spread = spread_scale(c)
            c = spread if len(spread) <= MAX_LEN else spread_scale(c, keep=False)
        if n in manual:
            c = manual[n]
            reason = manual_problems(c, j)
            if reason:
                reason = "manual: " + reason
        elif c is not None and has_text(j) and not has_text(c):
            # the GC left this number empty (it moved the text elsewhere):
            # an empty English window would replace the Japanese one
            reason = "empty on the GC"
        elif c is None:
            reason = "unconvertible" if e else "missing"
        elif len(c) > MAX_LEN:
            reason = "too long"
        elif logic(c) != logic(j) and (c := adopt_logic(c, j)) is None:
            reason = "logic differs"
        elif len(c) > MAX_LEN:
            reason = "too long"
        elif not filled(c := fix_filled(c, j)) <= filled(j):
            reason = "uses a string the N64 does not fill"
        else:
            reason = None
            # last, as fix_filled looks for the colours in their GC form
            spread = spread_color(c)
            c = spread if len(spread) <= MAX_LEN else spread_color(c, keep=False)
        if reason:
            why[reason] += 1
            lines.append(f"{n}\t{reason}")
            result.append(b"")
        else:
            why["ok"] += 1
            result.append(c)
    offs, data = [], bytearray()
    for r in result:
        offs.append(len(data))
        data += r
    offs.append(len(data))
    hdr = b"AFEN" + struct.pack("<II", 1, len(result)) + struct.pack(f"<{len(offs)}I", *offs)
    with open(out, "wb") as f:
        f.write(hdr + data)
    print(f"{out}: {why['ok']}/{len(jp)} messages English, {len(hdr) + len(data)} bytes")
    for k, v in why.most_common():
        print(f"  {k}: {v}")
    if conv.unknown:
        print("  unconvertible tags:", dict(conv.unknown.most_common(12)))
    if report:
        open(report, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
