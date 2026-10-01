#!/usr/bin/env python3
"""Build names_en.bin: English villager names, choices, strings and item names.

  make_names_en.py <gc_dir> <rom.z64> <out.bin> [--report file]

gc_dir holds the English image's files as scripts/make_text_en.sh unpacks
them: select.bin and string.bin (forest_1st_script.arc), npc_name_str_table.bin
(forest_2nd.arc) and forestd.rel (Yaz0-decompressed).

The N64 keeps these in fixed-size ROM tables the game reads by number, and
the GameCube kept nearly the same numbering:
  - villager names: ROM file 1914, 6 bytes per villager id; GC
    npc_name_str_table.bin, 8 bytes per id, same order.
  - choices: ROM files 1885/1886, <= 10 bytes, ids 0..459; GC select.bin,
    same ids (372 of the 401 ids the Japanese messages use are confirmed by
    messages whose English and Japanese choices agree).
  - strings: ROM files 1893/1894, 0x61A entries; GC string.bin by index, except
    156-356 (the GC dropped that block), the staff credits (reordered), the
    unused "よび" slots and 1372 (the gyroid's note, laid out in 16-character
    lines; the fan translation's own version stays).
  - item names: ROM file 2225 (segment 0x10F4000), 10-byte records: one table
    per item-1 category (D_801076BC_jp) and the furniture table at 0x1D98 with
    one record per furniture id (4 ids per piece). GC: 16-byte records in
    forestd.rel (itemName_* / ftrName_table in forestd.map). Categories match
    by index (the GC plant list has a cedar sapling inserted at 1); furniture is
    aligned by dynamic programming, anchored by names already paired in the
    other categories and a small dictionary of the words the Japanese names
    are built from.

Most English names are longer than the N64 buffers (1104 of the item names
are over 10 bytes). The runtime (runtime/src/names_en.c) writes a string that
fits as it is; a longer one becomes its first bytes plus a token the font and
message code expand again. Entry numbers therefore stay fixed: villagers 0,
choices 256, strings 768, items 2400 + item_index(offset in file 2225): the
category records (offset - 8) / 10, furniture 760 + (offset - 0x1D98) / 10
(the furniture table is not on the categories' 10-byte grid).

Villagers keep the catchphrase they were given when they moved in, in the
save file, so a town started without names_en.bin has Japanese ones: the
Japanese catchphrases (strings 569-668, each villager's own, then the pool
16-155) follow the item slots, from jp_tail, for the runtime to recognise
and replace; after them come the months and weekdays in full, for the dates
the English dialogue asks for ("the 29th of September").

Letters (mHandbill_Load_HandbillFromRom*, mHandbillz_load) follow from
mail_base: the header ("super", 0xCD where the name goes), body and footer
("ps") of the 548 ROM letters (files 1887-1892; GC super.bin, mail.bin,
ps.bin, same numbers), then the five banks villagers build letters from
(file 1895: superz, maila, mailb, mailc, psz, 384 each; the GC files of the
same names). Free strings stay 0x7F codes for the runtime to fill in. A
letter is left to the ROM when the English uses a string the Japanese one
does not. From piece_base come the fixed parts of the headers and footers,
for the runtime's tokens (a letter's fields are 10 and 16 bytes).

Output (little endian): "AFNM", u32 version 3, u32 count, u32 jp_tail,
u32 mail_base, u32 piece_base, u32 offset[count+1], then the strings in the
N64 encoding; an empty entry keeps the ROM's text.
"""
import json
import os
import re
import struct
import sys


sys.path.insert(0, os.path.dirname(__file__))
import afcharset  # noqa: E402
import afrom  # noqa: E402
import make_text_en  # noqa: E402
import msgfmt  # noqa: E402

BASE_VILLAGER, BASE_CHOICE, BASE_STRING, BASE_ITEM = 0, 256, 768, 2400
N_VILLAGER, N_CHOICE, N_STRING = 216, 460, 0x61A

