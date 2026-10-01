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
 * Needs a custom firmware that lets homebrew load the library's small kernel
 * PRX (kcall.prx, next to the EBOOT). Without a working ME (or with no_me.txt
 * next to the EBOOT; PPSSPP doesn't emulate it) tasks run on the main CPU.
 */
#include <stdio.h>

#include <pspkernel.h>
#include <pspthreadman.h>
#include <psppower.h>

#include <me-core-mapper/me-core.h>

#include "me_audio.h"
#include "rt.h"

#define ME_TIMEOUT_US 200000

typedef struct {
    u32 bench;      /* rounds of the speed test, then 0 when done */
    u32 bench_sum;
    u32 pad[14];    /* the ME writes these: keep them off the CPU's lines */
    u32 seq;        /* bumped by the main CPU to submit a task */
    u32 ack;        /* set to seq by the ME when the task is done */
    u32 alive;      /* incremented by the ME while idle */
    u32 rdram;      /* RDRAM base for the ME */
    u32 out;        /* sample buffer of the last task the ME finished */
    u32 quit;       /* 1 = main CPU asks the ME to stop, 2 = ME halted */
    AspTask task;
} MeShared;

static MeShared sShared __attribute__((aligned(64)));
#define SHARED ((volatile MeShared*)(UNCACHED_USER_MASK | (u32)&sShared))

/* Called through kcall: checks that kernel calls really work. */
static int probe_kernel(void) {
    return 0x1234;
}

/*
 * Starts the ME core, in kernel mode. This is meLibDefaultInit() without its
 * sleep/wake hook (install_sleep_hook below does that part, once the ME is
 * known to be there).
 */
static int start_me_core(void) {
    int table = meCoreGetTableIdFromWitnessWord();
    if (table < ME_CORE_T2_IMG_TABLE) {
        return ERROR_ON_ME_IMG;
    }
    meCoreSelectSystemTable(table);
    meLibReset();
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

/* A fixed amount of integer work, to compare the two cores. */
static u32 bench_work(u32 rounds) {
    volatile u32* buf = (volatile u32*)g_asp.dm;
    u32 sum = 0;
    for (u32 r = 0; r < rounds; r++) {
        for (u32 i = 0; i < 1024; i++) {
            sum = sum * 1103515245u + buf[i & 0x3FF] + r;
        }
    }
    return sum;
}

void meLibOnProcess(void) {
    volatile MeShared* sh = SHARED;
    u32 done = sh->ack;
    for (;;) {
        if (sh->bench != 0) {
            u32 rounds = sh->bench;
            meLibDcacheWritebackInvalidateAll();
            sh->bench_sum = bench_work(rounds);
            meLibDcacheWritebackInvalidateAll();
            sh->bench = 0;
            meLibSync();
        }
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
        } else {
            sh->alive++;
            meLibDelayPipeline();
        }
    }
}

/* How fast is the ME, compared with the main CPU? */
static void speed_test(const char* when) {
    volatile MeShared* sh = SHARED;
    const u32 rounds = 400;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    sh->bench = rounds;
    while (sh->bench != 0 && sceKernelGetSystemTimeLow() - t0 < 3000000) {
        sceKernelDelayThread(200);
    }
    uint32_t me_us = sceKernelGetSystemTimeLow() - t0;
    t0 = sceKernelGetSystemTimeLow();
    u32 cpu_sum = bench_work(rounds);
    uint32_t cpu_us = sceKernelGetSystemTimeLow() - t0;
    rt_log("me: speed test%s %u us on the ME, %u us on the main CPU (sums %08X/%08X), clocks %d/%d MHz", when,
           (unsigned)me_us, (unsigned)cpu_us, (unsigned)sh->bench_sum, (unsigned)cpu_sum,
           scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
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
static u32 sSleepAck = 0;   /* quit state the ME reached at the last suspend */

static int me_sysevent(int ev_id, char* ev_name, void* param, int* result) {
    volatile MeShared* sh = SHARED;
    if (ev_id == SYSEVENT_SUSPEND && sMeStarted && !sMeAsleep) {
        sMeReady = false;
        sh->quit = 1;
        for (u32 i = 0; i < 2000000 && sh->quit != 2; i++) {
        }
        sSleepAck = sh->quit;
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
    rt_log("me: %s before the suspend", sSleepAck == 2 ? "halted" : "did not halt");
    sh->quit = 0;
    u32 alive = sh->alive;
    int table = kcall(start_me_core, 0);
    sMeAsleep = false;
    for (int i = 0; i < 20 && sh->alive == alive; i++) {
        sceKernelDelayThread(10000);
    }
    if (table < 0 || sh->alive == alive) {
        rt_log("me: did not restart after the resume (%d), audio stays on the main CPU", table);
        return;
    }
    speed_test(" after the resume:");
    sSubmitTime = sceKernelGetSystemTimeLow();
    sAliveMark = sh->alive;
    sAliveTime = sceKernelGetSystemTimeLow();
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
    for (int i = 0; i < 20 && sh->alive == 0; i++) {
        sceKernelDelayThread(10000);
    }
    if (sh->alive == 0) {
        rt_log("me: no heartbeat, audio runs on the main CPU");
        return;
    }
    sMeReady = true;
    rt_log("me: audio tasks run on the Media Engine (core image %d)", table);

    speed_test("");

    /* How fast the idle loop counts when the ME has nothing to do: the
     * baseline for how busy it is later (rt_me_audio_report). */
    u32 a0 = sh->alive;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    sceKernelDelayThread(100000);
    u32 dt = sceKernelGetSystemTimeLow() - t0;
    sIdleRate = (float)(sh->alive - a0) / (float)dt;
    sAliveMark = sh->alive;
    sAliveTime = sceKernelGetSystemTimeLow();
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
