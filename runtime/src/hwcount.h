/* hwcount.h -- the Allegrex's performance counters (hwcount.c). */
#ifndef AFPSP_HWCOUNT_H
#define AFPSP_HWCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#define HWC_WORDS 21 /* PspDebugProfilerRegs and the word after it */

/* w[2] CPU cycles, w[13] I-cache misses, w[14] D-cache misses, w[15] D writebacks, w[4] memory stalls ... (pspdebug.h PspDebugProfilerRegs order) */
typedef struct {
    uint32_t w[HWC_WORDS];
} RtHwc;

bool rt_hwc_init(void);
void rt_hwc_read(RtHwc* out);
void rt_hwc_delta(const RtHwc* a, const RtHwc* b, RtHwc* d);
void rt_hwc_log(const char* what, const RtHwc* d, uint32_t us);
void rt_hwc_bench(void);

/* Section counting (hwc_sections.txt): who has the CPU from now on. */
#define HWC_OWNERS 18
#define HWC_RENDER 16 /* the renderer worker */
#define HWC_IDLE 17   /* the scheduler waiting, or not yet known */
/* (0..15: the game thread with that id) */
void rt_hwc_sections_init(void);
void rt_hwc_owner(int owner);
extern volatile bool gHwcRenderActive; /* set by the renderer worker around a command */
int rt_hwc_idle_owner(void);
void rt_hwc_report(uint32_t frames); /* logs a frame's worth of each owner's counts and starts over */

#endif