ITEM_FILE, ITEM_VROM = 2225, 0x10F4000
# D_801076BC_jp: segment-6 addresses of the item-1 name tables, by category
ITEM1_TABLES = [0x008, 0x288, 0x2B0, 0x418, 0x558, 0xF50, 0x107C, 0x12FC, 0x157C, 0x15C4, 0x1628, 0x1850,
                0x185C, 0x1C1C, 0x1D5C, 0x1D70, 0x1D98]
FTR_TABLE = 0x1D98
# forestd.rel: file offset = forestd.map address + 0x160
GC_DELTA = 0x160
GC_ITEM1 = [(0x1DF3C0, 0x1000), (0x1E03C0, 0x40), (0x1E0400, 0x5C0), (0x1E09C0, 0x280), (0x1E0C40, 0xFF0),
            (0x1E1C30, 0x310), (0x1E1F40, 0x430), (0x1E2370, 0x430), (0x1E27A0, 0x80), (0x1E2820, 0xB0),
            (0x1E28D0, 0x370), (0x1E2C40, 0x100), (0x1E2D40, 0x600), (0x1E3340, 0x2D0), (0x1E3610, 0x20),
            (0x1E3630, 0x40)]
GC_FTR = [(0x1E3670, 0x4000), (0x1E7670, 0xF20)]
FTR_INDEX = 760
# Letters: ROM files (data, end-offset table) of the header, body and footer
HB_FILES = [(1889, 1890), (1887, 1888), (1891, 1892)]
HB_COUNT = 548
HB_GC = ["super.bin", "mail.bin", "ps.bin"]
# file 1895: offset tables and data of superz, maila, mailb, mailc, psz (D_801071B8_jp, D_801071CC_jp)
HBZ_FILE, HBZ_COUNT = 1895, 384
HBZ_TABLES = [0xA360, 0x8B20, 0x9130, 0x9740, 0x9D50]
HBZ_DATA = [0x80D0, 0x0000, 0x26F0, 0x63E0, 0x7860]
HBZ_GC = ["superz.bin", "maila.bin", "mailb.bin", "mailc.bin", "psz.bin"]
# free strings mNpc_SetRemailFreeString always sets for villagers' letters (0-15)
HBZ_FREE = set(range(0x24, 0x2E)) | set(range(0x36, 0x3C))
TOKEN_LEN = 3
TAIL_RANGES = [(569, 668), (16, 155)]  # the catchphrases in the string bank (names_en.c too)
TAILS = [i for a, b in TAIL_RANGES for i in range(a, b + 1)]
CALENDAR = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
            "November", "December", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
PLANT = 9
SKIP_CATEGORIES = {11}  # "dummy": one placeholder on the N64

