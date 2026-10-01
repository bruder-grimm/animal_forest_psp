/*
 * strings_en.c -- English dates and times, for the English fan translation
 * and for the GameCube dialogue.
 *
 * The game builds a date or time out of a number and a unit word from its
 * string table (m_string.c): "9" + "がつ" (month), "25" + "にち" (day),
 * "2026" + "ねん" (year), "ごご" (PM) + "3" + "じ" (hour), "11" + "ふん"
 * (minute). The translation ("Animal Forest (U) [!]") rewrote the messages
 * but left that table alone, so on the N64 too Rover asks
 * "Is today 9がつ25にち2026ねん and is it now ごご3じ11ふん?".
 *
 * Every translated message uses these as [month][day][year] and
 * [hour][minute] ("In <town>, it's <date> at <time>."), so with the English
 * ROM the functions below write "Sep 25, 2026" and "3:11pm" instead. Each
 * piece stays within the buffer the game gives it (4 bytes for month, day
 * and minute, 6 for year and hour, 5 for seconds and weekday) and is padded
 * with spaces the way mString_Load_StringFromRom pads.
 *
 * With the GameCube dialogue (text_en.bin) the messages are laid out the
 * GameCube's way instead -- "it's the [day] of [month], [year], at
 * [hour]:[minute]", "on [weekday]s" -- so the pieces become "29th",
 * "September", "2026", "10", "32pm" and "Sunday". Full month and weekday
 * names don't fit their buffers and go through names_en.c's tokens.
 *
 * recomp/af.jp.toml hooks the start of each mString_Load_*StringFromRom;
 * a hook that returns true has done the work. With the Japanese ROM and no
 * English dialogue (text_en.bin) they return false and the game's own code
 * runs.
 */
#include <stdio.h>
#include <string.h>

#include "rt.h"

static int sEnglish = -1;
static uint32_t sLastHour = 0; /* the minute's am/pm comes from the hour before it */

static bool english(void) {
    if (sEnglish < 0) {
        uint8_t title[20];
        rt_rom_read(0x20, title, sizeof(title));
        bool fan = memcmp(title, "ANIMAL FOREST", 13) == 0;
        /* The GameCube dialogue is English whatever ROM it is read over. */
        sEnglish = fan || rt_text_en_active();
        if (sEnglish) {
            rt_log("strings: %s, dates and times in English", fan ? "English ROM" : "English dialogue");
        }
    }
    return sEnglish != 0;
}

/* Writes text to the game buffer at dst, pads it to size with spaces and
 * returns the length, as the functions it stands in for do. */
static uint32_t put(uint32_t dst, uint32_t size, const char* text) {
    uint32_t len = (uint32_t)strlen(text);
    if (len > size) {
        len = size;
    }
    for (uint32_t i = 0; i < size; i++) {
        wr_u8(dst + i, i < len ? (uint8_t)text[i] : ' ');
    }
    return len;
}

static const char* const kMonths[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};
static const char* const kWeekdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* s32 mString_Load_YearStringFromRom(char* dst, lbRTC_year_t year): " 2026" (GC: "2026") */
bool rt_str_year(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t year = ctx->r5 & 0xFFFF;
    if (year < 1901 || year > 2099) {
        year = 2001;
    }
    char text[8];
    snprintf(text, sizeof(text), rt_text_en_active() ? "%u" : " %u", (unsigned)year);
    ctx->r2 = put(ctx->r4, 6, text);
    return true;
}

/* s32 mString_Load_MonthStringFromRom(char* dst, lbRTC_month_t month): "Sep" (GC: "September") */
bool rt_str_month(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t month = ctx->r5 & 0xFF;
    if (month < 1 || month > 12) {
        month = 1;
    }
    int n = rt_text_en_active() ? rt_names_put_calendar((uint32_t)ctx->r4, 4, month - 1) : -1;
    ctx->r2 = n >= 0 ? (uint32_t)n : put(ctx->r4, 4, kMonths[month - 1]);
    return true;
}

/* s32 mString_Load_DayStringFromRom(char* dst, lbRTC_day_t day): " 25," (GC: "25th") */
bool rt_str_day(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t day = ctx->r5 & 0xFF;
    if (day < 1 || day > 31) {
        day = 1;
    }
    char text[8];
    if (rt_text_en_active()) {
        const char* th = (day % 10 == 1 && day != 11) ? "st" : (day % 10 == 2 && day != 12) ? "nd"
                         : (day % 10 == 3 && day != 13) ? "rd" : "th";
        snprintf(text, sizeof(text), "%u%s", (unsigned)day, th);
    } else {
        snprintf(text, sizeof(text), " %u,", (unsigned)day);
    }
    ctx->r2 = put(ctx->r4, 4, text);
    return true;
}

/* s32 mString_Load_WeekStringFromRom(char* dst, lbRTC_weekday_t weekday): "Sun" (GC: "Sunday") */
bool rt_str_week(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t weekday = ctx->r5 & 0xFF;
    if (weekday > 6) {
        weekday = 0;
    }
    int n = rt_text_en_active() ? rt_names_put_calendar((uint32_t)ctx->r4, 5, 12 + weekday) : -1;
    ctx->r2 = n >= 0 ? (uint32_t)n : put(ctx->r4, 5, kWeekdays[weekday]);
    return true;
}

/* s32 mString_Load_HourStringFromRom(char* dst, lbRTC_hour_t hour): "3:" (GC: "3", the text has the colon) */
bool rt_str_hour(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t hour = ctx->r5 & 0xFF;
    if (hour >= 24) {
        hour = 0;
    }
    sLastHour = hour;
    char text[8];
    snprintf(text, sizeof(text), rt_text_en_active() ? "%u" : "%u:", (unsigned)(hour % 12 == 0 ? 12 : hour % 12));
    ctx->r2 = put(ctx->r4, 6, text);
    return true;
}

/* s32 mString_Load_MinStringFromRom(char* dst, lbRTC_min_t min): "05pm" */
bool rt_str_min(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t min = ctx->r5 & 0xFF;
    if (min >= 60) {
        min = 0;
    }
    char text[8];
    snprintf(text, sizeof(text), "%02u%s", (unsigned)min, sLastHour < 12 ? "am" : "pm");
    ctx->r2 = put(ctx->r4, 4, text);
    return true;
}

/* s32 mString_Load_SecStringFromRom(char* dst, lbRTC_sec_t sec): "05s" */
bool rt_str_sec(uint8_t* rdram, recomp_context* ctx) {
    if (!english()) {
        return false;
    }
    uint32_t sec = ctx->r5 & 0xFF;
    if (sec >= 60) {
        sec = 0;
    }
    char text[8];
    snprintf(text, sizeof(text), "%02us", (unsigned)sec);
    ctx->r2 = put(ctx->r4, 5, text);
    return true;
}
