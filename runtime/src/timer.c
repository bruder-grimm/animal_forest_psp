/*
 * timer.c -- osSetTimer/osStopTimer, osGetTime/osGetCount, and the game's sleep.
 *
 * The N64 CPU counter runs at 46.875 MHz (half the 93.75 MHz clock). A timer
 * thread fires expired OSTimers by posting their messages.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#include "rt.h"

#define COUNTS_PER_SECOND 46875000ull
#define MAX_TIMERS 32

typedef struct {
    bool active;
    uint32_t addr;
    uint64_t fire_at_us;
    uint64_t interval_us;
    uint32_t mq;
    uint32_t msg;
} HostTimer;

static HostTimer sTimers[MAX_TIMERS];
static SceUID sTimerLock = -1;
static SceUID sTimerWake = -1;
static uint64_t sStartUs = 0;
/*
 * When the timer thread wakes up by itself (guarded by sTimerLock). The game
 * sets a timer as a timeout around most of its waits -- the audio manager
 * every retrace, the graph thread every frame -- and stops it again long
 * before it fires. Waking the timer thread for each of those, to work out
 * its next deadline, was two switches between PSP threads a time and 2% of
 * the CPU. So the thread never sleeps longer than TICK_US, and is only woken
 * for a timer due before it wakes anyway.
 */
static uint64_t sWakeAt = 0;   /* in the past while it is awake: it is about to look */
#define TICK_US 16000
/* When the scheduler wants running game code interrupted (rt_timer_poke); 0: it doesn't. */
static uint64_t sPokeAt = 0;

static uint64_t now_us(void) {
    return sceKernelGetSystemTimeWide();
}

uint64_t rt_os_time(void) {
    uint64_t elapsed = now_us() - sStartUs;
    return (elapsed * (COUNTS_PER_SECOND / 1000)) / 1000;
}

static uint64_t counts_to_us(uint64_t counts) {
    return (counts * 1000) / (COUNTS_PER_SECOND / 1000);
}

static int timer_thread(SceSize args, void* argp) {
    for (;;) {
        uint64_t now = now_us();
        uint64_t next_fire = UINT64_MAX;

        sceKernelWaitSema(sTimerLock, 1, NULL);
        for (int i = 0; i < MAX_TIMERS; i++) {
            HostTimer* t = &sTimers[i];
            if (!t->active) {
                continue;
            }
            if (t->fire_at_us <= now) {
                rt_post_message(t->mq, t->msg, false, true);
                if (t->interval_us != 0) {
                    t->fire_at_us += t->interval_us;
                    if (t->fire_at_us < now) {
                        t->fire_at_us = now + t->interval_us;
                    }
                } else {
                    t->active = false;
                    continue;
                }
            }
            if (t->fire_at_us < next_fire) {
                next_fire = t->fire_at_us;
            }
        }
        if (sPokeAt != 0) {
            if (sPokeAt <= now) {
                rt_sched_poke();
                sPokeAt = 0;
            } else if (sPokeAt < next_fire) {
                next_fire = sPokeAt;
            }
        }
        now = now_us();
        if (next_fire == UINT64_MAX) {
            sWakeAt = UINT64_MAX; /* no timers: sleep until one is set */
        } else {
            sWakeAt = next_fire < now + TICK_US ? next_fire : now + TICK_US;
        }
        uint64_t wake_at = sWakeAt;
        sceKernelSignalSema(sTimerLock, 1);

        if (wake_at == UINT64_MAX) {
            sceKernelWaitSema(sTimerWake, 1, NULL);
        } else if (wake_at > now) {
            SceUInt timeout = (SceUInt)(wake_at - now);
            sceKernelWaitSema(sTimerWake, 1, &timeout);
        }
    }
    return 0;
}