# Japanese words the furniture names are made of, for aligning the tail of
# the furniture list where no other category supplies a pair.
WORDS = {
    'かさ': 'umbrella', 'ファミコン': 'NES', 'ディスクシステム': 'NES', 'しかくいちくおんき': 'turntable',
    'ちくおんき': 'phonograph', 'ジュークボックス': 'jukebox', 'ダブルラジカセ': 'boom box', 'あかい': 'red',
    'しろい': 'white', 'プロしようコンポ': 'high-end stereo', 'こうきゅうコンポ': 'hi-fi stereo', 'コンポ': 'stereo',
    'ステレオ': 'stereo', 'ラブリー': 'lovely', 'クリスマス': 'Jingle', 'ランプ': 'lamp', 'チェア': 'chair',
    'ラック': 'shelves', 'ソファ': 'sofa', 'ベッド': 'bed', 'クロック': 'clock', 'テーブル': 'table', 'ピアノ': 'piano',
    'こけし': 'Aiko figurine', 'ロボ': 'robo', 'サイコロ': 'dice', 'リンゴ': 'apple', 'とけい': 'clock',
    'どけい': 'clock', 'アンティーク': 'antique', 'アメリカン': 'kitschy', 'テープレコーダー': 'reel-to-reel',
    'CDラジカセ': 'CD player', 'ラジカセ': 'tape deck', 'みみずく': 'owl', 'まねきねこ': 'lucky cat', 'くろい': 'black',
    'よろいかぶと': 'samurai suit', 'たぬき': 'racoon', 'ぶじかえる': 'lucky frog', 'Xマスツリー': 'festive tree',
    'おおきな': 'big', 'しろの': 'white', 'くろの': 'black', 'ルーク': 'rook', 'クイーン': 'queen',
    'ビショップ': 'bishop', 'キング': 'king', 'ナイト': 'knight', 'ポーン': 'pawn', 'カラフル': 'kiddie',
    'ほんだな': 'bookcase', 'とこのま': 'alcove', 'いろり': 'hearth', 'こくばん': 'chalk board',
    'そうじようぐ': 'mop', 'トリケラ': 'tricera', 'Tレックス': 'T-rex', 'アパト': 'apato', 'ステゴ': 'stego',
    'プテラノ': 'ptera', 'スズキリュウ': 'plesio', 'マンモス': 'mammoth', 'あたま': 'skull', 'しっぽ': 'tail',
    'からだ': 'torso', 'うよく': 'right wing', 'さよく': 'left wing', 'くび': 'neck', 'コハク': 'amber',
    'あしあとのかせき': 'dinosaur track', 'アンモナイト': 'ammonite', 'タマゴのかせき': 'dinosaur egg',
    'さんようちゅう': 'trilobite', 'モノクロ': 'modern', 'ゆきだるま': 'Snowman', 'れいぞうこ': 'fridge',
    'スタンド': 'lamp', 'テレビ': 'TV', 'タンス': 'dresser', 'クロゼット': 'wardrobe', 'イス': 'chair',
    'はにわ': 'oid', 'ガラス': 'cube', 'ふく': 'shirt',
}


