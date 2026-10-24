/*
 * game_ab.c -- running a second copy of the game's code (scripts/game_ab.sh).
 *
 * An A/B build links the generated game code twice: as usual, and again with
 * every symbol prefixed B_ (compiled the same way, or from a profile). The
 * build adds rt_game_ab_pairs, each function's address in both copies. With
 * game_b.txt next to the PRX the game runs the B copy: the function table
 * (sections.c) hands out B's functions, and every direct jal/j in the program
 * that targets an A function is pointed at its B twin, so even a thread
 * resumed from a capture inside an A function calls B for everything below.
 * A normal build has no pairs and none of this does anything.
 */
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

extern const uint32_t rt_game_ab_pairs[] __attribute__((weak)); /* A, B, A, B, ... */
extern const uint32_t rt_game_ab_count __attribute__((weak));    /* pairs */
extern char _ftext[], _etext[];

static uint32_t* sMap = NULL; /* the pairs sorted by A */
static uint32_t sCount = 0;

static int by_a(const void* x, const void* y) {
    uint32_t a = *(const uint32_t*)x, b = *(const uint32_t*)y;
    return a < b ? -1 : a > b;
}

static uint32_t to_b(uint32_t a) {
    uint32_t lo = 0, hi = sCount;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (sMap[2 * mid] < a) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < sCount && sMap[2 * lo] == a ? sMap[2 * lo + 1] : 0;
}

void* rt_game_b_func(void* f) {
    if (sMap == NULL) {
        return f;
    }
    uint32_t b = to_b((uint32_t)(uintptr_t)f);
    return b != 0 ? (void*)(uintptr_t)b : f;
}

void rt_game_b_init(void) {
    if (rt_game_ab_pairs == NULL || &rt_game_ab_count == NULL || !rt_data_file_exists("game_b.txt")) {
        return;
    }
    sCount = rt_game_ab_count;
    sMap = malloc(sCount * 8);
    if (sMap == NULL) {
        rt_log("game_ab: no memory for the map");
        return;
    }
    memcpy(sMap, rt_game_ab_pairs, sCount * 8);
    qsort(sMap, sCount, 8, by_a);
    uint32_t patched = 0;
    for (uint32_t* p = (uint32_t*)_ftext; p < (uint32_t*)_etext; p++) {
        uint32_t w = *p, op = w >> 26;
        if (op != 2 && op != 3) { /* j, jal */
            continue;
        }
        uint32_t target = ((w & 0x03FFFFFFu) << 2) | ((uint32_t)(uintptr_t)p & 0xF0000000u);
        uint32_t b = to_b(target);
        if (b != 0) {
            *p = (w & 0xFC000000u) | ((b >> 2) & 0x03FFFFFFu);
            patched++;
        }
    }
    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();
    rt_log("game_ab: running the B copy of the game code (%u functions, %u calls redirected)", (unsigned)sCount,
           (unsigned)patched);
}
