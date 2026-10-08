/*
 * letters.c -- English letters, the letter tag of the pocket menu, and the
 * Happy Room Academy's room themes.
 *
 * A letter is a 10-byte header (the recipient's name goes in at
 * headerBackStart), a 96-byte body the letter screen shows as 6 lines of at
 * most 16 bytes, and a 16-byte footer, all kept in the save file. English
 * does not fit, so a letter from the ROM is written as:
 *   header, footer: the English with names filled in if it fits, else its
 *     fixed parts as tokens of letter pieces (slots from piece_base) and the
 *     names as they are;
 *   body: 6 line tokens (EN_TOKEN, LINE_MARK, digit of the line, a line
 *     break), then from byte 24 a record of which letter it is and the names
 *     it uses: 'E' 'N' kind ids.. {code len bytes..}.. RECORD_END sum, all bytes but
 *     the name bytes written as digits (no line break, which the game trims).
 * When the letter screen draws line k (func_80090CC0_jp, rt_en_draw_line in
 * names.c) the English is put together again, wrapped to the paper and line
 * k shown. The footer is right-aligned by its byte count; rt_en_letter_footer
 * aligns it by its drawn width instead.
 */
#include <string.h>

#include "english.h"
#include "funcs.h"

#define FREE_STR 0x80140680u /* B_80140680_jp: mHandbill_Set_free_str's 20 strings of 10 bytes */
#define FREE_LEN 10
#define FREE_CODE 0x7F  /* in a letter's text: the next byte names a free string (see free_index) */
#define NAME_MARK 0xCD  /* in a header or footer: where the recipient's name goes */
#define RECORD_END 0x7F /* ends the record in a body */
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

/* The banks of letter texts in names_en.bin, from its mail base: the ROM's letters (mHandbill) and the villagers' (mHandbillz). */
enum { HB_SUPER = 0, HB_BODY = HB_COUNT, HB_PS = 2 * HB_COUNT, HBZ_SUPER = 3 * HB_COUNT,
       HBZ_A = HBZ_SUPER + HBZ_COUNT, HBZ_B = HBZ_A + HBZ_COUNT, HBZ_C = HBZ_B + HBZ_COUNT,
       HBZ_PS = HBZ_C + HBZ_COUNT };

static const uint8_t* mail(uint32_t part, uint32_t no, uint32_t* len) {
    uint32_t slot = en_mail_base() + part + no;
    return slot < en_piece_base() ? en_slot(slot, len) : NULL;
}

/* The free string a letter's FREE_CODE code stands for (func_8009341C_jp): codes
 * 0x24-0x2D are strings 0-9, 0x36-0x3F strings 10-19. */
static int free_index(uint8_t code) {
    if (code >= 0x24 && code <= 0x2D) {
        return code - 0x24;
    }
    if (code >= 0x36 && code <= 0x3F) {
        return code - 0x36 + 10;
    }
    return -1;
}

/* Free string idx from the game, trailing spaces dropped; returns its length. */
static uint32_t free_str(int idx, uint8_t* out) {
    uint32_t n;
    for (uint32_t i = 0; i < FREE_LEN; i++) {
        out[i] = rd_u8(FREE_STR + (uint32_t)idx * FREE_LEN + i);
    }
    for (n = FREE_LEN; n > 0 && out[n - 1] == ' '; n--) {
    }
    return n;
}

/* The letter piece slot that holds exactly (p, n), or -1. */
static int piece_slot(const uint8_t* p, uint32_t n) {
    for (uint32_t slot = en_piece_base(); slot < en_slot_count(); slot++) {
        uint32_t len;
        const uint8_t* s = en_slot(slot, &len);
        if (s != NULL && len == n && memcmp(s, p, n) == 0) {
            return (int)slot;
        }
    }
    return -1;
}

/* ---- headers and footers ------------------------------------------------ */

/* A header or footer: its English template (NAME_MARK where the name goes, FREE_CODE
 * codes for the free strings) into dst (n bytes, space padded). tokens: write fixed
 * parts longer than a token as tokens. Returns the name position, or -1 if
 * it did not fit. */