# Strings the GC left as "TRANSLATE" (catchphrases) and the staff credits,
# which the GC reordered: English by the Japanese text.
EXTRA_STRINGS = {
    17: "sumo", 18: "you know", 19: "yup yup", 20: "indeed", 25: "teehee", 33: "right-o", 47: "dig it",
    55: "so be it", 58: "nyan", 64: "oh my", 79: "dearie", 80: "sah", 83: "gansu", 90: "pooh", 93: "mew mew",
    96: "par", 99: "pssh", 101: "cho", 109: "zeh", 128: "matey", 610: "yeah man", 617: "y'know",
}
# Resetti's apologies (1160-1171) are typed back on the keyboard, so they may
# only use what it has: the GC's "Me = bad" has no "=", and 1166/1169 use GC
# symbols. Raw N64 bytes: "+" is the font's heart, which the keyboard also has.
RESETTI_PHRASES = {1162: b"Me? Bad!", 1166: b"U R my +!", 1169: b"Reset? No!"}
CREDITS = {
    "どうぶつのもり": "Animal Forest", "スタッフ": "Staff", "プロデューサー": "Producer",
    "ディレクター": "Directors", "キャラクターアニメーター": "Character Animator",
    "キャラクターデザイナー": "Character Designers", "スクリーンデザイナー": "Screen Designer",
    "インテリアデザイナー": "Interior Designer", "フィールドデザイナー": "Field Designers",
    "エフェクトデザイナー": "Effects Designer", "デザインアシスタント": "Design Assistants",
    "イベントデザイナー": "Event Designer", "サウンドディレクター": "Sound Director",
    "フィールドBGMコンポーザー": "Field Music", "インドアBGMコンポーザー": "Indoor Music",
    "イベントBGMコンポーザー": "Event Music", "サウンドエフェクトプログラマー": "Sound Effects",
    "サウンドサポーター": "Sound Support", "スクリプトライター": "Script Writers",
    "プログラムマネージャー": "Program Manager", "キャラクタープログラマー": "Character Programmer",
    "プレーヤープログラマー": "Player Programmer", "データしょりプログラマー": "Data Programmer",
    "フィールドプログラマー": "Field Programmer", "プログラマー": "Programmers",
    "ファミコンエミュプログラマー": "NES Emulator Program", "ファミコンエミュ": "NES Emulator",
    "サウンドプログラマー": "Sound Programmer", "スーパーバイザー": "Supervisors",
    "プログレスマネージャー": "Progress Managers", "デバッグサポーター": "Debug Support",
    "デバッガー": "Debugging", "スーパーマリオクラブ": "Super Mario Club", "さるがくちょう": "Sarugakucho",
    "エグゼクティブ・プロデューサー": "Executive Producer",
    "てづか たかし": "Takashi Tezuka", "えぐち かつや": "Katsuya Eguchi", "のがみ ひさし": "Hisashi Nogami",
    "こばやし りゅうじ": "Ryuji Kobayashi", "いけがわ のりこ": "Noriko Ikegawa",
    "もりもと よしひさ": "Yoshihisa Morimoto", "いいだ とき": "Toki Iida", "すみもと あや": "Aya Sumimoto",
    "たかむら じゅん": "Jun Takamura", "こにし しんこ": "Shinko Konishi", "ありもと まさなお": "Masanao Arimoto",
    "はやし みちほ": "Michiho Hayashi", "やまぐち かずみ": "Kazumi Yamaguchi", "すがわら たえこ": "Taeko Sugawara",
    "なかはら ともあき": "Tomoaki Nakahara", "みやぎ あつし": "Atsushi Miyagi", "もり なおき": "Naoki Mori",
    "とたか かずみ": "Kazumi Totaka", "ながた けんた": "Kenta Nagata", "みねぎし とおる": "Toru Minegishi",
    "たなか しのぶ": "Shinobu Tanaka", "ばんどう たろう": "Taro Bando", "いだ やすし": "Yasushi Ida",
    "わだ まこと": "Makoto Wada", "うえだ けんしろう": "Kenshiro Ueda", "わたなべ くにお": "Kunio Watanabe",
    "おおつき ゆひき": "Yuhiki Otsuki", "にい まさる": "Masaru Nii", "こまつ くにひろ": "Kunihiro Komatsu",
    "たかき げんたろう": "Gentaro Takaki", "みやけ ひろみち": "Hiromichi Miyake",
    "うめみや ひろし": "Hiroshi Umemiya", "おがわ まさとし": "Masatoshi Ogawa",
    "くずはら たかみつ": "Takamitsu Kuzuhara", "さかきばら まさろう": "Masaro Sakakibara",
    "さかぐち あつし": "Atsushi Sakaguchi", "ささき まこと": "Makoto Sasaki",
    "すみよし のぶひろ": "Nobuhiro Sumiyoshi", "たけした よしたか": "Yoshitaka Takeshita",
    "にしわき あつし": "Atsushi Nishiwaki", "のま たかふみ": "Takafumi Noma", "はやかわ けんぞう": "Kenzo Hayakawa",
    "まつたに けんじ": "Kenji Matsutani", "よしだ しげき": "Shigeki Yoshida", "やまもと ゆういち": "Yuichi Yamamoto",
    "かわせ ともひろ": "Tomohiro Kawase", "しみず ひであき": "Hideaki Shimizu", "みやもと しげる": "Shigeru Miyamoto",
    "なかごう としひこ": "Toshihiko Nakago", "かとう けいぞう": "Keizo Kato", "なりた みのる": "Minoru Narita",
    "かくい ひろのぶ": "Hironobu Kakui", "やすだ よしと": "Yoshito Yasuda", "やまうち ひろし": "Hiroshi Yamauchi",
}


def words(s):
    return set(w for w in re.split(r'[^a-z0-9]+', s.lower()) if w)


