/*
 * preempt.c -- lets a long stretch of recompiled code be interrupted.
 *
 * On the N64 the OS preempts a thread wherever it happens to be, so the audio
 * manager always gets its turn every retrace. Our game threads hand over only
 * where the runtime is entered: at OS calls, and inside recompiled code at
 * get_function(), which game code reaches only on an indirect call. Loading a
 * village takes the game through some 800 ms of work with a single indirect
 * call in it, and everything else waited behind all of it -- about fifty
 * retraces' worth of sound, never generated.
 *
 * scripts/recompile.sh therefore opens every recompiled function with
 * RECOMP_PREEMPT() (recomp_psp.h), which costs a byte load and a branch until
 * the scheduler asks for a hand-over. The longest stretch without one is then
 * a single function body, which the stats line reports as `h`.
 *
 * The same hook doubles as a sampling profiler of the game's own code
 * (prof_sample.txt next to the EBOOT): a thread raises the hint every
 * millisecond and the next function entered records its address. The
 * histogram goes to afpsp_samples.txt ("address count", cumulative, with the
 * address of rt_preempt on the first line so the PRX relocation can be undone
 * and the functions named from build/psp/afpsp.elf).
 */
#include <pspkernel.h>
#include <pspthreadman.h>

#include "prof.h"
#include "rt.h"

#define SAMPLE_SLOTS 8192 /* power of two */

static volatile uint8_t sSampleTick = 0;
static uint32_t sSampleAddr[SAMPLE_SLOTS];
static uint32_t sSampleCount[SAMPLE_SLOTS];
static RtSamples sSamples = { sSampleAddr, sSampleCount, SAMPLE_SLOTS, 0 };

void __attribute__((noinline)) rt_preempt(void) {
    if (sSampleTick) {
        sSampleTick = 0;
        rt_samples_record(&sSamples, (uint32_t)__builtin_return_address(0));
    }
    g_preempt_hint = 0;
    rt_process_external();
    rt_check_preempt();
}

static int sampler_thread(SceSize args, void* argp) {
    uint32_t ticks = 0;
    for (;;) {
        sceKernelDelayThread(1000);
        sSampleTick = 1;
        g_preempt_hint = 1;
        if (++ticks % 10000 == 0) {
            rt_samples_write(&sSamples, "afpsp_samples.txt");
        }
    }
    return 0;
}

void rt_preempt_init(void) {
    if (!RT_SWITCH("prof_sample.txt")) {
        return;
    }
    rt_log("preempt: sampling game code every 1 ms to afpsp_samples.txt");
    SceUID thid = sceKernelCreateThread("rt_sampler", sampler_thread, 0x10, 4 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}
