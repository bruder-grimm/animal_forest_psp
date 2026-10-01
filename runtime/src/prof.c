/*
 * prof.c -- see prof.h.
 */
#include <pspiofilemgr.h>
#include <stdio.h>

#include "prof.h"
#include "rt.h"

uint32_t g_prof_us[PROF_COUNT];
uint32_t g_prof_calls[PROF_COUNT];

static const char* const kNames[PROF_COUNT] = { "vtx", "mtx", "comb", "tex", "draw", "sync", "empty" };

void rt_prof_report(char* buf, int size, uint32_t span_us, uint32_t frames) {
    int len = 0;
    for (int i = 0; i < PROF_COUNT && len < size; i++) {
        len += snprintf(buf + len, size - len, " %s %u%% (%u/f)", kNames[i],
                        span_us ? (unsigned)((uint64_t)g_prof_us[i] * 100 / span_us) : 0,
                        frames ? (unsigned)(g_prof_calls[i] / frames) : 0);
        g_prof_us[i] = 0;
        g_prof_calls[i] = 0;
    }
}

/* ---- sampling profilers' histograms ------------------------------------- */

void rt_samples_record(RtSamples* s, uint32_t addr) {
    uint32_t h = (addr >> 2) * 2654435761u;
    for (uint32_t i = 0; i < s->slots; i++) {
        uint32_t k = (h + i) & (s->slots - 1);
        if (s->addr[k] == addr || s->addr[k] == 0) {
            s->addr[k] = addr;
            s->count[k]++;
            break;
        }
    }
    s->taken++;
}

void rt_samples_write(const RtSamples* s, const char* file) {
    char path[256];
    SceUID fd = sceIoOpen(rt_data_path(file, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    /* rt_preempt's address lets a symboliser undo the PRX relocation. */
    char line[48];
    int n = snprintf(line, sizeof(line), "base %08X %u\n", (unsigned)(uintptr_t)&rt_preempt, (unsigned)s->taken);
    sceIoWrite(fd, line, n);
    for (uint32_t k = 0; k < s->slots; k++) {
        if (s->addr[k] != 0) {
            n = snprintf(line, sizeof(line), "%08X %u\n", (unsigned)s->addr[k], (unsigned)s->count[k]);
            sceIoWrite(fd, line, n);
        }
    }
    sceIoClose(fd);
}
