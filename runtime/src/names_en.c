/*
 * names_en.c -- English villager names, choices, strings and item names.
 *
 * tools/make_names_en.py builds names_en.bin from the GameCube release: one
 * string per slot, numbered as the N64 looks them up (villagers 0, choices
 * 256, strings 768, items 2400; an empty slot keeps the ROM's text). The hooks
 * in recomp/af.jp.toml replace the four loaders:
 *   mNpc_LoadNpcNameString(name, id)            6 bytes, ROM file 1914
 *   mChoice_Load_ChoseStringFromRom(_, str, idx, actor)   10 bytes
 *   mString_Load_StringFromRom(dst, len, idx)   len bytes
 *   func_80096710_jp(buf, vrom)                 10 bytes of an item name record
 *
 * The N64 buffers are small (an item name is 10 bytes, a villager name 6) and
 * live in the save file and in fixed-size structures, so they cannot grow.
 * A string that does not fit is stored as its first n - 3 bytes followed by a
 * token: TOKEN_MARK and the slot number in two bytes. Everything that shows
 * text expands tokens again:
 *   - func_80090CC0_jp (the font's line drawing, which the choice window and
 *     all the menus use) and mFont_GetStringWidth are pointed at an expanded
 *     copy in RT_NAMES_SCRATCH;
 *   - the message window copies names into its text in the mMsg_Copy*
 *     functions: a hook after each length query makes the text room for the
 *     expanded string, and the copy (func_8009EB44_jp) then takes it.
 * Catchphrases are copied into the save file when a villager moves in, so a
 * town begun in Japanese keeps Japanese ones: mMsg_CopyTail also swaps a
 * catchphrase that matches one of the Japanese ones (names_en.bin carries
 * them from jp_tail) for its English slot.
 * A truncated copy of a token (some code keeps only part of a buffer) shows
 * the English prefix, and without names_en.bin a saved token shows the
 * prefix followed by three odd characters.
 */
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "funcs.h"

#define TOKEN_MARK 0xCE   /* an icon glyph that no name, item or choice uses */
#define TOKEN_LEN 3
#define BASE_VILLAGER 0
#define BASE_CHOICE 256
#define BASE_STRING 768
#define BASE_ITEM 2400
#define ITEM_VROM 0x010F4000u
#define ITEM_FTR 0x1D98u  /* furniture records; slots from 760 on */
#define ITEM_FTR_SLOT 760
#define EXPANDED_MAX 64

/* RT_NAMES_SCRATCH: three expansion buffers (draw, width, message copy). */
#define SCRATCH_DRAW (RT_NAMES_SCRATCH + 0 * EXPANDED_MAX)
#define SCRATCH_WIDTH (RT_NAMES_SCRATCH + 1 * EXPANDED_MAX)
#define SCRATCH_MSG (RT_NAMES_SCRATCH + 2 * EXPANDED_MAX)

static int sState = 0; /* 0 not tried, 1 loaded, -1 absent */
static uint8_t* sFile;
static uint32_t sCount;
static uint32_t sJpTail;  /* first slot of the Japanese catchphrases: strings 569-668, then 16-155 */
#define JP_TAILS 240      /* ... and after them the months and weekdays in full */
static const uint32_t* sOffsets;
static const uint8_t* sData;
static uint32_t sItemEnd; /* item records end (vrom offset) */
static uint32_t sMsgSrc;  /* the message copy source whose expansion is in SCRATCH_MSG */
static uint32_t sMailBase;  /* letters: see make_names_en.py */
static uint32_t sPieceBase; /* fixed parts of letter headers and footers, used whole by tokens */

static void letter_test(void);
static bool sTestLetters, sTestDone;

static bool load(void) {
    if (sState != 0) {
        return sState > 0;
    }
    sState = -1;
    char path[256];
    uint32_t base, file_size;
    SceUID fd = rt_data_open("names_en.bin", path, sizeof(path), &base, &file_size);
    if (fd < 0) {
        rt_log("names_en: no names_en.bin in the EBOOT or next to it: names and menus stay the ROM's");
        return false;
    }
    int size = (int)file_size;
    sFile = size > 24 ? malloc((size_t)size) : NULL;
    uint32_t version = 0;
    if (sFile != NULL && sceIoRead(fd, sFile, (SceSize)size) == size && memcmp(sFile, "AFNM", 4) == 0) {
        version = ((uint32_t*)sFile)[1];
    }
    sceIoClose(fd);
    if (version != 2 && version != 3) {
        rt_log("names_en: %s is not a version 2 or 3 AFNM file", path);
        free(sFile);
        return false;
    }
    sCount = ((uint32_t*)sFile)[2];
    sJpTail = ((uint32_t*)sFile)[3];
    uint32_t words = 4;
    if (version == 3) {
        sMailBase = ((uint32_t*)sFile)[4];
        sPieceBase = ((uint32_t*)sFile)[5];
        words = 6;
    } else {
        sMailBase = sPieceBase = sCount; /* no letters */
    }
    sOffsets = (const uint32_t*)sFile + words;
    sData = (const uint8_t*)(sOffsets + sCount + 1);
    sItemEnd = ITEM_FTR + (sJpTail - BASE_ITEM - ITEM_FTR_SLOT) * 10;
    sState = 1;
    rt_log("names_en: English names, item names and letters from %s (%u slots)", path, (unsigned)sCount);
    if (rt_data_file_exists("letter_test.txt")) {
        letter_test();
        sTestLetters = true;
    }
    return true;
}

static const uint8_t* entry(uint32_t slot, uint32_t* len) {
    if (!load() || slot >= sCount) {
        return NULL;
    }
    *len = sOffsets[slot + 1] - sOffsets[slot];
    return *len ? sData + sOffsets[slot] : NULL;
}

/* Slot numbers are written as two digits in 0x81..0xFF, skipping 0xCD (the
 * message line break): no control code, space or newline can appear. */
static uint8_t digit_byte(uint32_t v) {
    uint32_t b = 0x81 + v;
    return (uint8_t)(b >= 0xCD ? b + 1 : b);
}

static int byte_digit(uint8_t b) {
    if (b < 0x81 || b == 0xCD) {
        return -1;
    }
    return b > 0xCD ? b - 0x82 : b - 0x81;
}

/* Writes slot's string into the n-byte game buffer at dst, space padded:
 * as it is if it fits, else its first n - 3 bytes and a token. */
static void put_slot(uint32_t dst, uint32_t n, uint32_t slot, const uint8_t* s, uint32_t len) {
    uint32_t i = 0;
    if (len <= n || n < TOKEN_LEN + 1) {
        for (; i < len && i < n; i++) {
            wr_u8(dst + i, s[i]);
        }
    } else {
        for (; i < n - TOKEN_LEN; i++) {
            wr_u8(dst + i, s[i]);
        }
        wr_u8(dst + i++, TOKEN_MARK);
        wr_u8(dst + i++, digit_byte(slot / 126));
        wr_u8(dst + i++, digit_byte(slot % 126));
    }
    for (; i < n; i++) {
        wr_u8(dst + i, ' ');
    }
}

static bool put(uint32_t dst, uint32_t n, uint32_t slot) {
    uint32_t len;
    const uint8_t* s = entry(slot, &len);
    if (s == NULL) {
        return false;
    }
    put_slot(dst, n, slot, s, len);
    return true;
}

/* strings_en.c: month (0-11) or weekday (12-18) in full into the n-byte
 * buffer at dst; returns the length the game should use, or -1. */
int rt_names_put_calendar(uint32_t dst, uint32_t n, uint32_t k) {
    uint32_t len;
    if (!load() || entry(sJpTail + JP_TAILS + k, &len) == NULL) {
        return -1;
    }
    put(dst, n, sJpTail + JP_TAILS + k);
    return (int)(len < n ? len : n);
}

/* Expands the tokens in (in, len) into out (max bytes); returns the new
 * length, or -1 if there was no token. */
