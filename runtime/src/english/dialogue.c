/*
 * dialogue.c -- English dialogue from the GameCube release, and the font's
 * widths for English letters.
 *
 * tools/make_text_en.py converts the English messages of the European
 * Animal Crossing disc into Animal Forest's message format and writes
 * text_en.bin (see that script for the format). Animal Crossing kept Animal
 * Forest's message numbers, so entry N replaces message N; an empty entry
 * leaves the ROM's text.
 *
 * func_8009E558_jp is the one function that loads a message: (buf, msg_no,
 * arg) DMAs message msg_no from ROM file 1883 into buf + 0x10 and fills in
 * the header
 *   buf+0x0  1 (loaded)      buf+0x8  func_8009E4C8_jp(buf + 0x10)
 *   buf+0x4  msg_no          buf+0xC  arg
 * and returns 1. rt_en_message does the same with the English bytes. Only
 * the offset table (47 KB) stays in memory; messages are read from the file,
 * which is packed into EBOOT.PBP (files.c).
 */
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>

#include "english.h"
#include "funcs.h"

#define MSG_MAX 0x400     /* func_8009E388_jp refuses longer messages */
#define MSG_COUNT 0x2DE8  /* ... and message numbers from here on */

static int sState = 0; /* 0 not tried, 1 loaded, -1 absent */
static SceUID sFd = -1;
static char sPath[256];
static uint32_t sCount;
static uint32_t* sOffsets;
static uint32_t sDataStart;

/*
 * Animal Forest's font is monospaced: a character is 12 pixels wide less a
 * per-character cut (func_80090140_jp, table D_80106AF4_jp) that only applies
 * when the character's cut flag is set -- which the message window never
 * does, and the table is all zeros anyway. With the English dialogue loaded,
 * ASCII characters get proportional widths instead. The tables were measured
 * from the font's own glyphs (ROM file 1882 + 0x128, 16x16 cells of 12x16 I4):
 * width = ink width + 1 (+ 2 for glyphs 3 pixels or narrower), and shift =
 * how far into its cell the glyph's box starts, which keeps the ink centred.
 * Codes the font uses for kana or symbols (#$*+/;[\]^`{|}~ -- "/" is a note
 * and "+" a heart, as on the GameCube) stay 12 wide.
 */
static const uint8_t kAsciiWidth[0x7F - 0x20] = {
     5,  4,  6, 12, 12, 11,  9,  5,  5,  5, 12, 12,  5,  9,  5, 12, /* 0x20 */
     9,  6,  9,  9,  9,  9,  9,  9,  9,  9,  4, 12,  8,  9,  8,  8, /* 0x30 */
    11,  9,  8,  9,  8,  7,  7,  9,  8,  4,  6,  8,  7, 10,  8,  9, /* 0x40 */
     8,  9,  8,  8,  7,  8,  9, 11,  8,  9,  8, 12, 12, 12, 12,  9, /* 0x50 */
    12,  7,  7,  7,  7,  7,  5,  7,  7,  4,  5,  7,  5, 10,  7,  8, /* 0x60 */
     7,  7,  5,  7,  5,  7,  7, 11,  7,  7,  6, 12, 12, 12, 12, /* 0x70 */
};
static const uint8_t kAsciiShift[0x7F - 0x20] = {
     0,  4,  2,  0,  0,  1,  3,  1,  6,  2,  0,  0,  1,  2,  1,  0, /* 0x20 */
     2,  3,  2,  2,  2,  2,  2,  2,  2,  2,  4,  0,  2,  2,  2,  2, /* 0x30 */
     1,  2,  3,  2,  3,  3,  3,  2,  3,  4,  3,  3,  3,  1,  2,  2, /* 0x40 */
     3,  2,  3,  3,  3,  3,  2,  1,  3,  2,  2,  0,  0,  0,  0,  2, /* 0x50 */
     0,  3,  3,  3,  3,  3,  4,  3,  3,  4,  3,  3,  3,  1,  3,  2, /* 0x60 */
     3,  3,  4,  3,  4,  3,  3,  1,  3,  3,  3,  0,  0,  0,  0, /* 0x70 */
};