void rt_timer_init(void) {
    sStartUs = now_us();
    memset(sTimers, 0, sizeof(sTimers));
    sTimerLock = sceKernelCreateSema("rt_timer_lock", 0, 1, 1, NULL);
    sTimerWake = sceKernelCreateSema("rt_timer_wake", 0, 0, 1, NULL);
    SceUID thid = sceKernelCreateThread("rt_timer", timer_thread, 0x12, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

void rt_timer_poke(uint64_t at_us) {
    bool wake = false;
    sceKernelWaitSema(sTimerLock, 1, NULL);
    if (sPokeAt == 0 || at_us < sPokeAt) {
        sPokeAt = at_us;
        if (at_us < sWakeAt) {
            sWakeAt = at_us;
            wake = true;
        }
    }
    sceKernelSignalSema(sTimerLock, 1);
    if (wake) {
        sceKernelSignalSema(sTimerWake, 1);
    }
}

/* Stops the timers of the OSTimer at addr (the caller holds sTimerLock); false if there were none. */
static bool remove_timer_locked(uint32_t addr) {
    bool found = false;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].active && sTimers[i].addr == addr) {
            sTimers[i].active = false;
            found = true;
        }
    }
    return found;
}

/* s32 osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) */
void osSetTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4;
    uint64_t countdown = ((uint64_t)ctx->r6 << 32) | ctx->r7;
    uint64_t interval = ((uint64_t)rt_stack_arg(ctx, 4) << 32) | rt_stack_arg(ctx, 5);
    uint32_t mq = rt_stack_arg(ctx, 6);
    uint32_t msg = rt_stack_arg(ctx, 7);

    /* Mirror the fields into the OSTimer struct in case the game inspects them. */
    wr_w32(addr + 0x08, (uint32_t)(interval >> 32));
    wr_w32(addr + 0x0C, (uint32_t)interval);
    wr_w32(addr + 0x10, (uint32_t)(countdown >> 32));
    wr_w32(addr + 0x14, (uint32_t)countdown);
    wr_w32(addr + 0x18, mq);
    wr_w32(addr + 0x1C, msg);

    uint64_t delay = countdown != 0 ? countdown : interval;

    sceKernelWaitSema(sTimerLock, 1, NULL);
    remove_timer_locked(addr);
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!sTimers[i].active) {
            slot = i;
            break;
        }
    }
    bool wake = false;
    if (slot >= 0) {
        HostTimer* t = &sTimers[slot];
        t->active = true;
        t->addr = addr;
        t->fire_at_us = now_us() + counts_to_us(delay);
        t->interval_us = counts_to_us(interval);
        t->mq = mq;
        t->msg = msg;
        if (t->fire_at_us < sWakeAt) {
            /* due before the timer thread next looks */
            sWakeAt = t->fire_at_us;
            wake = true;
        }
    }
    sceKernelSignalSema(sTimerLock, 1);
    if (wake) {
        sceKernelSignalSema(sTimerWake, 1);
    }

    if (slot < 0) {
        rt_log("osSetTimer: out of timer slots");
    }
    ctx->r2 = 0;
}

void osStopTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    sceKernelWaitSema(sTimerLock, 1, NULL);
    bool found = remove_timer_locked(ctx->r4);
    sceKernelSignalSema(sTimerLock, 1);
    ctx->r2 = found ? 0 : (gpr)-1;
}

/*
 * void csleep(OSTime t): the game's own sleep (libc64), which usleep, msleep
 * and the others call. Its code sets an OSTimer and waits for the timer's
 * message; the scheduler does the same without either (rt_sched_sleep).
 */
void csleep(uint8_t* rdram, recomp_context* ctx) {
    uint64_t counts = ((uint64_t)ctx->r4 << 32) | ctx->r5;
    rt_sched_sleep(counts_to_us(counts));
}

void osGetTime_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t t = rt_os_time();
    ctx->r2 = (gpr)(t >> 32);
    ctx->r3 = (gpr)t;
}

void osGetCount_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)rt_os_time();
}
