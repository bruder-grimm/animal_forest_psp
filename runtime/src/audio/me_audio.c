/*
 * me_audio.c -- runs audio RSP tasks on the PSP's Media Engine.
 *
 * The Media Engine is a second Allegrex core. mcidclan's me-core library
 * (psp-media-engine-custom-core) boots it into meLibOnProcess() below, which
 * runs code straight from this module. Audio tasks are handed over through a
 * small shared block and processed by the same bit-exact HLE (aspmain.c) as
 * on the main CPU.
 *
 * The task is reported done to the game as soon as the ME picks it up, so the
 * game's scheduler goes straight on to the graphics task instead of waiting
 * for the RSP; whoever reads the samples (the AI path, through
 * rt_me_audio_before_read) waits for the ME instead. Only one task is in
 * flight at a time, so a task's output buffer is finished well before the
 * game reads it.
 *
 * Memory: both CPUs have their own caches. The ME reads and writes RDRAM
 * through its own cache (uncached access is far too slow for this), so it
 * drops its cache before a task and writes it back afterwards; the main CPU
 * flushes its cache before handing a task over and reads the samples through
 * the uncached alias. The handshake itself uses the uncached alias.
 *
 * Between tasks the ME also converts the game's samples to the DAC's rate, a
 * chunk at a time (rt_rs_me_poll, resample.c): its idle loop is that work
 * plus a look at two counters, and a task that arrives waits for one chunk at
 * most.
 *
 * Needs a custom firmware that lets homebrew load the library's small kernel
 * PRX (kcall.prx, next to the EBOOT). Without a working ME (or with no_me.txt
 * next to the EBOOT; PPSSPP doesn't emulate it) tasks run on the main CPU.
 */
#include <stdio.h>
#include <string.h>

#include <pspkernel.h>
#include <pspthreadman.h>

#include <me-core-mapper/me-core.h>

#include "me_audio.h"
#include "resample.h"
#include "rt.h"

#define ME_TIMEOUT_US 200000

/* Both CPUs use this through the uncached alias; it has cache lines to itself,
 * so neither's cache writes a stale copy of a neighbour over it. */
typedef struct {
    u32 seq;        /* bumped by the main CPU to submit a task */
    u32 ack;        /* set to seq by the ME when the task is done */
    u32 alive;      /* incremented by the ME while idle */
    u32 rdram;      /* RDRAM base for the ME */
    u32 out;        /* sample buffer of the last task the ME finished */
    u32 quit;       /* 1 = main CPU asks the ME to stop, 2 = ME halted */
    u32 starts;     /* times the ME has started our code */
    u32 fcr31;      /* its FPU control word at the first start, set again at every later one */
    AspTask task;
} __attribute__((aligned(64))) MeShared;

static MeShared sShared;
#define SHARED ((volatile MeShared*)(UNCACHED_USER_MASK | (u32)&sShared))

/* Runs on the ME, from resample.c. */
void rt_me_cache_invalidate(uint32_t addr, uint32_t size) {
    uint32_t start = addr & ~63u;
    meLibDcacheInvalidateRange(start, ((addr + size + 63) & ~63u) - start);
}

void rt_me_cache_writeback(uint32_t addr, uint32_t size) {
    uint32_t start = addr & ~63u;
    meLibDcacheWritebackRange(start, ((addr + size + 63) & ~63u) - start);
}

/* Called through kcall: checks that kernel calls really work. */
static int probe_kernel(void) {
    return 0x1234;
}

/* me_boot.S: empties the ME's caches before the library's start-up code runs. */
extern char rt_me_boot[], rt_me_boot_end[];

/*
 * Starts the ME core, in kernel mode. This is meLibDefaultInit() without its
 * sleep/wake hook (install_sleep_hook below does that part, once the ME is
 * known to be there), and its meLibReset() with me_boot.S put in front of the
 * library's code at the reset vector.
 */