def jwords(j):
    out, rest = set(), j
    for k in sorted(WORDS, key=len, reverse=True):
        if k in rest:
            out |= words(WORDS[k])
            rest = rest.replace(k, '')
    return out


def align(J, G, known):
    """Monotone alignment of the Japanese names J onto the English list G
    (which has extra entries). Returns {j index: g index}."""
    nJ, nG = len(J), len(G)
    Gw = [words(g) for g in G]
    sc = []
    for j in J:
        dj, jw, row = known.get(j), jwords(j), []
        for g in range(nG):
            if dj is not None:
                row.append(3 if dj == G[g] else -2)
            elif jw and Gw[g]:
                o = len(jw & Gw[g]) / len(jw | Gw[g])
                row.append(3 * o if o > 0 else -0.5)
            else:
                row.append(0.2)
        sc.append(row)
    # S[i][g]: best score of J[:i] against G[:g]; bt: 0 pair, 1 skip J, 2 skip G
    S = [[0.0] * (nG + 1)]
    bt = [[2] * (nG + 1)]
    for i in range(1, nJ + 1):
        prev, row_sc = S[i - 1], sc[i - 1]
        cur, b = [prev[0] - 1] + [0.0] * nG, [1] + [0] * nG
        for g in range(1, nG + 1):
            m, a, s_ = prev[g - 1] + row_sc[g - 1], prev[g] - 1, cur[g - 1]
            if m >= a and m >= s_:
                cur[g], b[g] = m, 0
            elif s_ >= a:
                cur[g], b[g] = s_, 2
            else:
                cur[g], b[g] = a, 1
        S.append(cur)
        bt.append(b)
    i, g, pairs = nJ, max(range(nG + 1), key=lambda k: S[nJ][k]), {}
    while i > 0 and g > 0:
        if bt[i][g] == 0:
            if sc[i - 1][g - 1] > 0:
                pairs[i - 1] = g - 1
            i, g = i - 1, g - 1
        elif bt[i][g] == 1:
            i -= 1
        else:
            g -= 1
    return pairs


def item_index(off):
    return FTR_INDEX + (off - FTR_TABLE) // 10 if off >= FTR_TABLE else (off - 8) // 10


def rom_bank(rom, entries, data_file, table_file, n):
    d = afrom.read(rom, entries[data_file])
    t = afrom.read(rom, entries[table_file])
    ends = struct.unpack(f">{len(t) // 4}I", t)
    out, s = [], 0
    for x in ends[:n]:
        out.append(d[s:x])
        s = x
    return out


def free_codes(b):
    return {t[1] for k, t in msgfmt.tokens(b, make_text_en.N64_SIZES) if k == "c"}


def pieces(b):
    """The fixed text between the free strings and the name mark of a header/footer."""
    out, cur = [], bytearray()
    for k, t in msgfmt.tokens(b, make_text_en.N64_SIZES):
        if k == "c":
            out.append(bytes(cur)); cur = bytearray()
        else:
            for c in t:
                if c == 0xCD:
                    out.append(bytes(cur)); cur = bytearray()
                else:
                    cur.append(c)
    out.append(bytes(cur))
    return [x for x in out if len(x) > TOKEN_LEN]