static int expand_bytes(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t max) {
    if (sState <= 0) {
        return -1;
    }
    bool any = false;
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == TOKEN_MARK && i + 2 < len) {
            int d1 = byte_digit(in[i + 1]), d2 = byte_digit(in[i + 2]);
            uint32_t slot = (uint32_t)(d1 * 126 + d2), slen;
            const uint8_t* s = (d1 >= 0 && d2 >= 0) ? entry(slot, &slen) : NULL;
            if (s != NULL) {
                /* drop the prefix written before the token, then the whole string
                 * (letter pieces are always used whole) */
                uint32_t k = slot >= sPieceBase ? 0 : (o < slen ? o : slen);
                while (k > 0 && memcmp(out + o - k, s, k) != 0) {
                    k--;
                }
                o -= k;
                for (uint32_t j = 0; j < slen && o < max; j++) {
                    out[o++] = s[j];
                }
                i += 2;
                any = true;
                continue;
            }
        }
        if (o < max) {
            out[o++] = c;
        }
    }
    return any ? (int)o : -1;
}

/* Expands the tokens in the game string (src, len) into out;
 * returns the new length, or -1 if there was no token. */
static int expand(uint32_t src, uint32_t len, uint8_t* out) {
    uint8_t in[256];
    if (len > sizeof(in)) {
        len = sizeof(in);
    }
    for (uint32_t i = 0; i < len; i++) {
        in[i] = rd_u8(src + i);
    }
    return expand_bytes(in, len, out, EXPANDED_MAX);
}

/* ---- loaders ------------------------------------------------------------ */

/* void mNpc_LoadNpcNameString(char* name, u8 npcId) */
bool rt_names_villager(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = ctx->r5 & 0xFF;
    return ctx->r4 != 0 && id < 0xFF && put((uint32_t)ctx->r4, 6, BASE_VILLAGER + id);
}

/* void mChoice_Load_ChoseStringFromRom(s32 unused, char* str, s32 idx, Actor* actor)
 * (no English choice has a control code, so mChoice_Change_ControlCode is not needed) */
bool rt_names_choice(uint8_t* rdram, recomp_context* ctx) {
    int32_t idx = (int32_t)ctx->r6;
    return idx >= 0 && idx < 460 && put((uint32_t)ctx->r5, 10, BASE_CHOICE + (uint32_t)idx);
}

/* void mString_Load_StringFromRom(char* dst, s32 dstLen, s32 strIdx) */
bool rt_names_string(uint8_t* rdram, recomp_context* ctx) {
    int32_t len = (int32_t)ctx->r5, idx = (int32_t)ctx->r6;
    return len > 0 && idx >= 0 && idx < 0x61A && put((uint32_t)ctx->r4, (uint32_t)len, BASE_STRING + (uint32_t)idx);
}

/* void func_80096710_jp(char* buf, u32 vrom): DMAs one 10-byte item name record */
bool rt_names_item(uint8_t* rdram, recomp_context* ctx) {
    uint32_t off = (uint32_t)ctx->r5 - ITEM_VROM;
    if (!load() || off >= sItemEnd) {
        return false;
    }
    uint32_t slot;
    if (off >= ITEM_FTR) {
        if ((off - ITEM_FTR) % 10) return false;
        slot = ITEM_FTR_SLOT + (off - ITEM_FTR) / 10;
    } else {
        if (off < 8 || (off - 8) % 10) return false;
        slot = (off - 8) / 10;
    }
    return put((uint32_t)ctx->r4, 10, BASE_ITEM + slot);
}

/* ---- letters ----
 *
 * A letter is a 10-byte header (the recipient's name goes in at
 * headerBackStart), a 96-byte body the letter screen shows as 6 lines of at
 * most 16 bytes, and a 16-byte footer, all kept in the save file. English
 * does not fit, so a letter from the ROM is written as:
 *   header, footer: the English with names filled in if it fits, else its
 *     fixed parts as tokens of letter pieces (slots from piece_base) and the
 *     names as they are;
 *   body: 6 line tokens (TOKEN_MARK, LINE_MARK, 0x81 + line, 0xCD), then from
 *     byte 24 a record of which letter it is and the names it uses:
 *     'E' 'N' kind ids.. {code len bytes..}.. 0x7F sum, all bytes but the
 *     name bytes written as digits (no 0xCD, which the game trims).
 * When the letter screen draws line k (func_80090CC0_jp, via rt_names_draw)
 * the English is put together again, wrapped to the paper and line k shown.
 * The footer is right-aligned by its byte count; rt_names_letter_footer
 * aligns it by its drawn width instead.
 */
#define FREE_STR 0x80140680u /* B_80140680_jp: mHandbill_Set_free_str's 20 strings of 10 bytes */
#define FREE_LEN 10
#define HB_COUNT 548
#define HBZ_COUNT 384
#define LINE_MARK 0x80
#define BODY_LINES 6
#define BODY_LEN 96
#define BODY_RECORD 24
#define HEADER_LEN 10
#define FOOTER_LEN 16
#define PAPER_WIDTH 192 /* 16 Japanese characters */
#define LETTER_MAX 320
#define KIND_ROM 1
#define KIND_VILLAGER 2

enum { HB_SUPER = 0, HB_BODY = HB_COUNT, HB_PS = 2 * HB_COUNT, HBZ_SUPER = 3 * HB_COUNT,
       HBZ_A = HBZ_SUPER + HBZ_COUNT, HBZ_B = HBZ_A + HBZ_COUNT, HBZ_C = HBZ_B + HBZ_COUNT,
       HBZ_PS = HBZ_C + HBZ_COUNT };

static const uint8_t* mail(uint32_t part, uint32_t no, uint32_t* len) {
    return (sMailBase + part + no < sPieceBase) ? entry(sMailBase + part + no, len) : NULL;
}

/* The free string a letter's 0x7F code stands for (func_8009341C_jp): codes
 * 0x24-0x2D are strings 0-9, 0x36-0x3F strings 10-19. */
static int free_index(uint8_t code) {
    if (code >= 0x24 && code <= 0x2D) return code - 0x24;
    if (code >= 0x36 && code <= 0x3F) return code - 0x36 + 10;
    return -1;
}

/* Free string idx from the game, trailing spaces dropped; returns its length. */
static uint32_t free_str(int idx, uint8_t* out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FREE_LEN; i++) {
        out[i] = rd_u8(FREE_STR + (uint32_t)idx * FREE_LEN + i);
    }
    for (n = FREE_LEN; n > 0 && out[n - 1] == ' '; n--) {
    }
    return n;
}

static int piece_slot(const uint8_t* p, uint32_t n) {
    for (uint32_t slot = sPieceBase; slot < sCount; slot++) {
        uint32_t len;
        const uint8_t* s = entry(slot, &len);
        if (s != NULL && len == n && memcmp(s, p, n) == 0) {
            return (int)slot;
        }
    }
    return -1;
}

/* A header or footer: its English template (0xCD marks the name, 0x7F codes
 * the free strings) into dst (n bytes, space padded). tokens: write fixed
 * parts longer than a token as tokens. Returns the name position, or -1 if
 * it did not fit. */
static int build_field(const uint8_t* t, uint32_t tlen, uint8_t* dst, uint32_t n, bool tokens) {
    uint32_t o = 0;
    int back = -1;
    uint32_t i = 0;
    while (i < tlen) {
        if (t[i] == 0x7F && i + 1 < tlen) {
            uint8_t v[FREE_LEN];
            int idx = free_index(t[i + 1]);
            uint32_t vl = idx >= 0 ? free_str(idx, v) : 0;
            if (o + vl > n) return -1;
            memcpy(dst + o, v, vl);
            o += vl;
            i += 2;
            continue;
        }
        if (t[i] == 0xCD) {
            back = (int)o;
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < tlen && t[j] != 0x7F && t[j] != 0xCD) j++;
        int slot = (tokens && j - i > TOKEN_LEN) ? piece_slot(t + i, j - i) : -1;
        if (slot >= 0) {
            if (o + TOKEN_LEN > n) return -1;
            dst[o++] = TOKEN_MARK;
            dst[o++] = digit_byte((uint32_t)slot / 126);
            dst[o++] = digit_byte((uint32_t)slot % 126);
        } else {
            if (o + (j - i) > n) return -1;
            memcpy(dst + o, t + i, j - i);
            o += j - i;
        }
        i = j;
    }
    while (o < n) dst[o++] = ' ';
    return back < 0 ? 0 : back;
}