static int build_field(const uint8_t* t, uint32_t tlen, uint8_t* dst, uint32_t n, bool tokens) {
    uint32_t o = 0;
    int back = -1;
    uint32_t i = 0;
    while (i < tlen) {
        if (t[i] == FREE_CODE && i + 1 < tlen) {
            uint8_t v[FREE_LEN];
            int idx = free_index(t[i + 1]);
            uint32_t vl = idx >= 0 ? free_str(idx, v) : 0;
            if (o + vl > n) {
                return -1;
            }
            memcpy(dst + o, v, vl);
            o += vl;
            i += 2;
            continue;
        }
        if (t[i] == NAME_MARK) {
            back = (int)o;
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < tlen && t[j] != FREE_CODE && t[j] != NAME_MARK) {
            j++;
        }
        int slot = (tokens && j - i > EN_TOKEN_LEN) ? piece_slot(t + i, j - i) : -1;
        if (slot >= 0) {
            if (o + EN_TOKEN_LEN > n) {
                return -1;
            }
            dst[o++] = EN_TOKEN;
            dst[o++] = en_digit_byte((uint32_t)slot / 126);
            dst[o++] = en_digit_byte((uint32_t)slot % 126);
        } else {
            if (o + (j - i) > n) {
                return -1;
            }
            memcpy(dst + o, t + i, j - i);
            o += j - i;
        }
        i = j;
    }
    while (o < n) {
        dst[o++] = ' ';
    }
    return back < 0 ? 0 : back;
}

