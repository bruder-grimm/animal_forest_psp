/*
 * typing.c -- typing in English: the text entry keyboard, the line being
 * typed, the bulletin board's editor, and Mr. Resetti's "say exactly what I
 * tell you".
 *
 * The editors lay their extras -- cursor, boxes, the "End" mark -- out at 12
 * units a character, the width of every character of the Japanese font. The
 * English letters are narrower (dialogue.c), so the hooks below place those
 * after the text as it is drawn.
 */
#include <string.h>

#include "english.h"

/* Where a cursor goes when the game places it at x0 + 12 * f16 with f16 = column - 1 + 0.3: after `width` units of text. */
static float cursor_column(uint32_t width) {
    return ((float)width - 5.5f) / 12.0f;
}

/*
 * The text entry keyboard (ovl__0078CB80) opens on hiragana; with the English
 * text it opens on ABC (mode 3: ひらがな, @_@, カタカナ, ABC, 123). Called at the end
 * of its state init, func_8088587C_jp, with the state in v1.
 */
void rt_en_keyboard_page(uint8_t* rdram, recomp_context* ctx) {
    if (en_names_ready()) {
        wr_u8((uint32_t)ctx->r3 + 4, 3);
    }
}

/*
 * The line being typed (m_ledit_ovl, func_80884514_jp) puts a box over every
 * space and the cursor after the text: the box of the first space sat on the
 * last letter typed and the cursor stood at the far end of the field.
 * rt_en_typed_space: before the test for a space (t8 the character): no
 * box, as a space's 5 units have no room for one.
 * rt_en_typed_cursor: before x = x0 + 12 * f16, with the editor's state in
 * s4 (cursor +0x16, text +0x24).
 */
void rt_en_typed_space(uint8_t* rdram, recomp_context* ctx) {
    if (en_names_ready() && rt_en_dialogue_active()) {
        ctx->r24 = 0;
    }
}

void rt_en_typed_cursor(uint8_t* rdram, recomp_context* ctx) {
    if (!en_names_ready() || !rt_en_dialogue_active()) {
        return;
    }
    uint32_t state = (uint32_t)ctx->r20, text = rd_w32(state + 0x24), w = 0;
    int cursor = (int16_t)rd_u16(state + 0x16);
    for (int i = 0; i < cursor && i < EN_TEXT_MAX; i++) {
        w += (uint32_t)en_char_width(rd_u8(text + (uint32_t)i));
    }
    ctx->f16.fl = cursor_column(w);
}

/*
 * The bulletin board (ovl__00797A50): a post is 6 lines of at most 16 bytes,
 * a line ending early after a line break.
 * rt_en_board_cursor: in func_8089562C_jp before x = x0 + 12 * f16, with the
 * editor in s0 (column +0x20, line +0x22, text +0x24).
 * rt_en_board_end: in func_8089542C_jp after end_x = x + 12 * s0 - 160 is
 * stored at s6, for the last line (s0 bytes ending at s5).
 */
#define BOARD_LINE 16
#define BOARD_LINES 6

/* The drawn width of a line of the board: n bytes at s, line breaks drawing nothing. */
static uint32_t board_width(uint32_t s, int n) {
    uint32_t w = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = rd_u8(s + (uint32_t)i);
        if (c != EN_NEW_LINE) {
            w += (uint32_t)en_char_width(c);
        }
    }
    return w;
}

void rt_en_board_cursor(uint8_t* rdram, recomp_context* ctx) {
    if (!en_names_ready() || !rt_en_dialogue_active()) {
        return;
    }
    uint32_t editor = (uint32_t)ctx->r16, s = rd_w32(editor + 0x24);
    int column = (int16_t)rd_u16(editor + 0x20), line = (int16_t)rd_u16(editor + 0x22);
    if (column < 0 || column > BOARD_LINE || line < 0 || line >= BOARD_LINES) {
        return;
    }
    for (int k = 0; k < line; k++) {
        int n = 0;
        while (n < BOARD_LINE && rd_u8(s + (uint32_t)n++) != EN_NEW_LINE) {
        }
        s += (uint32_t)n;
    }
    ctx->f16.fl = cursor_column(board_width(s, column));
}

void rt_en_board_end(uint8_t* rdram, recomp_context* ctx) {
    if (!en_names_ready() || !rt_en_dialogue_active()) {
        return;
    }
    int n = (int)ctx->r16;
    if (n <= 0 || n > BOARD_LINE) {
        return;
    }
    uint32_t end_x = (uint32_t)ctx->r22;
    float x = rt_f32(rd_w32(end_x));
    x += (float)board_width((uint32_t)ctx->r21 - (uint32_t)n, n) - 12.0f * (float)n;
    wr_w32(end_x, rt_f32_bits(x));
}

/*
 * Mr. Resetti's "say exactly what I tell you" (ovl_Npc_Majin3): the phrase is
 * string 0x484 + actor[0x956], typed into actor[0x94C] (10 bytes). The game
 * compares the bytes, but the keyboard's ABC page types capitals only (R
 * turns the last letter typed into a small one), so with the English phrases
 * a letter's case does not count.
 * It then looks for one of 32 rude words (strings 0x4C0..) anywhere in the
 * text, each at a fixed length of 2 to 7 bytes that the English words do not
 * have ("Moles suck" matched every answer beginning "Moles "): the English
 * list is here instead.
 */
#define RESETTI_TEXT 0x94C
#define RESETTI_PHRASE 0x956
#define RESETTI_LEN 10

static uint8_t lower(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 0x20) : c;
}

/* s32 func_809B4BB8_jp(Actor*): 1 if the text typed is the phrase asked for */
bool rt_en_resetti_match(uint8_t* rdram, recomp_context* ctx) {
    uint32_t actor = (uint32_t)ctx->r4, len;
    const uint8_t* s = en_slot(EN_STRING_SLOT + 0x484 + rd_u8(actor + RESETTI_PHRASE), &len);
    if (s == NULL || len > RESETTI_LEN) {
        return false;
    }
    ctx->r2 = 1;
    for (uint32_t i = 0; i < RESETTI_LEN; i++) {
        if (lower(rd_u8(actor + RESETTI_TEXT + i)) != lower(i < len ? s[i] : ' ')) {
            ctx->r2 = 0;
        }
    }
    return true;
}

/* s32 func_809B4C18_jp(Actor*): 1 if the text typed has a rude word in it */
bool rt_en_resetti_rude(uint8_t* rdram, recomp_context* ctx) {
    static const char* const kRude[] = {
        "jerk", "die!", "loser", "freak", "creep", "no way", "leave", "shut up", "go away", "pinhead",
        "dirtbag", "scumbag", "butthead", "bite me", "ugly", "groundhog", "you stink", "suck", "hate",
        "who cares", "i + reset",
    };
    uint32_t actor = (uint32_t)ctx->r4, len;
    if (en_slot(EN_STRING_SLOT + 0x4C0, &len) == NULL) {
        return false;
    }
    char text[RESETTI_LEN + 1];
    for (uint32_t i = 0; i < RESETTI_LEN; i++) {
        text[i] = (char)lower(rd_u8(actor + RESETTI_TEXT + i));
    }
    text[RESETTI_LEN] = '\0';
    ctx->r2 = 0;
    for (size_t i = 0; i < RT_COUNT(kRude); i++) {
        if (strstr(text, kRude[i]) != NULL) {
            ctx->r2 = 1;
        }
    }
    return true;
}