def letters(rom, entries, gc_dir, conv, notes):
    """[header, body, footer] x 548 then 5 x 384 banks (N64 bytes or b"")."""
    def gc_conv(g, header=False):
        """GC -> N64 bytes; a header keeps its 0xCD, which marks where the name goes."""
        c = conv.convert(g) if g else None
        if c is None:
            return None
        if not header:
            return c.rstrip(b"\xcd")
        # "Special Sale!" + name: the GC runs them together; " " + name: no indent
        pre, cd, post = c.partition(b"\xcd")
        if cd and pre.strip(b" ") and not pre.endswith(b" "):
            pre += b" "
        return (pre.lstrip(b" ") if not pre.strip(b" ") else pre) + cd + post

    out = []
    for part, (df, tf) in enumerate(HB_FILES):
        jp = rom_bank(rom, entries, df, tf, HB_COUNT)
        gc = msgfmt.gc_messages(os.path.join(gc_dir, HB_GC[part]))
        for i in range(HB_COUNT):
            c = gc_conv(gc[i], part == 0) if i < len(gc) else None
            if not c or not any(0x20 < x < 0x7F for x in c):
                c = b""
            elif not free_codes(c) <= free_codes(jp[i]):
                notes.append(f"letter {i}\t{HB_GC[part]} uses a string the Japanese does not")
                c = b""
            out.append(c)
    for part in range(5):
        gc = msgfmt.gc_messages(os.path.join(gc_dir, HBZ_GC[part]))
        for i in range(HBZ_COUNT):
            c = gc_conv(gc[i], part == 0) if i < len(gc) else None
            if c and not free_codes(c) <= HBZ_FREE:
                notes.append(f"letter part {i}\t{HBZ_GC[part]} uses a string villagers' letters do not set")
                c = b""
            out.append(c or b"")
    return out