/* Writes a header or footer; returns the name position. */
static int put_field(uint32_t dst, uint32_t size, uint32_t n, const uint8_t* t, uint32_t tlen) {
    uint8_t buf[FOOTER_LEN];
    if (n > sizeof(buf)) n = sizeof(buf);
    int back = build_field(t, tlen, buf, n, false);
    if (back < 0) back = build_field(t, tlen, buf, n, true);
    if (back < 0) {
        /* still too long: the fixed text alone */
        uint8_t fixed[LETTER_MAX];
        uint32_t fl = 0;
        for (uint32_t i = 0; i < tlen && fl < sizeof(fixed); i++) {
            if (t[i] == 0x7F) { i++; continue; }
            if (t[i] != 0xCD) fixed[fl++] = t[i];
        }
        back = build_field(fixed, fl, buf, n, true);
        if (back < 0) {
            memset(buf, ' ', n);
            back = 0;
        }
    }
    for (uint32_t i = 0; i < size; i++) {
        wr_u8(dst + i, i < n ? buf[i] : ' ');
    }
    return back;
}

/* Writes the body: line tokens and the record of kind, ids and names. */
static void put_body(uint32_t dst, uint32_t kind, const uint32_t* ids, uint32_t nids,
                     const uint8_t* const* t, const uint32_t* tlen, uint32_t nt) {
    uint8_t b[BODY_LEN];
    memset(b, 0xCD, sizeof(b));
    for (uint32_t k = 0; k < BODY_LINES; k++) {
        b[4 * k] = TOKEN_MARK;
        b[4 * k + 1] = LINE_MARK;
        b[4 * k + 2] = digit_byte(k);
        b[4 * k + 3] = 0xCD;
    }
    uint32_t o = BODY_RECORD;
    b[o++] = 'E';
    b[o++] = 'N';
    b[o++] = digit_byte(kind);
    for (uint32_t i = 0; i < nids; i++) {
        b[o++] = digit_byte(ids[i] / 126);
        b[o++] = digit_byte(ids[i] % 126);
    }
    uint32_t seen = 0; /* free strings already written, by index */
    for (uint32_t p = 0; p < nt; p++) {
        for (uint32_t i = 0; i + 1 < tlen[p]; i++) {
            if (t[p][i] != 0x7F) continue;
            int idx = free_index(t[p][i + 1]);
            i++;
            if (idx < 0 || (seen & (1u << idx))) continue;
            uint8_t v[FREE_LEN];
            uint32_t vl = free_str(idx, v);
            if (o + 2 + vl + 2 > BODY_LEN) continue; /* no room: it shows empty */
            seen |= 1u << idx;
            b[o++] = t[p][i];
            b[o++] = digit_byte(vl);
            memcpy(b + o, v, vl);
            o += vl;
        }
    }
    uint32_t sum = 0;
    for (uint32_t i = BODY_RECORD; i < o; i++) sum += b[i];
    b[o++] = 0x7F;
    b[o++] = digit_byte(sum % 126);
    rt_copy_to_rdram(dst, b, BODY_LEN);
}

/* The English text of a letter body drawn from rdram at body; false if it is not one. */
static int letter_text(uint32_t body, uint8_t* out, uint32_t max) {
    uint8_t b[BODY_LEN];
    for (uint32_t i = 0; i < BODY_LEN; i++) b[i] = rd_u8(body + i);
    for (uint32_t k = 0; k < BODY_LINES; k++) {
        if (b[4 * k] != TOKEN_MARK || b[4 * k + 1] != LINE_MARK || byte_digit(b[4 * k + 2]) != (int)k) return -1;
    }
    uint32_t o = BODY_RECORD;
    if (b[o] != 'E' || b[o + 1] != 'N') return -1;
    o += 2;
    int kind = byte_digit(b[o++]);
    uint32_t nids = kind == KIND_ROM ? 1 : kind == KIND_VILLAGER ? 3 : 0;
    if (nids == 0) return -1;
    uint32_t ids[3];
    for (uint32_t i = 0; i < nids; i++) {
        int d1 = byte_digit(b[o]), d2 = byte_digit(b[o + 1]);
        if (d1 < 0 || d2 < 0) return -1;
        ids[i] = (uint32_t)(d1 * 126 + d2);
        o += 2;
    }
    /* the names, up to the end mark */
    uint32_t names = o, end = o;
    while (end + 1 < BODY_LEN && b[end] != 0x7F) {
        int vl = byte_digit(b[end + 1]);
        if (vl < 0 || end + 2 + (uint32_t)vl > BODY_LEN) return -1;
        end += 2 + (uint32_t)vl;
    }
    if (end + 1 >= BODY_LEN) return -1;
    uint32_t sum = 0;
    for (uint32_t i = BODY_RECORD; i < end; i++) sum += b[i];
    if (byte_digit(b[end + 1]) != (int)(sum % 126)) return -1;

    const uint8_t* t[3];
    uint32_t tlen[3], nt = 0;
    if (kind == KIND_ROM) {
        t[0] = mail(HB_BODY, ids[0], &tlen[0]);
        nt = 1;
    } else {
        t[0] = mail(HBZ_A, ids[0], &tlen[0]);
        t[1] = mail(HBZ_B, ids[1], &tlen[1]);
        t[2] = mail(HBZ_C, ids[2], &tlen[2]);
        nt = 3;
    }
    uint32_t n = 0;
    for (uint32_t p = 0; p < nt; p++) {
        if (t[p] == NULL) return -1;
        if (p > 0 && n > 0 && out[n - 1] != 0xCD && n < max) out[n++] = 0xCD;
        for (uint32_t i = 0; i < tlen[p]; i++) {
            if (t[p][i] == 0x7F && i + 1 < tlen[p]) {
                uint8_t code = t[p][++i];
                for (uint32_t r = names; r < end; r += 2 + (uint32_t)byte_digit(b[r + 1])) {
                    if (b[r] == code) {
                        uint8_t v[EXPANDED_MAX];
                        uint32_t vl = (uint32_t)byte_digit(b[r + 1]);
                        int e = expand_bytes(b + r + 2, vl, v, sizeof(v));
                        const uint8_t* src = e >= 0 ? v : b + r + 2;
                        uint32_t sl = e >= 0 ? (uint32_t)e : vl;
                        for (uint32_t j = 0; j < sl && n < max; j++) out[n++] = src[j];
                        break;
                    }
                }
                continue;
            }
            if (n < max) out[n++] = t[p][i];
        }
    }
    return (int)n;
}

static uint32_t text_width(const uint8_t* s, uint32_t n) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) w += (uint32_t)rt_text_en_char_width(s[i]);
    return w;
}

/* Wraps text to the paper, 0xCD breaking lines: starts[k], lens[k] of the
 * first BODY_LINES lines; returns the number of lines it needs. */
static uint32_t wrap(const uint8_t* s, uint32_t n, uint32_t* starts, uint32_t* lens) {
    uint32_t lines = 0, i = 0;
    while (i < n) {
        uint32_t start = i, w = 0, j = i, space = 0;
        bool have_space = false;
        while (j < n && s[j] != 0xCD) {
            uint32_t cw = (uint32_t)rt_text_en_char_width(s[j]);
            if (w + cw > PAPER_WIDTH && j > start) break;
            if (s[j] == ' ') { space = j; have_space = true; }
            w += cw;
            j++;
        }
        uint32_t stop = j, next = j;
        if (j >= n) {
            next = n;
        } else if (s[j] == 0xCD) {
            next = j + 1;
        } else if (have_space && space > start) {
            stop = space;
            next = space + 1;
        }
        while (stop > start && s[stop - 1] == ' ') stop--;
        if (lines < BODY_LINES) { starts[lines] = start; lens[lines] = stop - start; }
        lines++;
        i = next;
    }
    return lines;
}

/* Line k of the letter body the line token at src belongs to, into out. */
static int letter_line(uint32_t src, uint32_t k, uint8_t* out) {
    uint8_t text[LETTER_MAX];
    int n = letter_text(src - 4 * k, text, sizeof(text));
    if (n < 0) return -1;
    while (n > 0 && text[n - 1] == 0xCD) n--;
    uint32_t starts[BODY_LINES], lens[BODY_LINES];
    uint32_t lines = wrap(text, (uint32_t)n, starts, lens);
    if (lines > BODY_LINES) {
        /* too many lines with the GameCube's line breaks: wrap it all again */
        for (int i = 0; i < n; i++) if (text[i] == 0xCD) text[i] = ' ';
        lines = wrap(text, (uint32_t)n, starts, lens);
    }
    if (k >= lines || k >= BODY_LINES) return 0;
    uint32_t l = lens[k] < EXPANDED_MAX ? lens[k] : EXPANDED_MAX;
    memcpy(out, text + starts[k], l);
    return (int)l;
}

