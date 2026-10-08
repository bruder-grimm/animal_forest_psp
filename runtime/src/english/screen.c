/*
 * screen.c -- words the game draws from its own data: menu labels, prompts,
 * warnings, the bulletin board's entries.
 *
 * The fan translation left these Japanese, or cut them short, and they are
 * not in any message bank: they are recognised by their bytes (or, for the
 * pocket menu's option labels, by where they are) whenever a line is drawn or
 * measured (rt_en_draw_line, rt_en_string_width in names.c).
 */
#include <string.h>

#include "english.h"

/* ---- the pocket menu's option labels (tag_ovl) --------------------------- */

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
    if (len == 0 || len > 8 || (src & 3) != 0 || src + 12 > 0x80400000u) {
        return NULL;
    }
    uint32_t handler = rd_w32(src + 8);
    if ((handler >> 24) == 0x80) {
        for (size_t i = 0; i < RT_COUNT(kTagLabels); i++) {
            if (handler - src == kTagLabels[i].handler - kTagLabels[i].record) {
                return kTagLabels[i].en;
            }
        }
    }
    if (len != 8) {
        return NULL;
    }
    bool money = true, bells = true;
    for (uint32_t i = 0; i < 8; i++) {
        uint8_t c = rd_u8(src + i);
        money = money && c == kMoneyJp[i];
        bells = bells && c == kBellsJp[i];
    }
    return money ? "Money:" : bells ? "   Bells" : NULL;
}

/* ---- words known by their bytes ----------------------------------------- */

/*
 * Japanese the fan translation left in the game's own data (menu words, the
 * text entry screen's prompts, the title screen's controller warning, the
 * "new player" choice) or cut short, known by its bytes once trimmed of
 * spaces.
 */
static const EnFixedText kScreenText[] = {
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
    /* the bulletin board's question when a post is done (m_editEndChk_ovl), the GC's words */
    { 9, { 0x09, 0x7E, 0xF4, 0x01, 0x01, 0xF4, 0x0C, 0x05, 0x3F }, "Is this OK?" }, /* これでいいですか? */
    { 2, { 0x19, 0x01 }, "Yes" }, /* はい */
    { 5, { 0x1E, 0xF1, 0x14, 0x04, 0x0C }, "Rewrite" }, /* まだなおす */
    { 9, { 0x14, 0x05, 0xCC, 0x0F, 0x09, 0x13, 0x15, 0x0C, 0x7D }, "Throw it out" }, /* なかったことにする */
};

int en_fixed_text(const EnFixedText* table, size_t count, uint32_t src, uint32_t len, uint8_t* out, int max) {
    uint32_t s = 0, e = len;
    if (len == 0 || len > EN_TEXT_MAX) {
        return -1;
    }
    while (s < e && rd_u8(src + s) == ' ') {
        s++;
    }
    while (e > s && rd_u8(src + e - 1) == ' ') {
        e--;
    }
    if (e == s || e - s > sizeof(table[0].jp)) {
        return -1;
    }
    uint8_t first = rd_u8(src + s);
    for (size_t i = 0; i < count; i++) {
        if (table[i].len != e - s || table[i].jp[0] != first) {
            continue;
        }
        uint32_t k = 1;
        while (k < table[i].len && rd_u8(src + s + k) == table[i].jp[k]) {
            k++;
        }
        if (k < table[i].len) {
            continue;
        }
        size_t en_len = strlen(table[i].en);
        int n = (int)s + (int)en_len;
        if (n > max) {
            return -1;
        }
        memset(out, ' ', s);
        memcpy(out + s, table[i].en, en_len);
        return n;
    }
    return -1;
}

/* ---- the bulletin board -------------------------------------------------- */

/* Its entry number, " 7けんめ" (two places, then "th one"): "Entry 7", as the GC has it. */
static int board_entry(uint32_t src, uint32_t len, uint8_t* out) {
    if (len != 5 || rd_u8(src + 2) != 0x08 || rd_u8(src + 3) != 0xC3 || rd_u8(src + 4) != 0x24) {
        return -1;
    }
    memcpy(out, "Entry ", 6);
    int n = 6;
    for (uint32_t i = 0; i < 2; i++) {
        uint8_t c = rd_u8(src + i);
        if (c >= '0' && c <= '9') {
            out[n++] = c;
        } else if (c != ' ') {
            return -1;
        }
    }
    return n > 6 ? n : -1;
}

/*
 * Its date, "2026 10 03": the board draws the slashes between the parts by
 * themselves, where the Japanese font's 12-unit cells leave the gaps, so the
 * line keeps those cells; with the English letters' widths the slashes stood
 * in the digits.
 */
bool en_board_date(uint32_t src, uint32_t len) {
    static const char kShape[] = "0000 00 00";
    if (len != sizeof(kShape) - 1) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = rd_u8(src + i);
        if (kShape[i] == ' ' ? c != ' ' : (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

/* ---- all of them --------------------------------------------------------- */

int en_screen_text(uint32_t src, uint32_t len, uint8_t* out) {
    const char* label = tag_label(src, len);
    if (label != NULL) {
        size_t n = strlen(label);
        memcpy(out, label, n);
        return (int)n;
    }
    int n = board_entry(src, len, out);
    return n >= 0 ? n : en_fixed_text(kScreenText, RT_COUNT(kScreenText), src, len, out, EN_TEXT_MAX);
}
