/*
 * english.h -- what the files of the English text share.
 *
 * The port shows Animal Forest in English with the text of the European
 * GameCube release (Animal Crossing), which the build converts from the
 * player's disc (tools/make_text_en.py, tools/make_names_en.py):
 *
 *   dialogue.c  the messages (text_en.bin), and the font's widths for English letters
 *   names.c     names, choices, strings and item names (names_en.bin), with tokens
 *               for those that don't fit the game's buffers, and the hooks that
 *               draw and measure a line of text
 *   letters.c   letters, their tag in the pocket menu, the Happy Room Academy's themes
 *   screen.c    words the game draws from its own data: menu labels, prompts
 *   typing.c    the keyboard, the line being typed, the bulletin board, Mr. Resetti's phrases
 *   dates.c     dates and times
 *
 * The game reaches them through [[patches.hook]]s (recomp/af.jp.toml, declared
 * in recomp_psp.h), named rt_en_*. A hook that returns true has done the
 * game function's work; the others change its arguments, or the registers of
 * the code around them, and let it run.
 */
#ifndef AFPSP_ENGLISH_H
#define AFPSP_ENGLISH_H

#include "rt.h"

/* The longest string the hooks put together (an expanded token, a line of a letter). */
#define EN_TEXT_MAX 64

/* The message line break, which is also where a text ends early in a fixed field. */
#define EN_NEW_LINE 0xCD

/* ---- dialogue.c --------------------------------------------------------- */

/* The drawn width of character c: an English letter's own, 12 for anything else. */
int en_char_width(uint8_t c);
/* The drawn width of n characters. */
uint32_t en_text_width(const uint8_t* s, uint32_t n);
/* While set, the font keeps its own 12-unit cells (a line laid out by them). */
void en_fixed_cells(bool on);

/* ---- names.c: names_en.bin and its tokens -------------------------------- */

/*
 * names_en.bin's slots: villagers from 0, choices from 256, strings from 768,
 * items from 2400 (make_names_en.py). A string that does not fit its N64
 * buffer is stored as its first n - 3 bytes and a token: EN_TOKEN, then the
 * slot number in two digit bytes (en_digit_byte).
 */
#define EN_STRING_SLOT 768
#define EN_TOKEN 0xCE /* an icon glyph that no name, item or choice uses */
#define EN_TOKEN_LEN 3

/* Tries to load names_en.bin (once); true if it is in use. */
bool en_names_load(void);
/* True if names_en.bin has been loaded (without trying to). */
bool en_names_ready(void);
/* The string in a slot (NULL if the slot is empty or there is none). */
const uint8_t* en_slot(uint32_t slot, uint32_t* len);
/* The slots of the letters' texts and of their fixed pieces (see letters.c). */
uint32_t en_mail_base(void);
uint32_t en_piece_base(void);
uint32_t en_slot_count(void);
/* Digits of a slot number, 0..125: bytes 0x81..0xFF without the line break (no control code, space or newline). */
uint8_t en_digit_byte(uint32_t v);
int en_byte_digit(uint8_t b); /* -1 if b is no digit */
/* Expands the tokens in (in, len) into out (max bytes): the new length, or -1 if there was none. */
int en_expand(const uint8_t* in, uint32_t len, uint8_t* out, uint32_t max);
/* Month k (0-11) or weekday k - 12 in full, into the n-byte buffer at dst: the length the game is to use, or -1. */
int en_put_calendar(uint32_t dst, uint32_t n, uint32_t k);

/* ---- letters.c ---------------------------------------------------------- */

/* A line of a letter's body (its line token at src): its English into out; -1 if it is none. */
int en_letter_line(uint32_t src, uint32_t len, uint8_t* out);

/* ---- screen.c ----------------------------------------------------------- */

/* A text the game has in its own data, known by its Japanese bytes (trimmed of spaces). */
typedef struct {
    uint8_t len;
    uint8_t jp[20];
    const char* en;
} EnFixedText;

/* The English for the game's text at src (len bytes) if it is one of the table's, its leading spaces kept; -1 if not. */
int en_fixed_text(const EnFixedText* table, size_t count, uint32_t src, uint32_t len, uint8_t* out, int max);
/* A label, a menu word or a prompt drawn from the game's data: its English, or -1. */
int en_screen_text(uint32_t src, uint32_t len, uint8_t* out);
/* The bulletin board's date, which keeps the font's own cells. */
bool en_board_date(uint32_t src, uint32_t len);

#endif