/* void mHandbill_Load_HandbillFromRom(u8* header, s32* headerBackStart, u8* footer, u8* body, s32 no) */
static bool letter(uint32_t header, uint32_t hsize, uint32_t back, uint32_t footer, uint32_t fsize,
                   uint32_t body, int32_t no) {
    if (!load() || no < 0 || no >= HB_COUNT) return false;
    uint32_t sl, bl, pl;
    const uint8_t* su = mail(HB_SUPER, (uint32_t)no, &sl);
    const uint8_t* bo = mail(HB_BODY, (uint32_t)no, &bl);
    const uint8_t* ps = mail(HB_PS, (uint32_t)no, &pl);
    if (su == NULL || bo == NULL || ps == NULL) return false;
    int b = put_field(header, hsize, hsize < HEADER_LEN ? hsize : HEADER_LEN, su, sl);
    wr_w32(back, (uint32_t)b);
    put_field(footer, fsize, fsize < FOOTER_LEN ? fsize : FOOTER_LEN, ps, pl);
    uint32_t ids[1] = { (uint32_t)no };
    put_body(body, KIND_ROM, ids, 1, &bo, &bl, 1);
    return true;
}

bool rt_names_letter(uint8_t* rdram, recomp_context* ctx) {
    return letter(ctx->r4, HEADER_LEN, ctx->r5, ctx->r6, FOOTER_LEN, ctx->r7, (int32_t)rt_stack_arg(ctx, 4));
}

/* void mHandbill_Load_HandbillFromRom2(u8* header, s32 headerLen, s32* headerBackStart,
 *                                      u8* footer, s32 footerLen, u8* body, s32 no) */
bool rt_names_letter2(uint8_t* rdram, recomp_context* ctx) {
    return letter(ctx->r4, ctx->r5, ctx->r6, ctx->r7, rt_stack_arg(ctx, 4), rt_stack_arg(ctx, 5),
                  (int32_t)rt_stack_arg(ctx, 6));
}

/* s32 mHandbillz_load(HandbillzInfo*): a villager's letter from 5 banks */
bool rt_names_letterz(uint8_t* rdram, recomp_context* ctx) {
    uint32_t info = (uint32_t)ctx->r4;
    if (!load() || info == 0) return false;
    uint32_t no[5];
    for (uint32_t i = 0; i < 5; i++) {
        no[i] = rd_w32(info + 0x18 + 4 * i);
        if (no[i] >= HBZ_COUNT) return false;
    }
    uint32_t len[5];
    const uint8_t* t[5];
    static const uint32_t parts[5] = { HBZ_SUPER, HBZ_A, HBZ_B, HBZ_C, HBZ_PS };
    for (uint32_t i = 0; i < 5; i++) {
        t[i] = mail(parts[i], no[i], &len[i]);
        if (t[i] == NULL) return false;
    }
    uint32_t hs = rd_w32(info + 0x4), fs = rd_w32(info + 0x14);
    int b = put_field(rd_w32(info + 0x0), hs, hs < HEADER_LEN ? hs : HEADER_LEN, t[0], len[0]);
    wr_w32(info + 0x2C, (uint32_t)b);
    put_field(rd_w32(info + 0x10), fs, fs < FOOTER_LEN ? fs : FOOTER_LEN, t[4], len[4]);
    put_body(rd_w32(info + 0x8), KIND_VILLAGER, &no[1], 3, &t[1], &len[1], 3);
    ctx->r2 = 1;
    return true;
}

/* In func_808899E4_jp (the letter screen's footer), before it draws: a1 the
 * footer, a2 its length, a3 its x (float) = x0 + 12 * (16 - length). */
void rt_names_letter_footer(uint8_t* rdram, recomp_context* ctx) {
    if (!load() || !rt_text_en_active()) return;
    uint32_t len = (uint32_t)ctx->r6;
    uint8_t buf[EXPANDED_MAX], in[FOOTER_LEN] = { 0 };
    if (len > FOOTER_LEN) return;
    for (uint32_t i = 0; i < len; i++) in[i] = rd_u8((uint32_t)ctx->r5 + i);
    int n = expand_bytes(in, len, buf, sizeof(buf));
    uint32_t w = n >= 0 ? text_width(buf, (uint32_t)n) : text_width(in, len);
    union { uint32_t u; float f; } x;
    x.u = (uint32_t)ctx->r7;
    float x0 = x.f - 12.0f * (float)(16 - (int)len);
    x.f = x0 + (w < PAPER_WIDTH ? (float)(PAPER_WIDTH - w) : 0.0f);
    ctx->r7 = (int32_t)x.u;
}


/* ---- letter_test.txt ---------------------------------------------------- */

static const char* const kTestNames[] = { "Marvin", "Bob", "Biff", "Tulip", "Nookway", "Castle" };

/* The 20 free strings letters take their names from, filled with test names;
 * with player != 0 the first is the player's name (6 bytes of RDRAM there). */
static void test_free_strings(uint32_t player) {
    for (uint32_t i = 0; i < 20; i++) {
        const char* v = kTestNames[i % RT_COUNT(kTestNames)];
        uint32_t vl = (uint32_t)strlen(v);
        for (uint32_t j = 0; j < FREE_LEN; j++) {
            uint8_t c = (i == 0 && player != 0) ? (j < 6 ? rd_u8(player + j) : ' ') : (j < vl ? (uint8_t)v[j] : ' ');
            wr_u8(FREE_STR + i * FREE_LEN + j, c);
        }
    }
}

/* A villager letter made through the mHandbillz_load hook: every part number
 * 5, or parts 100..104. The name position goes to *back. */
static bool test_villager_letter(uint32_t header, uint32_t hsize, uint32_t body, uint32_t footer, uint32_t fsize,
                                 bool fives, uint32_t back) {
    uint32_t info = RT_SCRATCH + 8; /* a HandbillzInfo */
    wr_w32(info + 0x0, header);
    wr_w32(info + 0x4, hsize);
    wr_w32(info + 0x8, body);
    wr_w32(info + 0xC, BODY_LEN);
    wr_w32(info + 0x10, footer);
    wr_w32(info + 0x14, fsize);
    for (uint32_t i = 0; i < 5; i++) {
        wr_w32(info + 0x18 + 4 * i, fives ? 5 : 100 + i);
    }
    recomp_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.r4 = info;
    bool ok = rt_names_letterz(g_rdram, &ctx);
    wr_w32(back, rd_w32(info + 0x2C));
    return ok;
}

/* Logs a header or footer as the letter screen shows it, tokens expanded. */
static void log_field(const char* what, uint32_t addr, uint32_t n) {
    uint8_t in[FOOTER_LEN], out[EXPANDED_MAX + 1];
    for (uint32_t i = 0; i < n; i++) in[i] = rd_u8(addr + i);
    int e = expand_bytes(in, n, out, EXPANDED_MAX);
    if (e < 0) { memcpy(out, in, n); e = (int)n; }
    out[e] = 0;
    rt_log("letter_test:   %s [%s]", what, (char*)out);
}

/* Builds a few letters with made-up names and logs how the letter screen would
 * show them (header with the name put in, body lines, footer). Runs once, the
 * first time names_en.bin is used. */
