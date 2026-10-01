/*
 * gfx_prof.c -- sampling profiler for the renderer (gmake GFXPROF=1 only).
 *
 * The renderer's files are then compiled with -finstrument-functions. A thread
 * raises a flag every 250 us; the next function entry or exit in the renderer
 * records the function whose body was running -- the caller on entry, the
 * function itself on exit -- so time spent in a loop is charged to the function
 * with the loop, not to whatever it calls next. Results go to
 * afpsp_gfxsamples.txt in the same format as preempt.c's.
 * Nothing here may be instrumented itself (prof.c isn't: only gfx/ is).
 */
#include <pspkernel.h>
#include <pspthreadman.h>

#include "prof.h"
#include "rt.h"

#define SLOTS 4096 /* power of two */

static volatile uint8_t sTick = 0;
static uint32_t sAddr[SLOTS], sCount[SLOTS];
static RtSamples sSamples = { sAddr, sCount, SLOTS, 0 };

void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void* fn, void* call_site) {
    (void)fn;
    if (sTick) {
        sTick = 0;
        rt_samples_record(&sSamples, (uint32_t)call_site);
    }
}

void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void* fn, void* call_site) {
    (void)call_site;
    if (sTick) {
        sTick = 0;
        rt_samples_record(&sSamples, (uint32_t)fn + 4);
    }
}

static int __attribute__((no_instrument_function)) sampler(SceSize args, void* argp) {
    for (uint32_t t = 1;; t++) {
        sceKernelDelayThread(250);
        sTick = 1;
        if (t % 20000 == 0) {
            rt_samples_write(&sSamples, "afpsp_gfxsamples.txt");
        }
    }
    return 0;
}

void __attribute__((no_instrument_function)) gfx_prof_init(void) {
    rt_log("gfx: renderer sampling profiler on (afpsp_gfxsamples.txt)");
    SceUID thid = sceKernelCreateThread("rt_gfxprof", sampler, 0x10, 4 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}