static int start_me_core(void) {
    int table = meCoreGetTableIdFromWitnessWord();
    if (table < ME_CORE_T2_IMG_TABLE) {
        return ERROR_ON_ME_IMG;
    }
    meCoreSelectSystemTable(table);
    u32 boot_size = (u32)(rt_me_boot_end - rt_me_boot);
    memcpy((void*)ME_HANDLER_BASE, rt_me_boot, boot_size);
    memcpy((void*)(ME_HANDLER_BASE + boot_size), &__start__me_section, (u32)(&__stop__me_section - &__start__me_section));
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    HW_SYS_RESET_ENABLE = SC_HW_RESET;
    HW_SYS_RESET_ENABLE = 0;
    meLibSync();
    return table;
}

static bool sMeReady = false;
static bool sMeStarted = false;   /* the ME runs our code (even if it timed out since) */
static u32 sSubmitTime = 0;

/* How many tasks the ME ran, and how often the main CPU had to wait for one. */
static u32 sWaits = 0;
static u32 sTasks = 0;
static float sIdleRate = 0;   /* idle-loop counts per us with the ME at rest */
static u32 sAliveMark = 0, sAliveTime = 0;

void meLibOnProcess(void) {
    volatile MeShared* sh = SHARED;
    /*
     * The FPU's control word (rounding, traps) is the one the firmware's ME
     * code left the first time, and whatever the hardware came up with after
     * a standby: the audio code has to find the same one every time.
     */
    u32 fcr31;
    if (sh->starts == 0) {
        asm volatile("cfc1 %0, $31" : "=r"(fcr31));
        sh->fcr31 = fcr31;
    } else {
        fcr31 = sh->fcr31;
        asm volatile("ctc1 %0, $31" : : "r"(fcr31));
    }
    sh->starts = sh->starts + 1;
    u32 done = sh->ack;
    for (;;) {
        u32 seq = sh->seq;
        if (seq != done) {
            AspTask task = sh->task;
            /* Forget what the main CPU may have changed since the last task. */
            meLibDcacheWritebackInvalidateAll();
            asp_run_task((uint8_t*)sh->rdram, &task);
            u32 out = asp_out_buffer;
            /* Put the samples in memory before saying the task is done. */
            meLibDcacheWritebackInvalidateAll();
            done = seq;
            sh->out = out;
            sh->ack = seq;
            meLibSync();
        } else if (sh->quit != 0) {
            /* rt_me_audio_shutdown: the task in hand is done; stop for good. */
            meLibDcacheWritebackInvalidateAll();
            sh->quit = 2;
            meLibSync();
            meLibHalt();
        } else if (rt_rs_me_poll()) {
            /* A chunk of the audio output's rate conversion (resample.c): one at a time, so that a task
             * that arrives meanwhile waits for at most one. */
        } else {
            sh->alive++;
            meLibDelayPipeline();
        }
    }
}

/* Kernel mode: stops the ME dead, cache and all. */
static int hold_reset(void) {
    HW_SYS_RESET_ENABLE = SC_HW_RESET;
    meLibSync();
    return 0;
}

/*
 * Standby. The firmware's "SceMeRpc" system event handler tells the ME
 * firmware to suspend and waits for its answer; with our code on the ME no
 * answer comes and the PSP powers off instead of going to sleep. As
 * meLibDefaultInit() does, that handler is replaced with this one (kinit):
 * before the suspend the ME finishes the task in hand and halts, then is held
 * in reset, and audio tasks go to the main CPU. It runs in kernel mode with
 * the other threads stopped, so it spins rather than sleeps. The ME is booted
 * again from the power callback once the PSP has resumed
 * (rt_me_audio_resume), the same way as at startup. The event ID is the one
 * the library's own handler uses.
 */
#define SYSEVENT_SUSPEND 0x00000402

static bool sMeAsleep = false;