static void letter_test(void) {
    test_free_strings(0);
    uint32_t header = RT_TEST_MAIL, footer = header + HEADER_LEN, body = footer + FOOTER_LEN;
    uint32_t back = RT_SCRATCH;
    static const int32_t nos[] = { 2, 200, 530, 532, 541 };
    for (uint32_t t = 0; t < RT_COUNT(nos) + 2; t++) {
        bool ok;
        if (t < RT_COUNT(nos)) {
            ok = letter(header, HEADER_LEN, back, footer, FOOTER_LEN, body, nos[t]);
            rt_log("letter_test: letter %d: %s", (int)nos[t], ok ? "English" : "ROM");
        } else {
            bool fives = t == RT_COUNT(nos);
            /* buffers longer than the fields, as the game passes them */
            ok = test_villager_letter(header, 20, body, footer, 26, fives, back);
            rt_log("letter_test: villager letter %s: %s", fives ? "5" : "100..", ok ? "English" : "ROM");
        }
        if (!ok) continue;
        /* the header as the letter screen puts it together (func_80889CD8_jp) */
        uint32_t b = rd_w32(back);
        uint8_t h[24];
        uint32_t hn = 0;
        for (uint32_t i = 0; i < b; i++) h[hn++] = rd_u8(header + i);
        for (const char* p = kTestNames[0]; *p; p++) h[hn++] = (uint8_t)*p;
        uint32_t hl = HEADER_LEN;
        while (hl > 0 && rd_u8(header + hl - 1) == ' ') hl--;
        for (uint32_t i = b; i < hl; i++) h[hn++] = rd_u8(header + i);
        rt_copy_to_rdram(RT_SCRATCH + 64, h, hn);
        log_field("header", RT_SCRATCH + 64, hn);
        for (uint32_t k = 0; k < BODY_LINES; k++) {
            uint8_t line[EXPANDED_MAX + 1];
            int n = letter_line(body + 4 * k, k, line);
            line[n < 0 ? 0 : n] = 0;
            rt_log("letter_test:   %u %3upx [%s]", (unsigned)k, (unsigned)text_width(line, n < 0 ? 0 : (uint32_t)n),
                   (char*)line);
        }
        uint32_t fl = FOOTER_LEN;
        while (fl > 0 && rd_u8(footer + fl - 1) == ' ') fl--;
        log_field("footer", footer, fl);
    }
}

/* In the game: once the player is loaded, pocket letters 0-3 become English
 * test letters (ROM letters 2 and 532, villager letters 5 and 100..), for
 * looking at the letter screen. */
#define COMMON_DATA 0x80126EA0u
static void letter_test_pockets(void) {
    uint32_t priv = rd_w32(COMMON_DATA + 0x10138);
    if (priv < 0x80000400u || priv >= 0x80400000u || rd_u8(priv) == ' ' || rd_u8(priv) == 0) return;
    sTestDone = true;
    test_free_strings(priv);
    for (uint32_t t = 0; t < 4; t++) {
        uint32_t m = priv + 0x40A + t * 0xA4;
        for (uint32_t j = 0; j < 0xA4; j++) wr_u8(m + j, 0);
        for (uint32_t j = 0; j < 0x10; j++) wr_u8(m + j, rd_u8(priv + j)); /* recipient: the player */
        wr_u8(m + 0x10, 0);
        const char* from = "Bob   Castle";
        for (uint32_t j = 0; j < 12; j++) wr_u8(m + 0x12 + j, (uint8_t)from[j]);
        wr_u8(m + 0x12 + 0x10, 1); /* sender: a villager */
        uint32_t c = m + 0x26, back = RT_SCRATCH;
        if (t < 2) {
            letter(c + 4, HEADER_LEN, back, c + 0x6E, FOOTER_LEN, c + 0x0E, t == 0 ? 2 : 532);
        } else {
            test_villager_letter(c + 4, HEADER_LEN, c + 0x0E, c + 0x6E, FOOTER_LEN, t == 2, back);
        }
        wr_u8(c + 1, (uint8_t)rd_w32(back));
    }
    rt_log("letter_test: pocket letters 0-3 are English test letters");
}

/*
 * With letter_test.txt, Z + START opens the pockets even where the game would
 * not (a first-day save), so the test letters can be read. Called at the start of
 * mSM_submenu_ctrl(Game_Play*); the game's own code then starts the menu.
 */
void rt_names_test_submenu(uint8_t* rdram, recomp_context* ctx) {
    static bool held;
    bool start = (rt_input_buttons() & 0x3000) == 0x3000, pressed = start && !held; /* Z + START */
    held = start;
    uint32_t submenu = (uint32_t)ctx->r4 + 0x1CBC;
    if (!sTestLetters || !pressed || rd_w32(submenu + 0x0C) != 0 || rd_w32(submenu + 0x04) != 0) return;
    recomp_context saved = *ctx;
    ctx->r4 = (int32_t)submenu;
    ctx->r5 = 1; /* the pockets */
    ctx->r6 = 0;
    ctx->r7 = 0;
    mSM_open_submenu(rdram, ctx);
    *ctx = saved;
    rt_log("letter_test: pockets opened");
}

/* ---- the pocket menu (tag_ovl) ------------------------------------------ */

/*
 * The menus' option labels are records of an 8-byte label and the handler it
 * runs; the fan translation left half of them Japanese and cut others short
 * ("e" for あげる). A label is known by the distance from its record to its
 * handler, which relocating the overlay does not change.
 */
static const struct {
    uint32_t record, handler;
    const char* en;
} kTagLabels[] = {
    { 0x80878AE0, 0x80871ECC, "Open" },      /* あける */
    { 0x80878AEC, 0x80871F74, "Give" },      /* あげる */
    { 0x80878AF8, 0x80872118, "Take" },      /* いただく */
    { 0x80878B04, 0x80872580, "OK" },        /* うん */
    { 0x80878B10, 0x8087207C, "Sell" },      /* これをうる */
    { 0x80878B1C, 0x808725C8, "Send" },      /* おくる */
    { 0x80878B28, 0x80872684, "Rewrite" },   /* かきなおす */
    { 0x80878B34, 0x808726B0, "Hang up" },   /* かべにはる */
    { 0x80878B40, 0x808727E0, "Put in" },    /* これをいれる */
    { 0x80878B4C, 0x8087287C, "Plant" },     /* じめんにうえる */
    { 0x80878B58, 0x80872A34, "Drop" },      /* じめんにおく */
    { 0x80878B64, 0x80872DEC, "Discard" },   /* すてる */
    { 0x80878B70, 0x80872E60, "Give free" }, /* タダであげる */
    { 0x80878B7C, 0x80872E84, "Grab" },      /* つかむ */
    { 0x80878B88, 0x808731EC, "Write" },     /* てがみをかく */
    { 0x80878B94, 0x80873278, "Set price" }, /* ねだんをつける */
    { 0x80878BA0, 0x80873348, "Gift" },      /* プレゼント */
    { 0x80878BAC, 0x80873428, "Just show" }, /* みせるだけ */
    { 0x80878BB8, 0x8087344C, "Quit" },      /* やっぱやめる */
    { 0x80878BC4, 0x80872B54, "Place" },     /* へやにおく */
    { 0x80878BD0, 0x80872748, "Lay down" },  /* ゆかにしく */
    { 0x80878BDC, 0x80873498, "Read" },      /* よむ */
    { 0x80878BE8, 0x80873510, "Hand over" }, /* わたす */
    { 0x80878C3C, 0x808727E0, "Put away" },  /* おさめる */
    { 0x80878C48, 0x80872E84, "Grab all" },  /* ぜんぶつかむ */
    { 0x80878C54, 0x80873724, "Grab one" },  /* 1まいつかむ */
    { 0x80878C60, 0x808737F4, "Order" },     /* ちゅうもんする */
    { 0x80878C6C, 0x808738C8, "Bury" },      /* じめんにうめる */
    { 0x80878C78, 0x808739B0, "Release" },   /* にがす */
    { 0x80878C84, 0x80873C88, "Open" },      /* あける */
    { 0x80878C90, 0x80872DEC, "Erase" },     /* けす */
    { 0x80878C9C, 0x80873F38, "Yes" },       /* はい */
    { 0x80878CA8, 0x8087344C, "No" },        /* いいえ */
};

/* The two labels without a handler, known by their bytes: おかね: and ベル. */
static const uint8_t kMoneyJp[8] = { 0x04, 0x05, 0x17, 0x3A, ' ', ' ', ' ', ' ' };
static const uint8_t kBellsJp[8] = { ' ', ' ', ' ', ' ', ' ', 0xE0, 0xB9, ' ' };

