#!/usr/bin/env python3
"""Animal Forest (N64) character set: byte -> Unicode, read off the game's font
(ROM file 1882, 16 x 16 cells of 12 x 16). 0x20-0x7E are ASCII except where the
font keeps kana and symbols (#$*+/;[\\]^`{|}~). Used to read the Japanese banks
when lining them up with the GameCube English ones."""

_ROWS = [
    "あいうえおかきくけこさしすせそた",
    "ちつてとなにぬねのはひふへほまみ",
    " !\"むめ%&'()~♥,-.♪",
    "0123456789:💧<=>?",
    "@ABCDEFGHIJKLMNO",
    "PQRSTUVWXYZも💢やゆ_",
    "よabcdefghijklmno",
    "pqrstuvwxyzらりるれ\x7f",
    "　。「」、・ヲァィゥェォャュョッ",
    "ーアイウエオカキクケコサシスセソ",
    "タチツテトナニヌネノハヒフヘホマ",
    "ミムメモヤユヨラリルレロワンヴ☺",
    "ろわをんぁぃぅぇぉゃゅょっ◘ガギ",
    "グゲゴザジズゼゾダヂヅデドバビブ",
    "ベボパピプペポがぎぐげござじずぜ",
    "ぞだぢづでどばびぶべぼぱぴぷぺぽ",
]
CHARS = "".join(_ROWS)
assert len(CHARS) == 256, len(CHARS)


def decode(b):
    """Text bytes to a readable string (0x7F control codes shown as {xx})."""
    out, i = [], 0
    while i < len(b):
        if b[i] == 0x7F and i + 1 < len(b):
            out.append("{%02x}" % b[i + 1]); i += 2
        elif b[i] == 0xCD:
            out.append("/"); i += 1
        else:
            out.append(CHARS[b[i]]); i += 1
    return "".join(out)