static bool load(void) {
    if (sState != 0) {
        return sState > 0;
    }
    sState = -1;
    uint32_t base, file_size;
    sFd = rt_data_open("text_en.bin", sPath, sizeof(sPath), &base, &file_size);
    if (sFd < 0) {
        rt_log("text_en: no text_en.bin in the EBOOT or next to it: the dialogue stays the ROM's");
        return false;
    }
    uint32_t hdr[3];
    if (sceIoRead(sFd, hdr, sizeof(hdr)) != sizeof(hdr) || memcmp(hdr, "AFEN", 4) != 0 || hdr[1] != 1) {
        rt_log("text_en: %s is not a version 1 AFEN file", sPath);
        sceIoClose(sFd);
        return false;
    }
    sCount = hdr[2];
    uint32_t size = (sCount + 1) * 4;
    sOffsets = malloc(size);
    if (sOffsets == NULL || sceIoRead(sFd, sOffsets, size) != (int)size) {
        rt_log("text_en: could not read the offset table");
        free(sOffsets);
        sceIoClose(sFd);
        return false;
    }
    sDataStart = base + sizeof(hdr) + size;
    sState = 1;
    rt_log("text_en: English dialogue from %s (%u messages)", sPath, (unsigned)sCount);
    return true;
}

bool rt_en_dialogue_active(void) {
    return load();
}

/* s32 func_8009E558_jp(void* buf, s32 msg_no, s32 arg) */
bool rt_en_message(uint8_t* rdram, recomp_context* ctx) {
    uint32_t buf = (uint32_t)ctx->r4;
    int32_t msg_no = (int32_t)ctx->r5;
    uint32_t arg = (uint32_t)ctx->r6;
    if (buf == 0 || msg_no < 0 || msg_no >= MSG_COUNT || !load() || (uint32_t)msg_no >= sCount) {
        return false;
    }
    uint32_t start = sOffsets[msg_no];
    uint32_t len = sOffsets[msg_no + 1] - start;
    if (len == 0 || len > MSG_MAX) {
        return false;
    }
    uint8_t text[MSG_MAX];
    if (rt_read_at(&sFd, sPath, sDataStart + start, text, (int)len) != (int)len) {
        RT_LOG_ONCE("text_en: read failed for message %d", (int)msg_no);
        return false;
    }
    rt_copy_to_rdram(buf + 0x10, text, len);
    wr_w32(buf + 0x0, 1);
    wr_w32(buf + 0x4, (uint32_t)msg_no);
    ctx->r4 = (int32_t)(buf + 0x10);
    func_8009E4C8_jp(rdram, ctx);
    wr_w32(buf + 0x8, (uint32_t)ctx->r2);
    wr_w32(buf + 0xC, arg);
    ctx->r2 = 1;
    return true;
}

/* ---- the font ----------------------------------------------------------- */

static inline bool is_ascii(uint32_t c) {
    return c >= 0x20 && c < 0x7F;
}

int en_char_width(uint8_t c) {
    return is_ascii(c) && load() ? kAsciiWidth[c - 0x20] : 12;
}

uint32_t en_text_width(const uint8_t* s, uint32_t n) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        w += (uint32_t)en_char_width(s[i]);
    }
    return w;
}

static bool sFixed = false;

void en_fixed_cells(bool on) {
    sFixed = on;
}

/* s32 func_8009028C_jp(u8 c, s32 cut): the width of character c. */
bool rt_en_char_width(uint8_t* rdram, recomp_context* ctx) {
    uint8_t c = (uint8_t)ctx->r4;
    if (!is_ascii(c) || sFixed || !load()) {
        return false;
    }
    ctx->r2 = (gpr)en_char_width(c);
    return true;
}

/*
 * void func_8009069C_jp(Gfx** gfxp, s32 c, s32* uls, s32* ult, s32* lrs, s32* lrt)
 * loads the 12x16 cell of character c and returns its texel corners; both
 * draw paths then map [uls, uls + width) onto the glyph's quad. With a
 * proportional width the quad has to start where the glyph does: the hook
 * runs the function and moves uls by the glyph's shift.
 */
static bool sInGlyph = false; /* rt_en_glyph is running the game's function */

bool rt_en_glyph(uint8_t* rdram, recomp_context* ctx) {
    uint32_t c = ctx->r5 & 0xFF;
    if (sInGlyph || sFixed || !is_ascii(c) || !load()) {
        return false;
    }
    uint32_t uls = (uint32_t)ctx->r6;
    sInGlyph = true;
    func_8009069C_jp(rdram, ctx);
    sInGlyph = false;
    wr_w32(uls, rd_w32(uls) + kAsciiShift[c - 0x20]);
    return true;
}

/* ---- captures (capture.c) ----------------------------------------------- */

void rt_en_dialogue_capture(RtCapture* c) {
    rt_cap_io(c, "TXEN", &sInGlyph, sizeof(sInGlyph));
}