static const char* tag_label(uint32_t src, uint32_t len) {
    if (len == 0 || len > 8 || (src & 3) != 0 || src + 12 > 0x80400000u) return NULL;
    uint32_t handler = rd_w32(src + 8);
    if ((handler >> 24) == 0x80) {
        for (size_t i = 0; i < RT_COUNT(kTagLabels); i++) {
            if (handler - src == kTagLabels[i].handler - kTagLabels[i].record) return kTagLabels[i].en;
        }
    }
    if (len != 8) return NULL;
    bool money = true, bells = true;
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t c = rd_u8(src + i);
        money = money && c == kMoneyJp[i];
        bells = bells && c == kBellsJp[i];
    }
    return money ? "Money:" : bells ? "   Bells" : NULL;
}

/*
 * Japanese the fan translation left in the game's own data (menu words, the
 * text entry screen's prompts, the title screen's controller warning, the
 * "new player" choice) or cut
 * short, known by its bytes once trimmed of spaces. kThemeText: the Happy
 * Room Academy's room themes, which its letters name through free strings.
 */
typedef struct {
    uint8_t len;
    uint8_t jp[20];
    const char* en;
} FixedText;

static const FixedText kScreenText[] = {
    { 5, { 0x0C, 0x12, 0x7D, 0x18, 0x3F }, "Discard?" }, /* すてるの? (JP) */
    { 5, { 0x0C, 0x12, 0x20, 0x18, 0x3F }, "Discard?" }, /* すて の? (fan translation) */
    { 4, { 0xAE, 0xBD, 0xA4, 0x15 }, "Are you" }, /* ホントに */
    { 6, { 0x01, 0x01, 0xF4, 0x0C, 0x05, 0x3F }, "sure?" }, /* いいですか? */
    { 5, { 0xE4, 0xBA, 0xD6, 0xBD, 0xA4 }, "Present" }, /* プレゼント */
    { 2, { 0x19, 0x19 }, "Mom" }, /* はは */
    { 2, { 0x05, 0xE9 }, "Furniture" }, /* かぐ */
    { 4, { 0x05, 0xF9, 0xE7, 0x1F }, "Wallpaper" }, /* かべがみ */
    { 5, { 0xED, 0xCA, 0x02, 0x0F, 0xC3 }, "Carpet" }, /* じゅうたん */
    { 2, { 0x1B, 0x07 }, "Clothing" }, /* ふく */
    { 1, { 0x1B }, "Clothing" }, /* ふ (fan translation) */
    { 2, { 0x05, 0x0A }, "Umbrella" }, /* かさ */
    { 4, { 0xF7, 0xC3, 0x0D, 0xC3 }, "Stationery" }, /* びんせん */
    { 3, { 0x19, 0x15, 0xC1 }, "Gyroid" }, /* はにわ */
    { 3, { 0x05, 0x0D, 0x06 }, "Fossil" }, /* かせき */
    { 6, { 0xB0, 0x8D, 0x90, 0xD4, 0x8F, 0x98 }, "Music" }, /* ミュージック */
    { 8, { 0x14, 0x1E, 0x03, 0xC2, 0x06, 0x24, 0x12, 0x17 }, "Your name" }, /* なまえをきめてね */
    { 9, { 0x01, 0x06, 0x0A, 0x06, 0xC2, 0x06, 0x24, 0x12, 0x17 }, "Town name" }, /* いきさきをきめてね */
    { 9, { 0x07, 0x10, 0xE9, 0x0D, 0xC2, 0x06, 0x24, 0x12, 0x17 }, "Catchphrase" }, /* くちぐせをきめてね */
    { 9, { 0x07, 0x10, 0xE9, 0x0D, 0x20, 0x4E, 0x61, 0x6D, 0x65 }, "Catchphrase" }, /* くちぐせ Name (fan translation) */
    { 9, { 0xAE, 0xBA, 0x8F, 0x84, 0x01, 0xCC, 0x12, 0x1F, 0x21 }, "Go on, say it!" }, /* ホレッ、いってみ! */
    { 9, { 0xB8, 0x98, 0x94, 0x9D, 0xA4, 0xC2, 0xF5, 0x02, 0xF0 }, "Your request?" }, /* リクエストをどうぞ */
    { 3, { 0x23, 0x7B, 0x1C }, "" }, /* むらへ ("to ... village", after the town name) */
    { 4, { 0x19, 0xED, 0x24, 0x12 }, "I'm new" }, /* はじめて (Npc_P_Sel2: a new player, after the residents) */
    { 17, { 0x9A, 0xBD, 0xA4, 0xBB, 0x90, 0xB7, 0x31, 0xE7, 0x0A, 0x0A, 0xCC, 0x12, 0x01, 0x1E, 0x0D, 0xC3, 0x81 }, "Controller 1 is not connected." }, /* コントローラ1がささっていません。 */
    { 16, { 0x1D, 0xC3, 0x0F, 0x01, 0x18, 0xF4, 0xC3, 0xEA, 0xC3, 0xC2, 0x06, 0xCC, 0x12, 0x05, 0x7B, 0x84 }, "Turn the power off, then" }, /* ほんたいのでんげんをきってから、 */
    { 19, { 0x9A, 0xBD, 0xA4, 0xBB, 0x90, 0xB7, 0x31, 0xC2, 0x0D, 0x11, 0xF0, 0x07, 0x0B, 0x12, 0x07, 0xF1, 0x0A, 0x01, 0x81 }, "connect Controller 1." }, /* コントローラ1をせつぞくしてください。 */
    { 9, { 0x9E, 0x90, 0xDF, 0xDB, 0x90, 0xA0, 0x20, 0x08, 0x0C }, "Erase Save" }, /* セーブデータ けす */
};