/* Writes a header or footer (size bytes, the text in the first n); returns the name position. */
static int put_field(uint32_t dst, uint32_t size, uint32_t n, const uint8_t* t, uint32_t tlen) {
    uint8_t buf[FOOTER_LEN];
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    int back = build_field(t, tlen, buf, n, false);
    if (back < 0) {
        back = build_field(t, tlen, buf, n, true);
    }
    if (back < 0) {
        /* still too long: the fixed text alone */
        uint8_t fixed[LETTER_MAX];
        uint32_t fl = 0;
        for (uint32_t i = 0; i < tlen && fl < sizeof(fixed); i++) {
            if (t[i] == FREE_CODE) {
                i++;
                continue;
            }
            if (t[i] != NAME_MARK) {
                fixed[fl++] = t[i];
            }
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

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/* ---- the body ----------------------------------------------------------- */

/* Writes the body: line tokens and the record of kind, ids and names. */
static void put_body(uint32_t dst, uint32_t kind, const uint32_t* ids, uint32_t nids,
                     const uint8_t* const* t, const uint32_t* tlen, uint32_t nt) {
    uint8_t b[BODY_LEN];
    memset(b, EN_NEW_LINE, sizeof(b));
    for (uint32_t k = 0; k < BODY_LINES; k++) {
        b[4 * k] = EN_TOKEN;
        b[4 * k + 1] = LINE_MARK;
        b[4 * k + 2] = en_digit_byte(k);
        b[4 * k + 3] = EN_NEW_LINE;
    }
    uint32_t o = BODY_RECORD;
    b[o++] = 'E';
    b[o++] = 'N';
    b[o++] = en_digit_byte(kind);
    for (uint32_t i = 0; i < nids; i++) {
        b[o++] = en_digit_byte(ids[i] / 126);
        b[o++] = en_digit_byte(ids[i] % 126);
    }
    uint32_t seen = 0; /* free strings already written, by index */
    for (uint32_t p = 0; p < nt; p++) {
        for (uint32_t i = 0; i + 1 < tlen[p]; i++) {
            if (t[p][i] != FREE_CODE) {
                continue;
            }
            int idx = free_index(t[p][i + 1]);
            i++;
            if (idx < 0 || (seen & (1u << idx))) {
                continue;
            }
            uint8_t v[FREE_LEN];
            uint32_t vl = free_str(idx, v);
            if (o + 2 + vl + 2 > BODY_LEN) {
                continue; /* no room: it shows empty */
            }
            seen |= 1u << idx;
            b[o++] = t[p][i];
            b[o++] = en_digit_byte(vl);
            memcpy(b + o, v, vl);
            o += vl;
        }
    }
    uint32_t sum = 0;
    for (uint32_t i = BODY_RECORD; i < o; i++) {
        sum += b[i];
    }
    b[o++] = RECORD_END;
    b[o++] = en_digit_byte(sum % 126);
    rt_copy_to_rdram(dst, b, BODY_LEN);
}

/* The English text of a letter body drawn from rdram at body; -1 if it is not one. */
static int letter_text(uint32_t body, uint8_t* out, uint32_t max) {
    uint8_t b[BODY_LEN];
    for (uint32_t i = 0; i < BODY_LEN; i++) {
        b[i] = rd_u8(body + i);
    }
    for (uint32_t k = 0; k < BODY_LINES; k++) {
        if (b[4 * k] != EN_TOKEN || b[4 * k + 1] != LINE_MARK || en_byte_digit(b[4 * k + 2]) != (int)k) {
            return -1;
        }
    }
    uint32_t o = BODY_RECORD;
    if (b[o] != 'E' || b[o + 1] != 'N') {
        return -1;
    }
    o += 2;
    int kind = en_byte_digit(b[o++]);
    uint32_t nids = kind == KIND_ROM ? 1 : kind == KIND_VILLAGER ? 3 : 0;
    if (nids == 0) {
        return -1;
    }
    uint32_t ids[3];
    for (uint32_t i = 0; i < nids; i++) {
        int d1 = en_byte_digit(b[o]), d2 = en_byte_digit(b[o + 1]);
        if (d1 < 0 || d2 < 0) {
            return -1;
        }
        ids[i] = (uint32_t)(d1 * 126 + d2);
        o += 2;
    }
    /* the names, up to the end mark */
    uint32_t names = o, end = o;
    while (end + 1 < BODY_LEN && b[end] != RECORD_END) {
        int vl = en_byte_digit(b[end + 1]);
        if (vl < 0 || end + 2 + (uint32_t)vl > BODY_LEN) {
            return -1;
        }
        end += 2 + (uint32_t)vl;
    }
    if (end + 1 >= BODY_LEN) {
        return -1;
    }
    uint32_t sum = 0;
    for (uint32_t i = BODY_RECORD; i < end; i++) {
        sum += b[i];
    }
    if (en_byte_digit(b[end + 1]) != (int)(sum % 126)) {
        return -1;
    }

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
        if (t[p] == NULL) {
            return -1;
        }
        if (p > 0 && n > 0 && out[n - 1] != EN_NEW_LINE && n < max) {
            out[n++] = EN_NEW_LINE;
        }
        for (uint32_t i = 0; i < tlen[p]; i++) {
            if (t[p][i] == FREE_CODE && i + 1 < tlen[p]) {
                uint8_t code = t[p][++i];
                for (uint32_t r = names; r < end; r += 2 + (uint32_t)en_byte_digit(b[r + 1])) {
                    if (b[r] == code) {
                        uint8_t v[EN_TEXT_MAX];
                        uint32_t vl = (uint32_t)en_byte_digit(b[r + 1]);
                        int e = en_expand(b + r + 2, vl, v, sizeof(v));
                        const uint8_t* src = e >= 0 ? v : b + r + 2;
                        uint32_t sl = e >= 0 ? (uint32_t)e : vl;
                        for (uint32_t j = 0; j < sl && n < max; j++) {
                            out[n++] = src[j];
                        }
                        break;
                    }
                }
                continue;
            }
            if (n < max) {
                out[n++] = t[p][i];
            }
        }
    }
    return (int)n;
}

/* Wraps text to the paper, line breaks breaking lines: starts[k], lens[k] of the
 * first BODY_LINES lines; returns the number of lines it needs. */
static uint32_t wrap(const uint8_t* s, uint32_t n, uint32_t* starts, uint32_t* lens) {
    uint32_t lines = 0, i = 0;
    while (i < n) {
        uint32_t start = i, w = 0, j = i, space = 0;
        bool have_space = false;
        while (j < n && s[j] != EN_NEW_LINE) {
            uint32_t cw = (uint32_t)en_char_width(s[j]);
            if (w + cw > PAPER_WIDTH && j > start) {
                break;
            }
            if (s[j] == ' ') {
                space = j;
                have_space = true;
            }
            w += cw;
            j++;
        }
        uint32_t stop = j, next = j;
        if (j >= n) {
            next = n;
        } else if (s[j] == EN_NEW_LINE) {
            next = j + 1;
        } else if (have_space && space > start) {
            stop = space;
            next = space + 1;
        }
        while (stop > start && s[stop - 1] == ' ') {
            stop--;
        }
        if (lines < BODY_LINES) {
            starts[lines] = start;
            lens[lines] = stop - start;
        }
        lines++;
        i = next;
    }
    return lines;
}

int en_letter_line(uint32_t src, uint32_t len, uint8_t* out) {
    if ((int32_t)len < 3 || rd_u8(src) != EN_TOKEN || rd_u8(src + 1) != LINE_MARK) {
        return -1;
    }
    int k = en_byte_digit(rd_u8(src + 2));
    if (k < 0 || k >= BODY_LINES) {
        return -1;
    }
    /* the whole body, from its first line token */
    uint8_t text[LETTER_MAX];
    int n = letter_text(src - 4 * (uint32_t)k, text, sizeof(text));
    if (n < 0) {
        return -1;
    }
    while (n > 0 && text[n - 1] == EN_NEW_LINE) {
        n--;
    }
    uint32_t starts[BODY_LINES], lens[BODY_LINES];
    uint32_t lines = wrap(text, (uint32_t)n, starts, lens);
    if (lines > BODY_LINES) {
        /* too many lines with the GameCube's line breaks: wrap it all again */
        for (int i = 0; i < n; i++) {
            if (text[i] == EN_NEW_LINE) {
                text[i] = ' ';
            }
        }
        lines = wrap(text, (uint32_t)n, starts, lens);
    }
    if ((uint32_t)k >= lines) {
        return 0;
    }
    uint32_t l = min_u32(lens[k], EN_TEXT_MAX);
    memcpy(out, text + starts[k], l);
    return (int)l;
}

/* ---- the loaders -------------------------------------------------------- */

/* mHandbill_Load_HandbillFromRom(2): header (hsize bytes, the name position written to back), footer, body. */
static bool letter(uint32_t header, uint32_t hsize, uint32_t back, uint32_t footer, uint32_t fsize,
                   uint32_t body, int32_t no) {
    if (!en_names_load() || no < 0 || no >= HB_COUNT) {
        return false;
    }
    uint32_t sl, bl, pl;
    const uint8_t* su = mail(HB_SUPER, (uint32_t)no, &sl);
    const uint8_t* bo = mail(HB_BODY, (uint32_t)no, &bl);
    const uint8_t* ps = mail(HB_PS, (uint32_t)no, &pl);
    if (su == NULL || bo == NULL || ps == NULL) {
        return false;
    }
    wr_w32(back, (uint32_t)put_field(header, hsize, min_u32(hsize, HEADER_LEN), su, sl));
    put_field(footer, fsize, min_u32(fsize, FOOTER_LEN), ps, pl);
    uint32_t ids[1] = { (uint32_t)no };
    put_body(body, KIND_ROM, ids, 1, &bo, &bl, 1);
    return true;
}

/* void mHandbill_Load_HandbillFromRom(u8* header, s32* headerBackStart, u8* footer, u8* body, s32 no) */
bool rt_en_letter(uint8_t* rdram, recomp_context* ctx) {
    return letter(ctx->r4, HEADER_LEN, ctx->r5, ctx->r6, FOOTER_LEN, ctx->r7, (int32_t)rt_stack_arg(ctx, 4));
}

/* void mHandbill_Load_HandbillFromRom2(u8* header, s32 headerLen, s32* headerBackStart,
 *                                      u8* footer, s32 footerLen, u8* body, s32 no) */
bool rt_en_letter2(uint8_t* rdram, recomp_context* ctx) {
    return letter(ctx->r4, ctx->r5, ctx->r6, ctx->r7, rt_stack_arg(ctx, 4), rt_stack_arg(ctx, 5),
                  (int32_t)rt_stack_arg(ctx, 6));
}

/* s32 mHandbillz_load(HandbillzInfo*): a villager's letter from 5 banks */
bool rt_en_villager_letter(uint8_t* rdram, recomp_context* ctx) {
    uint32_t info = (uint32_t)ctx->r4;
    if (!en_names_load() || info == 0) {
        return false;
    }
    static const uint32_t parts[5] = { HBZ_SUPER, HBZ_A, HBZ_B, HBZ_C, HBZ_PS };
    uint32_t no[5], len[5];
    const uint8_t* t[5];
    for (uint32_t i = 0; i < 5; i++) {
        no[i] = rd_w32(info + 0x18 + 4 * i);
        if (no[i] >= HBZ_COUNT) {
            return false;
        }
    }
    for (uint32_t i = 0; i < 5; i++) {
        t[i] = mail(parts[i], no[i], &len[i]);
        if (t[i] == NULL) {
            return false;
        }
    }
    uint32_t hs = rd_w32(info + 0x4), fs = rd_w32(info + 0x14);
    wr_w32(info + 0x2C, (uint32_t)put_field(rd_w32(info + 0x0), hs, min_u32(hs, HEADER_LEN), t[0], len[0]));
    put_field(rd_w32(info + 0x10), fs, min_u32(fs, FOOTER_LEN), t[4], len[4]);
    put_body(rd_w32(info + 0x8), KIND_VILLAGER, &no[1], 3, &t[1], &len[1], 3);
    ctx->r2 = 1;
    return true;
}

/* In func_808899E4_jp (the letter screen's footer), before it draws: a1 the
 * footer, a2 its length, a3 its x (float) = x0 + 12 * (16 - length). */
void rt_en_letter_footer(uint8_t* rdram, recomp_context* ctx) {
    if (!en_names_load() || !rt_en_dialogue_active()) {
        return;
    }
    uint32_t len = (uint32_t)ctx->r6;
    uint8_t buf[EN_TEXT_MAX], in[FOOTER_LEN] = { 0 };
    if (len > FOOTER_LEN) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        in[i] = rd_u8((uint32_t)ctx->r5 + i);
    }
    int n = en_expand(in, len, buf, sizeof(buf));
    uint32_t w = n >= 0 ? en_text_width(buf, (uint32_t)n) : en_text_width(in, len);
    float x0 = rt_f32((uint32_t)ctx->r7) - 12.0f * (float)(16 - (int)len);
    ctx->r7 = (int32_t)rt_f32_bits(x0 + (w < PAPER_WIDTH ? (float)(PAPER_WIDTH - w) : 0.0f));
}

/* ---- the letter tag (pocket menu) --------------------------------------- */

/* Draws text with func_80090E98_jp(gfx, str, len, x, y, r, g, b, a, 0, 0, sx, sy, 0). */
static void tag_line(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, uint32_t str, int len,
                     float x, float y, float scale, const uint8_t* rgb) {
    recomp_context c = *ctx;
    c.r29 = ctx->r29 - 0x40;
    uint32_t sp = (uint32_t)c.r29;
    wr_w32(sp + 0x10, rt_f32_bits(y));
    wr_w32(sp + 0x14, rgb[0]);
    wr_w32(sp + 0x18, rgb[1]);
    wr_w32(sp + 0x1C, rgb[2]);
    wr_w32(sp + 0x20, 0xFF);
    wr_w32(sp + 0x24, 0);
    wr_w32(sp + 0x28, 0);
    wr_w32(sp + 0x2C, rt_f32_bits(scale));
    wr_w32(sp + 0x30, rt_f32_bits(scale));
    wr_w32(sp + 0x34, 0);
    c.r4 = (int32_t)gfx;
    c.r5 = (int32_t)str;
    c.r6 = len;
    c.r7 = (int32_t)rt_f32_bits(x);
    func_80090E98_jp(rdram, &c);
}

/* A fixed English word, from RT_TAG_SCRATCH; returns x after it. */
static float tag_word(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, const char* s,
                      float x, float y, float scale, const uint8_t* rgb) {
    uint32_t n = (uint32_t)strlen(s);
    rt_copy_to_rdram(RT_TAG_SCRATCH, (const uint8_t*)s, n);
    tag_line(rdram, ctx, gfx, RT_TAG_SCRATCH, (int)n, x, y, scale, rgb);
    return x + (float)en_text_width((const uint8_t*)s, n) * scale;
}

/*
 * The letter tag, func_80877B0C_jp(gfx, tag, x, y, scale, char width, line
 * height): the Japanese "<to>さんへの / おてがみ / <from>さんより" puts the
 * suffixes after the names, at a fixed 12 units a character. Drawn in English
 * as "To <to> / Letter / From <from>", with the game's colours by tag type.
 */
bool rt_en_letter_tag(uint8_t* rdram, recomp_context* ctx) {
    if (!en_names_ready()) {
        return false;
    }
    static const uint8_t kRed[3] = { 0xCD, 0x28, 0x28 }, kPurple[3] = { 0x64, 0x41, 0xC3 },
                         kGreen[3] = { 0x3C, 0x96, 0x41 }, kViolet[3] = { 0xA5, 0x1E, 0xFF },
                         kBlue[3] = { 0x3C, 0x32, 0x9B }, kPink[3] = { 0xE1, 0x1E, 0xDC },
                         kLabel[3] = { 0x5A, 0x3C, 0x32 };
    uint32_t gfx = ctx->r4, tag = ctx->r5;
    float x = rt_f32(ctx->r6), y = rt_f32(ctx->r7);
    float scale = rt_f32(rt_stack_arg(ctx, 4)), line = rt_f32(rt_stack_arg(ctx, 6));
    uint8_t type = rd_u8(tag + 2);

    float nx = tag_word(rdram, ctx, gfx, "To ", x, y, scale, kLabel);
    tag_line(rdram, ctx, gfx, tag + 0x44, 6, nx, y, scale, type == 4 ? kGreen : kRed);

    const char* what = type == 1 ? "Package" : type == 8 ? "Fortune" : "Letter";
    tag_word(rdram, ctx, gfx, what, x, y + line, scale, kLabel);

    float y3 = y + 2 * line;
    if (type == 9) {
        nx = tag_word(rdram, ctx, gfx, "From ", x, y3, scale, kLabel);
        tag_word(rdram, ctx, gfx, "HRA", nx, y3, scale, kGreen);
        return true;
    }
    const uint8_t* rgb = type == 3 ? kGreen : type == 5 ? kViolet : type == 6 || type == 8 ? kBlue
                       : type == 7 ? kPink : kPurple;
    nx = type == 8 ? x : tag_word(rdram, ctx, gfx, "From ", x, y3, scale, kLabel);
    tag_line(rdram, ctx, gfx, tag + 0x4E, 6, nx, y3, scale, rgb);
    return true;
}

/* ---- the Happy Room Academy's themes ------------------------------------- */

/* Its letters name the room's theme through a free string; the game's own are Japanese. */
static const EnFixedText kThemeText[] = {
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

/* mHandbill_Set_free_str(s32 index, char* str, s32 len): a room theme in English. */
bool rt_en_free_string(uint8_t* rdram, recomp_context* ctx) {
    uint8_t buf[EN_TEXT_MAX];
    if (!en_names_ready()) {
        return false;
    }
    int n = en_fixed_text(kThemeText, RT_COUNT(kThemeText), (uint32_t)ctx->r5, (uint32_t)ctx->r6, buf, EN_TEXT_MAX);
    int s = 0;
    while (s < n && buf[s] == ' ') {
        s++; /* the table pads the names on the left */
    }
    n = n - s > FREE_LEN ? FREE_LEN + s : n;
    if (n > s) {
        rt_copy_to_rdram(RT_TAG_SCRATCH, buf + s, (uint32_t)(n - s));
        ctx->r5 = (int32_t)RT_TAG_SCRATCH;
        ctx->r6 = n - s;
    }
    return false;
}
