/*
 * spram.c -- running the hottest code from the scratchpad.
 *
 * The CPU's caches are 16 KB each and the game and the renderer together run through
 * far more code than that every frame, so most functions are fetched from RAM (about
 * 340 ns a 64-byte line at 222 MHz) each time they are called after something else
 * has run. The scratchpad is 16 KB of memory a user program can use (0x00010000,
 * even for code) with cache-hit latency that nothing evicts. Functions marked RT_SPRAM
 * are linked together (the "spram_text" section) and copied there once at startup:
 *   - calls inside the block, which are absolute jumps to the original addresses,
 *     are pointed at the copies;
 *   - the first two instructions of each original (RT_SPRAM_ENTRY) become a jump to
 *     its copy, for callers outside the block and for function pointers.
 * The originals stay otherwise intact: a jump table entry, which points inside a
 * function, runs the original's code, which is slower but right.
 * no_spram.txt leaves everything where it is.
 */
#include <pspkernel.h>
#include <string.h>

#include "rt.h"

#define SPRAM_BASE 0x00010000u
#define SPRAM_SIZE 0x3FF0u   /* the last 16 bytes hold the marker (rt.h) */

/* (the linker defines these when something is in the section) */
extern char __start_spram_text[] __attribute__((weak));
extern char __stop_spram_text[] __attribute__((weak));
extern void* const __start_spram_ptrs[] __attribute__((weak));
extern void* const __stop_spram_ptrs[] __attribute__((weak));

static uint32_t sBlockStart = 0, sBlockSize = 0;

/* Copies the block into the scratchpad and points its internal calls at the copies. */
static void spram_install(void) {
    uint32_t* copy = (uint32_t*)(uintptr_t)SPRAM_BASE;
    memcpy(copy, (void*)(uintptr_t)sBlockStart, sBlockSize);
    uint32_t delta = SPRAM_BASE - sBlockStart;
    uint32_t patched = 0;
    for (uint32_t i = 0; i < sBlockSize / 4; i++) {
        uint32_t w = copy[i];
        uint32_t op = w >> 26;
        if (op == 2 || op == 3) { /* j, jal */
            uint32_t target = ((w & 0x03FFFFFFu) << 2) | ((sBlockStart + i * 4) & 0xF0000000u);
            if (target >= sBlockStart && target < sBlockStart + sBlockSize) {
                copy[i] = (w & 0xFC000000u) | (((target + delta) >> 2) & 0x03FFFFFFu);
                patched++;
            }
        }
    }
    volatile uint32_t* marker = (volatile uint32_t*)(uintptr_t)RT_SPRAM_MARKER_ADDR;
    marker[1] = sBlockSize;
    marker[0] = RT_SPRAM_MARKER;
    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();
    rt_log("spram: %u bytes of code copied, %u calls redirected", (unsigned)sBlockSize, (unsigned)patched);
}

/*
 * Standby may clear the scratchpad; the originals still jump into it. rt_spram_check
 * (rt.h) finds the marker gone, and the code is copied again before anything runs it.
 */
void rt_spram_restore(void) {
    if (sBlockSize != 0) {
        spram_install();
    }
}

void rt_spram_init(void) {
    if (__start_spram_text == NULL || __stop_spram_text == NULL) {
        return;
    }
    uint32_t start = (uint32_t)(uintptr_t)__start_spram_text;
    uint32_t size = (uint32_t)(uintptr_t)(__stop_spram_text - __start_spram_text);
    if (size == 0 || rt_data_file_exists("no_spram.txt")) {
        return;
    }
    if (size > SPRAM_SIZE) {
        rt_log("spram: %u bytes of hot code do not fit", (unsigned)size);
        return;
    }
    sBlockStart = start;
    sBlockSize = size;
    spram_install();
    uint32_t delta = SPRAM_BASE - start;
    uint32_t entries = 0;
    for (void* const* e = __start_spram_ptrs; e < __stop_spram_ptrs; e++) {
        uint32_t* orig = (uint32_t*)*e;
        uint32_t to = (uint32_t)(uintptr_t)orig + delta;
        orig[0] = (2u << 26) | ((to >> 2) & 0x03FFFFFFu); /* j to the copy */
        orig[1] = 0;                                      /* nop */
        entries++;
    }
    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();
    rt_log("spram: %u entries redirected", (unsigned)entries);
}