static const FixedText kThemeText[] = {
    { 3, { 0x91, 0xD4, 0x91 }, "Asian" }, /* アジア */
    { 4, { 0xB7, 0xDF, 0xB8, 0x90 }, "Lovely" }, /* ラブリー */
    { 3, { 0x9C, 0x8F, 0x98 }, "Chic" }, /* シック */
    { 5, { 0x96, 0xBD, 0xA4, 0xB8, 0x90 }, "Country" }, /* カントリー */
    { 4, { 0xB8, 0xD7, 0x90, 0xA4 }, "Resort" }, /* リゾート */
    { 4, { 0x00, 0x04, 0x01, 0xC0 }, "Blue" }, /* あおいろ */
    { 4, { 0xB3, 0xA9, 0x98, 0xBB }, "Monochrome" }, /* モノクロ */
    { 4, { 0xBB, 0x92, 0xB4, 0xB9 }, "Regal" }, /* ロイヤル */
    { 3, { 0x1F, 0xF5, 0x7C }, "Green" }, /* みどり */
    { 2, { 0xBB, 0xD0 }, "Cabin" }, /* ログ */
    { 5, { 0xAA, 0xBB, 0x93, 0x88, 0xBD }, "Halloween" }, /* ハロウィン */
    { 5, { 0x98, 0xB8, 0x9D, 0xAF, 0x9D }, "Christmas" }, /* クリスマス */
    { 4, { 0x96, 0xB7, 0xAC, 0xB9 }, "Colorful" }, /* カラフル */
    { 2, { 0x15, 0xC1 }, "Garden" }, /* にわ */
    { 6, { 0x05, 0x7E, 0x0A, 0xC3, 0x0C, 0x01 }, "Zen garden" }, /* かれさんすい */
    { 3, { 0xC1, 0x1B, 0x02 }, "Japanese" }, /* わふう */
    { 3, { 0x09, 0x02, 0xED }, "Roadwork" }, /* こうじ */
    { 4, { 0x02, 0x10, 0xCA, 0x02 }, "Space" }, /* うちゅう */
    { 4, { 0x0D, 0xC3, 0x13, 0x02 }, "Bathhouse" }, /* せんとう */
    { 4, { 0xE7, 0xCC, 0x09, 0x02 }, "School" }, /* がっこう */
    { 3, { 0xA1, 0x8A, 0x9D }, "Chess" }, /* チェス */
    { 2, { 0xFA, 0xC3 }, "Bonsai" }, /* ぼん */
    { 5, { 0xF9, 0xC3, 0x06, 0xCB, 0x02 }, "Study" }, /* べんきょう */
    { 2, { 0x11, 0xFA }, "Vase" }, /* つぼ */
    { 4, { 0xED, 0x19, 0xC3, 0x06 }, "Vending" }, /* じはんき */
    { 6, { 0x15, 0xC3, 0x12, 0xC3, 0xF5, 0x02 }, "Nintendo" }, /* にんてんどう */
    { 3, { 0xF1, 0x7D, 0x1E }, "Daruma" }, /* だるま */
    { 4, { 0x9B, 0xE1, 0xA3, 0xBD }, "Cactus" }, /* サボテン */
    { 2, { 0x07, 0x1E }, "Bear" }, /* くま */
    { 3, { 0x09, 0x08, 0x0B }, "Kokeshi" }, /* こけし */
    { 4, { 0xA4, 0x90, 0xA3, 0xB1 }, "Totem" }, /* トーテム */
    { 3, { 0xCF, 0xA0, 0x90 }, "Guitar" }, /* ギター */
    { 3, { 0xDC, 0xB7, 0xB1 }, "Drum" }, /* ドラム */
    { 4, { 0xEA, 0xC3, 0xE7, 0x07 }, "Strings" }, /* げんがく */
    { 2, { 0xED, 0x23 }, "Office" }, /* じむ */
    { 4, { 0xA4, 0xB8, 0x99, 0xB7 }, "Tricera" }, /* トリケラ */
    { 4, { 0xA3, 0x88, 0xB7, 0xA9 }, "T. rex" }, /* ティラノ */
    { 3, { 0x91, 0xE2, 0xA4 }, "Apato" }, /* アパト */
    { 3, { 0x9D, 0xA3, 0xD2 }, "Stego" }, /* ステゴ */
    { 6, { 0xE4, 0xA3, 0xB7, 0xA9, 0xDC, 0xBD }, "Pteranodon" }, /* プテラノドン */
    { 3, { 0xAC, 0xA0, 0xDD }, "Plesio" }, /* フタバ */
    { 4, { 0xAF, 0xBD, 0xB3, 0x9D }, "Mammoth" }, /* マンモス */
    { 2, { 0xBB, 0xE1 }, "Robo" }, /* ロボ */
    { 3, { 0xB8, 0xBD, 0xD2 }, "Apple" }, /* リンゴ */
    { 4, { 0x05, 0xC3, 0x06, 0x11 }, "Citrus" }, /* かんきつ */
    { 3, { 0x9D, 0x92, 0x96 }, "Watermelon" }, /* スイカ */
    { 4, { 0xB6, 0x93, 0xA5, 0x9C }, "Pear" }, /* ヨウナシ */
    { 3, { 0x1E, 0x17, 0x06 }, "Lucky cat" }, /* まねき */
    { 5, { 0xE2, 0x92, 0xBD, 0xEC, 0x01 }, "Pine" }, /* パインざい */
    { 3, { 0x96, 0x94, 0xB9 }, "Frog" }, /* カエル */
    { 6, { 0x0B, 0xC0, 0x01, 0x04, 0x19, 0x14 }, "White" }, /* しろいおはな */
    { 6, { 0x00, 0x05, 0x01, 0x04, 0x19, 0x14 }, "Red" }, /* あかいおはな */
    { 6, { 0x06, 0x01, 0xC0, 0x18, 0x19, 0x14 }, "Yellow" }, /* きいろのはな */
    { 3, { 0x0E, 0x18, 0x0F }, "Mixed" }, /* そのた */
    { 5, { 0x5E, 0x06, 0xF1, 0x7D, 0x1E }, "Snowman" }, /* ゆきだるま */
};

/* The English for src[0..len) if it is one of t's, with its leading spaces kept; -1 if not. */
static int fixed_text(const FixedText* t, size_t count, uint32_t src, uint32_t len, uint8_t* out, int max) {
    uint32_t s = 0, e = len;
    if (len == 0 || len > EXPANDED_MAX) return -1;
    while (s < e && rd_u8(src + s) == ' ') s++;
    while (e > s && rd_u8(src + e - 1) == ' ') e--;
    if (e == s || e - s > sizeof(t[0].jp)) return -1;
    uint8_t first = rd_u8(src + s);
    for (size_t i = 0; i < count; i++) {
        if (t[i].len != e - s || t[i].jp[0] != first) continue;
        uint32_t k = 1;
        while (k < t[i].len && rd_u8(src + s + k) == t[i].jp[k]) k++;
        if (k < t[i].len) continue;
        int n = (int)s + (int)strlen(t[i].en);
        if (n > max) return -1;
        memset(out, ' ', s);
        memcpy(out + s, t[i].en, strlen(t[i].en));
        return n;
    }
    return -1;
}

/* A label, a menu word or a prompt drawn from the game's data: its English, or -1. */
static int screen_text(uint32_t src, uint32_t len, uint8_t* out) {
    const char* label = tag_label(src, len);
    if (label != NULL) {
        memcpy(out, label, strlen(label));
        return (int)strlen(label);
    }
    return fixed_text(kScreenText, RT_COUNT(kScreenText), src, len, out, EXPANDED_MAX);
}

/* mHandbill_Set_free_str(s32 index, char* str, s32 len): a room theme in English. */
bool rt_names_free_str(uint8_t* rdram, recomp_context* ctx) {
    uint8_t buf[EXPANDED_MAX];
    if (sState <= 0) return false;
    int n = fixed_text(kThemeText, RT_COUNT(kThemeText), (uint32_t)ctx->r5, (uint32_t)ctx->r6, buf, EXPANDED_MAX);
    int s = 0;
    while (s < n && buf[s] == ' ') s++; /* the table pads the names on the left */
    n = n - s > FREE_LEN ? FREE_LEN + s : n;
    if (n > s) {
        rt_copy_to_rdram(RT_TAG_SCRATCH, buf + s, (uint32_t)(n - s));
        n -= s;
        ctx->r5 = (int32_t)RT_TAG_SCRATCH;
        ctx->r6 = n;
    }
    return false;
}

/*
 * The text entry keyboard (ovl__0078CB80) opens on hiragana; with the English
 * text it opens on ABC (mode 3: ひらがな, @_@, カタカナ, ABC, 123). Called at the end
 * of its state init, func_8088587C_jp, with the state in v1.
 */
void rt_names_keyboard(uint8_t* rdram, recomp_context* ctx) {
    if (sState > 0) wr_u8((uint32_t)ctx->r3 + 4, 3);
}

/* Draws text with func_80090E98_jp(gfx, str, len, x, y, r, g, b, a, 0, 0, sx, sy, 0). */
static void tag_line(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, uint32_t str, int len,
                     float x, float y, float scale, const uint8_t* rgb) {
    recomp_context c = *ctx;
    union { float f; uint32_t u; } fx = { x }, fy = { y }, fs = { scale };
    c.r29 = ctx->r29 - 0x40;
    uint32_t sp = (uint32_t)c.r29;
    wr_w32(sp + 0x10, fy.u);
    wr_w32(sp + 0x14, rgb[0]);
    wr_w32(sp + 0x18, rgb[1]);
    wr_w32(sp + 0x1C, rgb[2]);
    wr_w32(sp + 0x20, 0xFF);
    wr_w32(sp + 0x24, 0);
    wr_w32(sp + 0x28, 0);
    wr_w32(sp + 0x2C, fs.u);
    wr_w32(sp + 0x30, fs.u);
    wr_w32(sp + 0x34, 0);
    c.r4 = (int32_t)gfx;
    c.r5 = (int32_t)str;
    c.r6 = len;
    c.r7 = (int32_t)fx.u;
    func_80090E98_jp(rdram, &c);
}

/* A fixed English word, from RT_TAG_SCRATCH; returns x after it. */
static float tag_word(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, const char* s,
                      float x, float y, float scale, const uint8_t* rgb) {
    uint32_t n = (uint32_t)strlen(s), w = 0;
    rt_copy_to_rdram(RT_TAG_SCRATCH, (const uint8_t*)s, n);
    tag_line(rdram, ctx, gfx, RT_TAG_SCRATCH, (int)n, x, y, scale, rgb);
    for (uint32_t i = 0; i < n; i++) w += (uint32_t)rt_text_en_char_width((uint8_t)s[i]);
    return x + (float)w * scale;
}

/*
 * The letter tag, func_80877B0C_jp(gfx, tag, x, y, scale, char width, line
 * height): the Japanese "<to>さんへの / おてがみ / <from>さんより" puts the
 * suffixes after the names, at a fixed 12 units a character. Drawn in English
 * as "To <to> / Letter / From <from>", with the game's colours by tag type.
 */
