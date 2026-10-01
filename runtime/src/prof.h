/*
 * prof.h -- profiling: coarse CPU time accounting for the periodic stats
 * line, and the histograms of the two sampling profilers.
 *
 * PROF_BEGIN/PROF_END bracket a code section and add its wall time to a
 * named slot; rt_prof_report() prints the share of each slot since the
 * previous report. Meant for sections entered at most a few hundred times
 * per frame (the timer is a syscall).
 */
#ifndef AFPSP_PROF_H
#define AFPSP_PROF_H

#include <pspkernel.h>
#include <stdint.h>

typedef enum {
    PROF_GFX_VTX,
    PROF_GFX_MTX,
    PROF_GFX_CLASSIFY,
    PROF_GFX_TEX,
    PROF_GFX_DRAW,
    PROF_GFX_SYNC,
    PROF_EMPTY, /* calibration: an empty section per vertex batch */
    PROF_COUNT
} ProfSlot;

extern uint32_t g_prof_us[PROF_COUNT];
extern uint32_t g_prof_calls[PROF_COUNT];

#ifdef RT_NO_PROF
#define PROF_BEGIN(slot) ((void)0)
#define PROF_END(slot) ((void)0)
#else
#define PROF_BEGIN(slot) uint32_t prof_t0_##slot = sceKernelGetSystemTimeLow()
#define PROF_END(slot)                                                   \
    do {                                                                 \
        g_prof_us[slot] += sceKernelGetSystemTimeLow() - prof_t0_##slot; \
        g_prof_calls[slot]++;                                            \
    } while (0)
#endif

/* Appends "name share% (calls/frame)" for every slot to buf. */
void rt_prof_report(char* buf, int size, uint32_t span_us, uint32_t frames);

/*
 * A sampling profiler's histogram: how often each code address was seen
 * (preempt.c samples game code, gfx/gfx_prof.c the renderer). `slots` is a
 * power of two; addr and count point at arrays of that many zeros.
 */
typedef struct {
    uint32_t* addr;
    uint32_t* count;
    uint32_t slots;
    uint32_t taken;
} RtSamples;

void rt_samples_record(RtSamples* s, uint32_t addr);
/* Writes the histogram next to the EBOOT: a line
 * "base <address of rt_preempt> <samples>", then "<address> <count>" lines. */
void rt_samples_write(const RtSamples* s, const char* file);

#endif