def main():
    gc_dir, rom_path, out = sys.argv[1:4]
    report = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
    conv = make_text_en.Converter({}, json.load(open(make_text_en.TAG_TABLE)))

    def n64(s):
        """GC bytes -> N64 bytes, or None if a character has no equivalent."""
        if isinstance(s, str):
            s = s.encode("latin1")
        out = bytearray()
        for b in s:
            r = conv.text(b)
            if r is None:
                return None
            out += r
        return bytes(out)

    rom = afrom.load(rom_path)
    entries = afrom.entries(rom)
    jp_tail = BASE_ITEM + item_index(len(afrom.read(rom, entries[ITEM_FILE])) - 10) + 1
    cal = jp_tail + len(TAILS)
    total = cal + len(CALENDAR)
    result = [b""] * total
    notes = []

    def put(i, s, why):
        b = n64(s) if s else None
        if b:
            result[i] = b
        elif s:
            notes.append(f"{i}\tunconvertible\t{why}")

    # villagers
    t = open(os.path.join(gc_dir, "npc_name_str_table.bin"), "rb").read()
    names = [t[i:i + 8].decode("latin1").strip() for i in range(0, len(t), 8)]
    for i in range(N_VILLAGER):
        put(BASE_VILLAGER + i, names[i], f"villager {i}")

    # choices
    sel = msgfmt.gc_messages(os.path.join(gc_dir, "select.bin"))
    for i in range(N_CHOICE):
        put(BASE_CHOICE + i, sel[i], f"choice {i}")

    # strings
    st = msgfmt.gc_messages(os.path.join(gc_dir, "string.bin"))
    jp_st = rom_bank(rom, entries, 1893, 1894, N_STRING)
    spare = bytes([0x60, 0xF7])  # よび
    # strings 156-356 are villager names again (the GC dropped the block):
    # match them to the villager table by their Japanese name
    vt = afrom.read(rom, entries[1914])
    jp_names = {vt[8 + 6 * k:14 + 6 * k].rstrip(b" "): k for k in range(N_VILLAGER)}
    for i in range(156, 357):
        k = jp_names.get(jp_st[i].rstrip(b" "))
        if k is not None and k < len(names):
            put(BASE_STRING + i, names[k], f"string {i}")
        else:
            notes.append(f"{BASE_STRING + i}\tno villager\tstring {i}")
    for i in range(N_STRING):
        if 156 <= i <= 356 or 1258 <= i <= 1367 or i == 1372 or jp_st[i] == spare:
            continue
        if i == 484:
            # "むら" (village), appended to the town name; English has no suffix
            # (the GC reused the slot). A space, which the game trims.
            put(BASE_STRING + i, b" ", "string 484")
        elif i < len(st) and st[i] and st[i] != b"TRANSLATE":
            put(BASE_STRING + i, st[i], f"string {i}")

    for i, en in EXTRA_STRINGS.items():
        put(BASE_STRING + i, en, f"string {i}")
    for i, en in RESETTI_PHRASES.items():
        result[BASE_STRING + i] = en
    for i in range(1258, 1368):
        j = afcharset.decode(jp_st[i])
        en = CREDITS.get(j.strip())
        if en:
            put(BASE_STRING + i, " " * (len(j) - len(j.lstrip())) + en, f"credits {i}")

    for k, i in enumerate(TAILS):
        result[jp_tail + k] = jp_st[i].rstrip(b" ")

    for k, name in enumerate(CALENDAR):
        result[cal + k] = name.encode()

    # items
    rel = open(os.path.join(gc_dir, "forestd.rel"), "rb").read()

    def gc_table(off, size):
        b = rel[off + GC_DELTA:off + GC_DELTA + size]
        return [b[k:k + 16].rstrip(b"\x00 ").decode("latin1") for k in range(0, len(b), 16)]

    d = afrom.read(rom, entries[ITEM_FILE])
    rec = lambda o: d[o:o + 10]  # noqa: E731
    known = {}
    cat_pairs = []
    for c in range(16):
        if c in SKIP_CATEGORIES:
            continue
        lo, hi = ITEM1_TABLES[c], ITEM1_TABLES[c + 1]
        G = gc_table(*GC_ITEM1[c])
        for k in range((hi - lo) // 10):
            g = k + 1 if (c == PLANT and k >= 1) else k
            if g < len(G):
                cat_pairs.append((lo + k * 10, G[g]))
                known.setdefault(afcharset.decode(rec(lo + k * 10)).rstrip(), G[g])
    J = [afcharset.decode(rec(o)).rstrip() for o in range(FTR_TABLE, len(d) - 9, 40)]
    F = gc_table(*GC_FTR[0]) + gc_table(*GC_FTR[1])
    for k in range(740):  # the list agrees entry by entry up to here
        known.setdefault(J[k], F[k])
    known["ファミコン"] = "NES"
    pairs = align(J, F, known)
    for off, name in cat_pairs:
        put(BASE_ITEM + item_index(off), name, f"item {off:#x}")
    missing = 0
    for k, j in enumerate(J):
        name = F[pairs[k]] if k in pairs else known.get(j)
        if name is None:
            missing += 1
            notes.append(f"furniture {k}\tno English\t{j}")
            continue
        for r in range(4):
            off = FTR_TABLE + (4 * k + r) * 10
            if off + 10 <= len(d):
                put(BASE_ITEM + item_index(off), name, f"furniture {k}")

    # letters, then the fixed pieces of their headers and footers
    mail_base = len(result)
    mail = letters(rom, entries, gc_dir, conv, notes)
    result += mail
    piece_base = len(result)
    seen = {}
    fields = mail[0:HB_COUNT] + mail[2 * HB_COUNT:3 * HB_COUNT] + mail[3 * HB_COUNT:3 * HB_COUNT + HBZ_COUNT] + \
        mail[3 * HB_COUNT + 4 * HBZ_COUNT:3 * HB_COUNT + 5 * HBZ_COUNT]
    for f in fields:
        for pc in pieces(f):
            if pc not in seen:
                seen[pc] = len(result)
                result.append(pc)
    total = len(result)
    assert total < 126 * 126, total

    offs, data = [], bytearray()
    for r in result:
        offs.append(len(data))
        data += r
    offs.append(len(data))
    hdr = b"AFNM" + struct.pack("<IIIII", 3, total, jp_tail, mail_base, piece_base) + \
        struct.pack(f"<{total + 1}I", *offs)
    with open(out, "wb") as f:
        f.write(hdr + data)
    filled = sum(1 for r in result if r)
    print(f"{out}: {filled} English strings of {total} slots, {len(hdr) + len(data)} bytes, "
          f"{missing} furniture pieces without a name")
    if report:
        open(report, "w").write("\n".join(notes) + "\n")


if __name__ == "__main__":
    main()