/* Waits (up to 200 ms) for the ME's idle loop to count past `alive`: is our code running there? */
static bool heartbeat_after(u32 alive) {
    volatile MeShared* sh = SHARED;
    for (int i = 0; i < 20 && sh->alive == alive; i++) {
        sceKernelDelayThread(10000);
    }
    return sh->alive != alive;
}

/* Starts a new period for rt_me_audio_report's busy figure. */
static void mark_alive(void) {
    sAliveMark = SHARED->alive;
    sAliveTime = sceKernelGetSystemTimeLow();
}

static int me_sysevent(int ev_id, char* ev_name, void* param, int* result) {
    volatile MeShared* sh = SHARED;
    if (ev_id == SYSEVENT_SUSPEND && sMeStarted && !sMeAsleep) {
        sMeReady = false;
        sh->quit = 1;
        for (u32 i = 0; i < 2000000 && sh->quit != 2; i++) {
        }
        HW_SYS_RESET_ENABLE = SC_HW_RESET;
        meLibSync();
        sMeAsleep = true;
    }
    return 0;
}

/*
 * After a resume: boots the ME again. A task submitted while it was stopping
 * (seq != ack) is picked up by the new meLibOnProcess().
 */
void rt_me_audio_resume(void) {
    volatile MeShared* sh = SHARED;
    if (!sMeAsleep) {
        return;
    }
    sh->quit = 0;
    u32 alive = sh->alive;
    int table = kcall(start_me_core, 0);
    sMeAsleep = false;
    if (table < 0 || !heartbeat_after(alive)) {
        rt_log("me: did not restart after the resume (%d), audio stays on the main CPU", table);
        return;
    }
    sSubmitTime = sceKernelGetSystemTimeLow();
    mark_alive();
    sMeReady = true;
    rt_log("me: restarted after the resume");
}

/* Only on hardware: on emulators the firmware's handler list isn't there. */
static void install_sleep_hook(void) {
    if (kinit((void*)me_sysevent) < 0) {
        rt_log("me: no SceMeRpc event handler found, standby will power off");
    }
}

bool rt_me_audio_ready(void) {
    return sMeReady;
}

bool rt_me_audio_busy(void) {
    volatile MeShared* sh = SHARED;
    return sMeReady && sh->ack != sh->seq;
}

/* Spins until the task in flight is done; gives up if the ME stopped. */
static void wait_done(void* arg) {
    volatile MeShared* sh = SHARED;
    while (sh->ack != sh->seq) {
        if (sceKernelGetSystemTimeLow() - sSubmitTime > ME_TIMEOUT_US) {
            /* Hold it in reset: finishing late, it would write the task's
             * samples and its cache over what the main CPU has done since. */
            sMeReady = false;
            kcall(hold_reset, 0);
            rt_log("me: audio task timed out (seq %u, ack %u, alive %u), stopped it; using the main CPU from now on",
                   (unsigned)sh->seq, (unsigned)sh->ack, (unsigned)sh->alive);
            return;
        }
        sceKernelDelayThread(100);
    }
}

/*
 * Waits for the task the ME is running: before handing it the next one, and
 * before the game reads samples it may still be writing.
 */
static void wait_for_me(void) {
    volatile MeShared* sh = SHARED;
    if (!sMeReady || sh->ack == sh->seq) {
        return;
    }
    uint64_t t0 = sceKernelGetSystemTimeWide();
    sWaits++;
    /* Other game threads keep running while this one waits. */
    rt_sched_native_wait(wait_done, NULL);
    g_audio_us += sceKernelGetSystemTimeWide() - t0;
}

/*
 * Called before the game reads a buffer of samples. The game plays the buffer
 * the previous task filled, which the ME has finished with -- one task runs at
 * a time -- so this normally returns straight away; anything else waits.
 */
void rt_me_audio_before_read(uint32_t addr) {
    volatile MeShared* sh = SHARED;
    if (!sMeReady || sh->ack == sh->seq) {
        return;
    }
    if ((addr & RDRAM_MASK) == (sh->out & RDRAM_MASK)) {
        return;
    }
    wait_for_me();
}

