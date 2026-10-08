/*
 * names.c -- English villager names, choices, strings and item names, and
 * the hooks every line of text the game draws goes through.
 *
 * tools/make_names_en.py builds names_en.bin from the GameCube release: one
 * string per slot, numbered as the N64 looks them up (villagers 0, choices
 * 256, strings 768, items 2400; an empty slot keeps the ROM's text). Hooks
 * replace the four loaders:
 *   mNpc_LoadNpcNameString(name, id)            6 bytes, ROM file 1914
 *   mChoice_Load_ChoseStringFromRom(_, str, idx, actor)   10 bytes
 *   mString_Load_StringFromRom(dst, len, idx)   len bytes
 *   func_80096710_jp(buf, vrom)                 10 bytes of an item name record
 *
 * The N64 buffers are small (an item name is 10 bytes, a villager name 6) and
 * live in the save file and in fixed-size structures, so they cannot grow.
 * A string that does not fit is stored as its first n - 3 bytes followed by a
 * token: EN_TOKEN and the slot number in two bytes. Everything that shows
 * text expands tokens again:
 *   - func_80090CC0_jp (the font's line drawing, which the choice window and
 *     all the menus use) and mFont_GetStringWidth are pointed at an expanded
 *     copy in RT_NAMES_SCRATCH (rt_en_draw_line, rt_en_string_width), which
 *     also put letters (letters.c) and the game's own Japanese words
 *     (screen.c) into English;
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
#include <stdlib.h>
#include <string.h>

#include "english.h"
#include "funcs.h"

#define BASE_VILLAGER 0
#define BASE_CHOICE 256
#define BASE_ITEM 2400
#define ITEM_VROM 0x010F4000u
#define ITEM_FTR 0x1D98u  /* furniture records; slots from 760 on */
#define ITEM_FTR_SLOT 760

/* RT_NAMES_SCRATCH: three expansion buffers (draw, width, message copy). */
#define SCRATCH_DRAW (RT_NAMES_SCRATCH + 0 * EN_TEXT_MAX)
#define SCRATCH_WIDTH (RT_NAMES_SCRATCH + 1 * EN_TEXT_MAX)
#define SCRATCH_MSG (RT_NAMES_SCRATCH + 2 * EN_TEXT_MAX)

static int sState = 0; /* 0 not tried, 1 loaded, -1 absent */
static uint8_t* sFile;
static uint32_t sCount;
static uint32_t sJpTail;  /* first slot of the Japanese catchphrases: strings 569-668, then 16-155 */
#define JP_TAILS 240      /* ... and after them the months and weekdays in full */
static const uint32_t* sOffsets;
static const uint8_t* sData;
static uint32_t sItemEnd;   /* item records end (vrom offset) */
static uint32_t sMailBase;  /* letters: see make_names_en.py */
static uint32_t sPieceBase; /* fixed parts of letter headers and footers, used whole by tokens */
static uint32_t sMsgSrc;    /* the message copy source whose expansion is in SCRATCH_MSG */

bool en_names_load(void) {
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
    return true;
}

bool en_names_ready(void) {
    return sState > 0;
}

const uint8_t* en_slot(uint32_t slot, uint32_t* len) {
    if (!en_names_load() || slot >= sCount) {
        return NULL;
    }
    *len = sOffsets[slot + 1] - sOffsets[slot];
    return *len ? sData + sOffsets[slot] : NULL;
}

uint32_t en_mail_base(void) {
    return sMailBase;
}

uint32_t en_piece_base(void) {
    return sPieceBase;
}

uint32_t en_slot_count(void) {
    return sCount;
}

/* ---- tokens ------------------------------------------------------------- */

uint8_t en_digit_byte(uint32_t v) {
    uint32_t b = 0x81 + v;
    return (uint8_t)(b >= EN_NEW_LINE ? b + 1 : b);
}

int en_byte_digit(uint8_t b) {
    if (b < 0x81 || b == EN_NEW_LINE) {
        return -1;
    }
    return b > EN_NEW_LINE ? b - 0x82 : b - 0x81;
}

/* Writes slot's string into the n-byte game buffer at dst, space padded:
 * as it is if it fits, else its first n - 3 bytes and a token. */
static void put_slot(uint32_t dst, uint32_t n, uint32_t slot, const uint8_t* s, uint32_t len) {
    uint32_t i = 0;
    if (len <= n || n < EN_TOKEN_LEN + 1) {
        for (; i < len && i < n; i++) {
            wr_u8(dst + i, s[i]);
        }
    } else {
        for (; i < n - EN_TOKEN_LEN; i++) {
            wr_u8(dst + i, s[i]);
        }
        wr_u8(dst + i++, EN_TOKEN);
        wr_u8(dst + i++, en_digit_byte(slot / 126));
        wr_u8(dst + i++, en_digit_byte(slot % 126));
    }
    for (; i < n; i++) {
        wr_u8(dst + i, ' ');
    }
}