bool rt_names_mail_tag(uint8_t* rdram, recomp_context* ctx) {
    if (sState <= 0) return false;
    static const uint8_t kRed[3] = { 0xCD, 0x28, 0x28 }, kPurple[3] = { 0x64, 0x41, 0xC3 },
                         kGreen[3] = { 0x3C, 0x96, 0x41 }, kViolet[3] = { 0xA5, 0x1E, 0xFF },
                         kBlue[3] = { 0x3C, 0x32, 0x9B }, kPink[3] = { 0xE1, 0x1E, 0xDC },
                         kLabel[3] = { 0x5A, 0x3C, 0x32 };
    union { uint32_t u; float f; } x, y, scale, line;
    uint32_t gfx = ctx->r4, tag = ctx->r5;
    x.u = ctx->r6;
    y.u = ctx->r7;
    scale.u = rt_stack_arg(ctx, 4);
    line.u = rt_stack_arg(ctx, 6);
    uint8_t type = rd_u8(tag + 2);

    float nx = tag_word(rdram, ctx, gfx, "To ", x.f, y.f, scale.f, kLabel);
    tag_line(rdram, ctx, gfx, tag + 0x44, 6, nx, y.f, scale.f, type == 4 ? kGreen : kRed);

    const char* what = type == 1 ? "Package" : type == 8 ? "Fortune" : "Letter";
    tag_word(rdram, ctx, gfx, what, x.f, y.f + line.f, scale.f, kLabel);

    float y3 = y.f + 2 * line.f;
    if (type == 9) {
        nx = tag_word(rdram, ctx, gfx, "From ", x.f, y3, scale.f, kLabel);
        tag_word(rdram, ctx, gfx, "HRA", nx, y3, scale.f, kGreen);
        return true;
    }
    const uint8_t* rgb = type == 3 ? kGreen : type == 5 ? kViolet : type == 6 || type == 8 ? kBlue
                       : type == 7 ? kPink : kPurple;
    nx = type == 8 ? x.f : tag_word(rdram, ctx, gfx, "From ", x.f, y3, scale.f, kLabel);
    tag_line(rdram, ctx, gfx, tag + 0x4E, 6, nx, y3, scale.f, rgb);
    return true;
}

/* ---- display ------------------------------------------------------------ */

/*
 * Debug (kana_log.txt): logs each distinct line drawn with Japanese still in
 * it, with the address it came from; tools/afcharset.py decodes the bytes.
 */
static bool japanese_byte(uint8_t c) {
    /* the kana the font keeps in the ASCII range: む め も や ゆ よ ら り る れ */
    return c < 0x20 || c == 0x23 || c == 0x24 || c == 0x5B || c == 0x5D || c == 0x5E || c == 0x60 ||
           (c >= 0x7B && c <= 0x7E) || (c >= 0x80 && c != 0xCD);
}

static void kana_log(uint32_t from, const uint8_t* text, int len) {
    static uint32_t seen[512];
    static int nseen;
    int jp = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == 0x7F && i + 1 < len) { i++; continue; }
        jp += japanese_byte(text[i]);
    }
    if (jp == 0) return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ text[i]) * 16777619u;
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return;
    if (nseen < 512) seen[nseen++] = h;
    char hex[2 * EXPANDED_MAX + 1];
    int n = len < EXPANDED_MAX ? len : EXPANDED_MAX;
    for (int i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", text[i]);
    hex[2 * n] = 0;
    rt_log("kana: %08x len %d %s", (unsigned)from, len, hex);
}

/* func_80090CC0_jp(Game*, char* str, s32 len, ...): draws one line of text */
bool rt_names_draw(uint8_t* rdram, recomp_context* ctx) {
    uint8_t buf[EXPANDED_MAX];
    uint32_t src = (uint32_t)ctx->r5;
    int n = -1;
    if (sTestLetters && !sTestDone) letter_test_pockets();
    if (sState > 0 && (int32_t)ctx->r6 >= 3 && rd_u8(src) == TOKEN_MARK && rd_u8(src + 1) == LINE_MARK) {
        int k = byte_digit(rd_u8(src + 2));
        if (k >= 0 && k < BODY_LINES) n = letter_line(src, (uint32_t)k, buf);
    }
    if (n < 0 && sState > 0) n = screen_text(src, (uint32_t)ctx->r6, buf);
    if (n < 0) n = expand(src, (uint32_t)ctx->r6, buf);
    if (RT_SWITCH("kana_log.txt")) {
        if (n >= 0) {
            kana_log(src, buf, n);
        } else if ((int32_t)ctx->r6 > 0 && (int32_t)ctx->r6 <= EXPANDED_MAX) {
            rt_copy_from_rdram(src, buf, (uint32_t)ctx->r6);
            kana_log(src, buf, (int)ctx->r6);
        }
    }
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_DRAW, buf, (uint32_t)n);
        ctx->r5 = (int32_t)SCRATCH_DRAW;
        ctx->r6 = n;
    }
    return false;
}

/* s32 mFont_GetStringWidth(char* str, s32 len, s32 cut) */
bool rt_names_width(uint8_t* rdram, recomp_context* ctx) {
    uint8_t buf[EXPANDED_MAX];
    int n = sState > 0 ? screen_text((uint32_t)ctx->r4, (uint32_t)ctx->r5, buf) : -1;
    if (n < 0) n = expand((uint32_t)ctx->r4, (uint32_t)ctx->r5, buf);
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_WIDTH, buf, (uint32_t)n);
        ctx->r4 = (int32_t)SCRATCH_WIDTH;
        ctx->r5 = n;
    }
    return false;
}

/* In an mMsg_Copy* function, right after mMsg_Get_Length_String(src, max)
 * returned the length in v0 (or, for mMsg_CopyDetermination, before the text
 * is moved with the length in v1 and pos + length in a1). */
void rt_names_msg_len(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint8_t buf[EXPANDED_MAX];
    int n = expand(src, (uint32_t)ctx->r2, buf);
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_MSG, buf, (uint32_t)n);
        sMsgSrc = src;
        ctx->r2 = n;
    }
}

void rt_names_msg_det(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint8_t buf[EXPANDED_MAX];
    int n = expand(src, (uint32_t)ctx->r3, buf);
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_MSG, buf, (uint32_t)n);
        sMsgSrc = src;
        ctx->r5 += n - (int32_t)ctx->r3;
        ctx->r3 = n;
    }
}

/* mMsg_CopyTail, after mMsg_Get_Length_String(catchphrase, 4): a Japanese
 * catchphrase from the save becomes the English one, else as rt_names_msg_len. */
void rt_names_msg_tail(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint32_t len = (uint32_t)ctx->r2;
    if (load() && len > 0 && len <= 4) {
        uint8_t cur[4];
        for (uint32_t i = 0; i < len; i++) {
            cur[i] = rd_u8(src + i);
        }
        for (uint32_t slot = sJpTail; slot < sCount; slot++) {
            uint32_t jlen;
            const uint8_t* j = entry(slot, &jlen);
            uint32_t en_len;
            uint32_t k = slot - sJpTail;
            if (k >= JP_TAILS) {
                break;
            }
            const uint8_t* en = entry(BASE_STRING + (k < 100 ? 569 + k : 16 + (k - 100)), &en_len);
            if (j != NULL && en != NULL && jlen == len && memcmp(j, cur, len) == 0) {
                /* the English one may not fit the 4-byte field, but this copy goes
                 * straight into the message text */
                uint32_t n = en_len < EXPANDED_MAX ? en_len : EXPANDED_MAX;
                rt_copy_to_rdram(SCRATCH_MSG, en, n);
                sMsgSrc = src;
                ctx->r2 = (int32_t)n;
                return;
            }
        }
    }
    rt_names_msg_len(rdram, ctx, src);
}

/* func_8009EB44_jp(dst, src, len): the copy into the message text */
bool rt_names_msg_copy(uint8_t* rdram, recomp_context* ctx) {
    if (sMsgSrc != 0 && (uint32_t)ctx->r5 == sMsgSrc) {
        ctx->r5 = (int32_t)SCRATCH_MSG;
        sMsgSrc = 0;
    }
    return false;
}