void rt_me_audio_report(char* buf, int size) {
    /* The ME's load: 1 - (idle loop rate now / idle loop rate at rest). */
    unsigned busy = 0;
    if (sMeReady && sIdleRate > 0) {
        volatile MeShared* sh = SHARED;
        u32 a = sh->alive;
        u32 t = sceKernelGetSystemTimeLow();
        float rate = (float)(a - sAliveMark) / (float)(t - sAliveTime);
        float f = 1.0f - rate / sIdleRate;
        busy = f <= 0 ? 0 : (unsigned)(f * 100.0f + 0.5f);
        sAliveMark = a;
        sAliveTime = t;
    }
    snprintf(buf, size, " me %u/%u (ME %u%% busy)", (unsigned)sWaits, (unsigned)sTasks, busy);
    sWaits = 0;
    sTasks = 0;
}

void rt_me_audio_init(void) {
    if (rt_data_file_exists("no_me.txt")) {
        rt_log("me: disabled by no_me.txt");
        return;
    }
    volatile MeShared* sh = SHARED;
    sh->seq = sh->ack = 0;
    sh->alive = 0;
    sh->quit = 0;
    sh->starts = 0;
    int loaded = meLibLoadPrx();
    if (loaded < 0) {
        rt_log("me: no kernel module (%d), audio runs on the main CPU", loaded);
        return;
    }
    if (kcall(probe_kernel, 0) != 0x1234) {
        rt_log("me: kernel calls don't work here, audio runs on the main CPU");
        return;
    }
    int table = kcall(start_me_core, 0);
    if (table < 0) {
        rt_log("me: no media engine here (%d), audio runs on the main CPU", table);
        return;
    }
    sMeStarted = true;
    install_sleep_hook();
    if (!heartbeat_after(0)) {
        rt_log("me: no heartbeat, audio runs on the main CPU");
        return;
    }
    sMeReady = true;
    rt_log("me: audio tasks run on the Media Engine (core image %d)", table);

    /* How fast the idle loop counts when the ME has nothing to do: the
     * baseline for how busy it is later (rt_me_audio_report). */
    u32 a0 = sh->alive;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    sceKernelDelayThread(100000);
    u32 dt = sceKernelGetSystemTimeLow() - t0;
    sIdleRate = (float)(sh->alive - a0) / (float)dt;
    mark_alive();
    rt_log("me: idle loop %.2f per us", (double)sIdleRate);
}

/*
 * Stops the ME before the game exits. It runs code from this module and
 * writes into its memory, and sceKernelExitGame hands both back to the XMB:
 * left running, it brings the whole PSP down (it just powers off). As in the
 * library's samples, it is asked to stop, finishes the task in hand, writes
 * back its cache and halts. Tasks submitted meanwhile run on the main CPU.
 */
void rt_me_audio_shutdown(void) {
    volatile MeShared* sh = SHARED;
    if (!sMeStarted) {
        return;
    }
    sMeReady = false;
    sh->quit = 1;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    while (sh->quit != 2 && sceKernelGetSystemTimeLow() - t0 < 500000) {
        sceKernelDelayThread(1000);
    }
    if (sh->quit == 2) {
        rt_log("me: stopped after %u us", (unsigned)(sceKernelGetSystemTimeLow() - t0));
    } else {
        rt_log("me: did not stop within 500 ms");
    }
    sMeStarted = false;
}

void rt_me_audio_submit(const AspTask* task) {
    volatile MeShared* sh = SHARED;
    wait_for_me();
    if (!sMeReady) {
        return;
    }
    /* The ME reads RDRAM from memory, so our writes have to be there. */
    sceKernelDcacheWritebackAll();
    sh->rdram = (u32)g_rdram;
    sh->task = *task;
    sSubmitTime = sceKernelGetSystemTimeLow();
    sh->seq = sh->seq + 1;
    sTasks++;
}