static bool put(uint32_t dst, uint32_t n, uint32_t slot) {
    uint32_t len;
    const uint8_t* s = en_slot(slot, &len);
    if (s == NULL) {
        return false;
    }
    put_slot(dst, n, slot, s, len);
    return true;
}

int en_put_calendar(uint32_t dst, uint32_t n, uint32_t k) {
    uint32_t len;
    if (!en_names_load() || en_slot(sJpTail + JP_TAILS + k, &len) == NULL) {
        return -1;
    }
    put(dst, n, sJpTail + JP_TAILS + k);
    return (int)(len < n ? len : n);
}

int en_expand(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t max) {
    if (sState <= 0) {
        return -1;
    }
    bool any = false;
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == EN_TOKEN && i + 2 < len) {
            int d1 = en_byte_digit(in[i + 1]), d2 = en_byte_digit(in[i + 2]);
            uint32_t slot = (uint32_t)(d1 * 126 + d2), slen;
            const uint8_t* s = (d1 >= 0 && d2 >= 0) ? en_slot(slot, &slen) : NULL;
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

/* The same for the game string (src, len). */
static int expand_rdram(uint32_t src, uint32_t len, uint8_t* out) {
    uint8_t in[256];
    if (len > sizeof(in)) {
        len = sizeof(in);
    }
    for (uint32_t i = 0; i < len; i++) {
        in[i] = rd_u8(src + i);
    }
    return en_expand(in, len, out, EN_TEXT_MAX);
}

/* ---- loaders ------------------------------------------------------------ */

/* void mNpc_LoadNpcNameString(char* name, u8 npcId) */
bool rt_en_villager_name(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = ctx->r5 & 0xFF;
    return ctx->r4 != 0 && id < 0xFF && put((uint32_t)ctx->r4, 6, BASE_VILLAGER + id);
}

/* void mChoice_Load_ChoseStringFromRom(s32 unused, char* str, s32 idx, Actor* actor)
 * (no English choice has a control code, so mChoice_Change_ControlCode is not needed) */
bool rt_en_choice(uint8_t* rdram, recomp_context* ctx) {
    int32_t idx = (int32_t)ctx->r6;
    return idx >= 0 && idx < 460 && put((uint32_t)ctx->r5, 10, BASE_CHOICE + (uint32_t)idx);
}

/* void mString_Load_StringFromRom(char* dst, s32 dstLen, s32 strIdx) */
bool rt_en_string(uint8_t* rdram, recomp_context* ctx) {
    int32_t len = (int32_t)ctx->r5, idx = (int32_t)ctx->r6;
    return len > 0 && idx >= 0 && idx < 0x61A && put((uint32_t)ctx->r4, (uint32_t)len, EN_STRING_SLOT + (uint32_t)idx);
}

/* D_801076BC_jp: where each item-1 category's records start in the item name
 * file, then the furniture table. The tables are packed one after another, so
 * from the sixth on (errand items, carpets, ...) they are off the 10-byte grid
 * of the first; slot numbers are still (offset - 8) / 10 (make_names_en.py). */
static const uint16_t kItemTables[] = { 0x008, 0x288, 0x2B0, 0x418, 0x558, 0xF50, 0x107C, 0x12FC, 0x157C,
                                        0x15C4, 0x1628, 0x1850, 0x185C, 0x1C1C, 0x1D5C, 0x1D70, ITEM_FTR };

/* void func_80096710_jp(char* buf, u32 vrom): DMAs one 10-byte item name record */
bool rt_en_item_name(uint8_t* rdram, recomp_context* ctx) {
    uint32_t off = (uint32_t)ctx->r5 - ITEM_VROM;
    if (!en_names_load() || off >= sItemEnd || off < kItemTables[0]) {
        return false;
    }
    uint32_t slot;
    if (off >= ITEM_FTR) {
        if ((off - ITEM_FTR) % 10) {
            return false;
        }
        slot = ITEM_FTR_SLOT + (off - ITEM_FTR) / 10;
    } else {
        size_t t = 0;
        while (off >= kItemTables[t + 1]) {
            t++;
        }
        if ((off - kItemTables[t]) % 10) {
            return false;
        }
        slot = (off - 8) / 10;
    }
    return put((uint32_t)ctx->r4, 10, BASE_ITEM + slot);
}

/* ---- drawing and measuring a line --------------------------------------- */

/* func_80090CC0_jp(Game*, char* str, s32 len, ...): draws one line of text */
bool rt_en_draw_line(uint8_t* rdram, recomp_context* ctx) {
    static bool sFixedLine = false; /* drawing a line in the font's own cells, below */
    uint8_t buf[EN_TEXT_MAX];
    uint32_t src = (uint32_t)ctx->r5;
    uint32_t len = (uint32_t)ctx->r6;
    if (!sFixedLine && sState > 0 && en_board_date(src, len)) {
        /* drawn here with the cells kept; the game's own call then draws nothing */
        recomp_context c = *ctx;
        sFixedLine = true;
        en_fixed_cells(true);
        func_80090CC0_jp(rdram, &c);
        en_fixed_cells(false);
        sFixedLine = false;
        ctx->r6 = 0;
        return false;
    }
    int n = sState > 0 ? en_letter_line(src, len, buf) : -1;
    if (n < 0 && sState > 0) {
        n = en_screen_text(src, len, buf);
    }
    if (n < 0) {
        n = expand_rdram(src, len, buf);
    }
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_DRAW, buf, (uint32_t)n);
        ctx->r5 = (int32_t)SCRATCH_DRAW;
        ctx->r6 = n;
    }
    return false;
}

