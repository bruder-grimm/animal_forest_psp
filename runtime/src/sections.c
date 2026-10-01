/*
 * sections.c -- section table, overlay tracking and function-pointer lookup.
 *
 * Static sections (makerom, boot, code) live at their link addresses.
 * Overlays are loaded by the game at arbitrary addresses; the Overlay_Load
 * hook tells us where, which drives both RELOC_HI16/LO16 (through
 * section_addresses) and LOOKUP_FUNC for relocated function pointers.
 */
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#include "funcs.h"
#include "librecomp/sections.h"
#include "recomp_overlays.inl" /* generated: section_table, overlay_sections_by_index */

int32_t* section_addresses = NULL;

volatile uint32_t g_indirect_calls = 0;
volatile uint32_t g_last_indirect_vram = 0;

#define STATIC_HASH_SIZE 16384
#define CACHE_SIZE 4096
#define MAX_LOADED 128

typedef struct {
    uint32_t vram;
    recomp_func_t* func;
} FuncSlot;

typedef struct {
    uint32_t start;
    uint32_t end;
    SectionTableEntry* entry;
} LoadedRegion;

static FuncSlot sStaticFuncs[STATIC_HASH_SIZE];
static FuncSlot sCache[CACHE_SIZE];
static LoadedRegion sLoaded[MAX_LOADED];
static int sNumLoaded = 0;
static bool* sIsRelocatable = NULL;

static inline uint32_t hash_vram(uint32_t vram) {
    return (vram >> 2) * 2654435761u;
}

static int compare_func_entries(const void* a, const void* b) {
    const FuncEntry* fa = a;
    const FuncEntry* fb = b;
    return (fa->offset > fb->offset) - (fa->offset < fb->offset);
}

static void static_insert(uint32_t vram, recomp_func_t* func) {
    uint32_t slot = hash_vram(vram) & (STATIC_HASH_SIZE - 1);
    while (sStaticFuncs[slot].func != NULL) {
        if (sStaticFuncs[slot].vram == vram) {
            return;
        }
        slot = (slot + 1) & (STATIC_HASH_SIZE - 1);
    }
    sStaticFuncs[slot].vram = vram;
    sStaticFuncs[slot].func = func;
}

static recomp_func_t* static_find(uint32_t vram) {
    uint32_t slot = hash_vram(vram) & (STATIC_HASH_SIZE - 1);
    while (sStaticFuncs[slot].func != NULL) {
        if (sStaticFuncs[slot].vram == vram) {
            return sStaticFuncs[slot].func;
        }
        slot = (slot + 1) & (STATIC_HASH_SIZE - 1);
    }
    return NULL;
}

void rt_sections_init(void) {
    size_t num_entries = ARRLEN(section_table);

    section_addresses = calloc(num_sections, sizeof(int32_t));
    sIsRelocatable = calloc(num_entries, sizeof(bool));

    for (size_t i = 0; i < ARRLEN(overlay_sections_by_index); i++) {
        int index = overlay_sections_by_index[i];
        if (index >= 0 && (size_t)index < num_entries) {
            sIsRelocatable[index] = true;
        }
    }

    size_t static_count = 0;
    for (size_t i = 0; i < num_entries; i++) {
        SectionTableEntry* entry = &section_table[i];
        section_addresses[entry->index] = (int32_t)entry->ram_addr;
        qsort(entry->funcs, entry->num_funcs, sizeof(FuncEntry), compare_func_entries);

        if (!sIsRelocatable[i]) {
            for (size_t f = 0; f < entry->num_funcs; f++) {
                static_insert(entry->ram_addr + entry->funcs[f].offset, entry->funcs[f].func);
                static_count++;
            }
        }
    }

    rt_log("sections: %u table entries, %u section slots, %u static functions",
           (unsigned)num_entries, (unsigned)num_sections, (unsigned)static_count);
}

/* FuncEntry offsets are relative to the start of their section. */
static recomp_func_t* find_in_entry(SectionTableEntry* entry, uint32_t offset) {
    size_t lo = 0;
    size_t hi = entry->num_funcs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t mid_offset = entry->funcs[mid].offset;
        if (mid_offset == offset) {
            return entry->funcs[mid].func;
        }
        if (mid_offset < offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static void unknown_function(uint8_t* rdram, recomp_context* ctx) {
    rt_log("call through unknown function pointer (ra=%08X)", ctx->r31);
    ctx->r2 = 0;
}

recomp_func_t* get_function(int32_t vram_signed) {
    /*
     * Game code reaches this on every indirect call: a preemption point, like
     * the RECOMP_PREEMPT() at the start of every function (preempt.c).
     */
    g_indirect_calls++;
    g_last_indirect_vram = vram_signed;
    if (g_preempt_hint) {
        g_preempt_hint = 0;
        rt_process_external();
        rt_check_preempt();
    }
    uint32_t vram = (uint32_t)vram_signed;
    FuncSlot* cached = &sCache[hash_vram(vram) & (CACHE_SIZE - 1)];
    if (cached->vram == vram && cached->func != NULL) {
        return cached->func;
    }

    recomp_func_t* func = static_find(vram);
    if (func == NULL) {
        for (int i = sNumLoaded - 1; i >= 0; i--) {
            if (vram >= sLoaded[i].start && vram < sLoaded[i].end) {
                func = find_in_entry(sLoaded[i].entry, vram - sLoaded[i].start);
                break;
            }
        }
    }

    if (func == NULL) {
        rt_log("LOOKUP_FUNC: no function at %08X", vram);
        return unknown_function;
    }

    cached->vram = vram;
    cached->func = func;
    return func;
}

/*
 * A hook at the start of the game's Overlay_Load (recomp/af.jp.toml): the
 * overlay at vrom_start, linked for vram_start, is about to be loaded at
 * `allocated`.
 */
void recomp_overlay_load_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t vrom_start = ctx->r4;
    uint32_t vram_start = rt_stack_arg(ctx, 4);
    uint32_t allocated = rt_stack_arg(ctx, 6);

    SectionTableEntry* entry = NULL;
    for (size_t i = 0; i < ARRLEN(section_table); i++) {
        if (sIsRelocatable[i] && section_table[i].rom_addr == vrom_start) {
            entry = &section_table[i];
            break;
        }
    }

    if (entry == NULL) {
        rt_log("overlay load: no section for vrom %08X (vram %08X -> %08X)", vrom_start, vram_start, allocated);
        return;
    }
    if (entry->ram_addr != vram_start) {
        rt_log("overlay load: vram mismatch for vrom %08X: table %08X, game %08X", vrom_start, entry->ram_addr, vram_start);
    }

    uint32_t start = allocated;
    uint32_t end = allocated + entry->size;

    /* Drop regions this load overlaps; the memory they occupied has been freed and reused. */
    int out = 0;
    for (int i = 0; i < sNumLoaded; i++) {
        bool overlaps = sLoaded[i].start < end && start < sLoaded[i].end;
        if (!overlaps) {
            sLoaded[out++] = sLoaded[i];
        }
    }
    sNumLoaded = out;

    if (sNumLoaded == MAX_LOADED) {
        memmove(&sLoaded[0], &sLoaded[1], sizeof(sLoaded[0]) * (MAX_LOADED - 1));
        sNumLoaded--;
    }
    sLoaded[sNumLoaded].start = start;
    sLoaded[sNumLoaded].end = end;
    sLoaded[sNumLoaded].entry = entry;
    sNumLoaded++;

    section_addresses[entry->index] = (int32_t)allocated;
    memset(sCache, 0, sizeof(sCache));
}