/* s32 mFont_GetStringWidth(char* str, s32 len, s32 cut) */
bool rt_en_string_width(uint8_t* rdram, recomp_context* ctx) {
    uint8_t buf[EN_TEXT_MAX];
    int n = sState > 0 ? en_screen_text((uint32_t)ctx->r4, (uint32_t)ctx->r5, buf) : -1;
    if (n < 0) {
        n = expand_rdram((uint32_t)ctx->r4, (uint32_t)ctx->r5, buf);
    }
    if (n >= 0) {
        rt_copy_to_rdram(SCRATCH_WIDTH, buf, (uint32_t)n);
        ctx->r4 = (int32_t)SCRATCH_WIDTH;
        ctx->r5 = n;
    }
    return false;
}

/* ---- the message window's copies ---------------------------------------- */

/* Puts the English the copy from src is to take into SCRATCH_MSG (n bytes). */
static void msg_expansion(uint32_t src, const uint8_t* text, uint32_t n) {
    rt_copy_to_rdram(SCRATCH_MSG, text, n);
    sMsgSrc = src;
}

/* In an mMsg_Copy* function, right after mMsg_Get_Length_String(src, max)
 * returned the length in v0. */
void rt_en_msg_length(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint8_t buf[EN_TEXT_MAX];
    int n = expand_rdram(src, (uint32_t)ctx->r2, buf);
    if (n >= 0) {
        msg_expansion(src, buf, (uint32_t)n);
        ctx->r2 = n;
    }
}

/* mMsg_CopyDetermination, before the text is moved with the length in v1 and pos + length in a1. */
void rt_en_msg_determination(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint8_t buf[EN_TEXT_MAX];
    int n = expand_rdram(src, (uint32_t)ctx->r3, buf);
    if (n >= 0) {
        msg_expansion(src, buf, (uint32_t)n);
        ctx->r5 += n - (int32_t)ctx->r3;
        ctx->r3 = n;
    }
}

/* mMsg_CopyTail, after mMsg_Get_Length_String(catchphrase, 4): a Japanese
 * catchphrase from the save becomes the English one, else as rt_en_msg_length. */
void rt_en_msg_catchphrase(uint8_t* rdram, recomp_context* ctx, uint32_t src) {
    uint32_t len = (uint32_t)ctx->r2;
    if (en_names_load() && len > 0 && len <= 4) {
        uint8_t cur[4];
        for (uint32_t i = 0; i < len; i++) {
            cur[i] = rd_u8(src + i);
        }
        for (uint32_t k = 0; k < JP_TAILS && sJpTail + k < sCount; k++) {
            uint32_t jlen, en_len;
            const uint8_t* jp = en_slot(sJpTail + k, &jlen);
            const uint8_t* en = en_slot(EN_STRING_SLOT + (k < 100 ? 569 + k : 16 + (k - 100)), &en_len);
            if (jp != NULL && en != NULL && jlen == len && memcmp(jp, cur, len) == 0) {
                /* the English one may not fit the 4-byte field, but this copy goes
                 * straight into the message text */
                uint32_t n = en_len < EN_TEXT_MAX ? en_len : EN_TEXT_MAX;
                msg_expansion(src, en, n);
                ctx->r2 = (int32_t)n;
                return;
            }
        }
    }
    rt_en_msg_length(rdram, ctx, src);
}

/* func_8009EB44_jp(dst, src, len): the copy into the message text */
bool rt_en_msg_copy(uint8_t* rdram, recomp_context* ctx) {
    if (sMsgSrc != 0 && (uint32_t)ctx->r5 == sMsgSrc) {
        ctx->r5 = (int32_t)SCRATCH_MSG;
        sMsgSrc = 0;
    }
    return false;
}

/* ---- captures (capture.c) ----------------------------------------------- */

/* A message copy in progress: its source, whose English text is in SCRATCH_MSG. */
void rt_en_names_capture(RtCapture* c) {
    rt_cap_io(c, "NMEN", &sMsgSrc, sizeof(sMsgSrc));
}
