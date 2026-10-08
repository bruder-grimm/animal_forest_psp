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
/* The OSTimer of each active slot, 0 if free: what osSetTimer/osStopTimer scan (two cache lines, not twenty). */
static uint32_t sTimerAddr[MAX_TIMERS];
static SceUID sTimerLock = -1;
static SceUID sTimerWake = -1;
static uint64_t sStartUs = 0;
/*
 * When the timer thread wakes up by itself (guarded by sTimerLock). The game
 * sets a timer as a timeout around most of its waits -- the audio manager
 * every retrace, the graph thread every frame -- and stops it again long
 * before it fires. Waking the timer thread for each of those, to work out
 * its next deadline, was two switches between PSP threads a time and 2% of
 * the CPU. So nothing wakes it for a timer the retrace thread will see in
 * time: every vblank rt_timer_vblank looks at the next deadline and wakes
 * the timer thread if it comes before the following vblank. Between those
 * the timer thread sleeps until a deadline that close, or until woken. If
 * the vblanks stop (a capture, standby), osSetTimer wakes it as before.
 */
static uint64_t sWakeAt = 0;   /* in the past while it is awake: it is about to look */
#define VBLANK_US 16684        /* 59.94 Hz */
#define VBLANK_SLACK_US 2000   /* how late a vblank's look may come */
#define TICK_US 16000          /* the longest sleep while the vblanks don't come */
/* When rt_timer_vblank next looks (0: it hasn't yet); a deadline before it needs the timer thread awake. */
static uint64_t sNextVblankUs = 0;
/* When the scheduler wants running game code interrupted (rt_timer_poke); 0: it doesn't. */
static uint64_t sPokeAt = 0;

/*
 * sTimerLock as a "benaphore": a count of the threads that want the timers,
 * and the semaphore only when one has to wait. osSetTimer runs about ninety
 * times a second; as a semaphore pair the lock was 20 of its 32 us -- system
 * calls into kernel code that is never in the cache -- though it is almost
 * never contended. (One CPU: the count's ll/sc is all the atomicity needed.)
 */
static int32_t sTimerLockCount = 0;

static void timer_lock(void) {
    if (__atomic_fetch_add(&sTimerLockCount, 1, __ATOMIC_ACQUIRE) > 0) {
        sceKernelWaitSema(sTimerLock, 1, NULL);
    }
}

static void timer_unlock(void) {
    if (__atomic_fetch_sub(&sTimerLockCount, 1, __ATOMIC_RELEASE) > 1) {
        sceKernelSignalSema(sTimerLock, 1);
    }
}

static uint64_t now_us(void) {
    return sceKernelGetSystemTimeWide();
}

/*
 * N64 counter ticks since boot. 46875 ticks a millisecond is 375 / 8 a
 * microsecond: a multiply and a shift, the same result as dividing by 1000
 * (the game reads the counter often, and a 64-bit division is a libgcc call).
 */
_Static_assert(COUNTS_PER_SECOND / 1000000 * 8 + (COUNTS_PER_SECOND % 1000000) * 8 / 1000000 == 375, "375/8 ticks a us");
static uint64_t os_time(void) {
    uint64_t elapsed = now_us() - sStartUs;
    return (elapsed * 375) >> 3;
}

static uint64_t counts_to_us(uint64_t counts) {
    if (counts < 0x10000000u) {
        return (uint32_t)counts * 8u / 375u; /* any timeout under 45 s: a 32-bit division */
    }
    return (counts * 8) / 375;
}

/* Whether the vblanks are coming: rt_timer_vblank looked within the last two periods. */
static bool vblanks_running(uint64_t now) {
    return sNextVblankUs != 0 && now < sNextVblankUs + VBLANK_US;
}

static int timer_thread(SceSize args, void* argp) {
    for (;;) {
        uint64_t now = now_us();
        uint64_t next_fire = UINT64_MAX;

        timer_lock();
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
                    sTimerAddr[i] = 0;
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
        if (vblanks_running(now)) {
            /* Sleep until the deadline if the next vblank's look would be too late for it, else until woken. */
            sWakeAt = next_fire < sNextVblankUs + VBLANK_SLACK_US ? next_fire : UINT64_MAX;
        } else if (next_fire == UINT64_MAX) {
            sWakeAt = UINT64_MAX; /* no timers: sleep until one is set */
        } else {
            sWakeAt = next_fire < now + TICK_US ? next_fire : now + TICK_US;
        }
        uint64_t wake_at = sWakeAt;
        timer_unlock();

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
    sTimerLock = sceKernelCreateSema("rt_timer_lock", 0, 0, 64, NULL); /* the waits of timer_lock */
    sTimerWake = sceKernelCreateSema("rt_timer_wake", 0, 0, 1, NULL);
    SceUID thid = sceKernelCreateThread("rt_timer", timer_thread, 0x12, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

/* Something is due at at_us: true if that is before the timer thread next looks (it has to be woken then).
 * The caller holds sTimerLock. */
static bool due_before_wake_locked(uint64_t at_us) {
    if (at_us >= sWakeAt) {
        return false;
    }
    sWakeAt = at_us;
    return true;
}

/* The same for a timer: one the next vblank's look will see in time needs nothing now. */
static bool timer_needs_wake_locked(uint64_t at_us, uint64_t now) {
    if (vblanks_running(now) && at_us >= sNextVblankUs + VBLANK_SLACK_US) {
        return false;
    }
    return due_before_wake_locked(at_us);
}

/* Every vblank (vi.c): wakes the timer thread for a deadline before the next vblank's look. */
void rt_timer_vblank(void) {
    uint64_t now = now_us();
    bool wake = false;
    timer_lock();
    sNextVblankUs = now + VBLANK_US;
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (sTimerAddr[i] != 0 && sTimers[i].fire_at_us < next) {
            next = sTimers[i].fire_at_us;
        }
    }
    if (next < sNextVblankUs + VBLANK_SLACK_US) {
        wake = due_before_wake_locked(next);
    }
    timer_unlock();
    if (wake) {
        sceKernelSignalSema(sTimerWake, 1);
    }
}

void rt_timer_poke(uint64_t at_us) {
    bool wake = false;
    timer_lock();
    if (sPokeAt == 0 || at_us < sPokeAt) {
        sPokeAt = at_us;
        wake = due_before_wake_locked(at_us);
    }
    timer_unlock();
    if (wake) {
        sceKernelSignalSema(sTimerWake, 1);
    }
}

/* Stops the timers of the OSTimer at addr (the caller holds sTimerLock); false if there were none. */
static bool remove_timer_locked(uint32_t addr) {
    bool found = false;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (sTimerAddr[i] == addr) {
            sTimers[i].active = false;
            sTimerAddr[i] = 0;
            found = true;
        }
    }
    return found;
}

/* s32 osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) */
void osSetTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4;
    uint64_t countdown = rt_u64(ctx->r6, ctx->r7);
    uint64_t interval = rt_u64(rt_stack_arg(ctx, 4), rt_stack_arg(ctx, 5));
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

    timer_lock();
    remove_timer_locked(addr);
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (sTimerAddr[i] == 0) {
            slot = i;
            break;
        }
    }
    bool wake = false;
    if (slot >= 0) {
        HostTimer* t = &sTimers[slot];
        uint64_t now = now_us();
        t->active = true;
        t->addr = addr;
        t->fire_at_us = now + counts_to_us(delay);
        t->interval_us = counts_to_us(interval);
        t->mq = mq;
        t->msg = msg;
        sTimerAddr[slot] = addr;
        wake = timer_needs_wake_locked(t->fire_at_us, now);
    }
    timer_unlock();
    if (wake) {
        sceKernelSignalSema(sTimerWake, 1);
    }

    if (slot < 0) {
        rt_log("osSetTimer: out of timer slots");
    }
    ctx->r2 = 0;
}

void osStopTimer_recomp(uint8_t* rdram, recomp_context* ctx) {
    timer_lock();
    bool found = remove_timer_locked(ctx->r4);
    timer_unlock();
    ctx->r2 = found ? 0 : (gpr)-1;
}

/*
 * void csleep(OSTime t): the game's own sleep (libc64), which usleep, msleep
 * and the others call. Its code sets an OSTimer and waits for the timer's
 * message; the scheduler does the same without either (rt_sched_sleep).
 */
void csleep(uint8_t* rdram, recomp_context* ctx) {
    rt_sched_sleep(counts_to_us(rt_u64(ctx->r4, ctx->r5)));
}

void osGetTime_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t t = os_time();
    ctx->r2 = (gpr)(t >> 32);
    ctx->r3 = (gpr)t;
}

void osGetCount_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)os_time();
}

/* ---- captures (capture.c) ----------------------------------------------- */

void rt_timer_hold(bool hold) {
    if (hold) {
        timer_lock();
    } else {
        timer_unlock();
    }
}

/* The timers, due as long after the resume as they were after the capture; the game's clock goes on from where it was. */
void rt_timer_capture(RtCapture* c) {
    static struct {
        HostTimer timers[MAX_TIMERS]; /* fire_at_us: us after the capture */
        uint64_t elapsed_us;          /* since the game's clock started */
    } s;
    uint64_t now = now_us();
    if (rt_cap_saving(c)) {
        /* (the caller holds the timers: rt_timer_hold) */
        memcpy(s.timers, sTimers, sizeof(s.timers));
        for (int i = 0; i < MAX_TIMERS; i++) {
            s.timers[i].fire_at_us = sTimers[i].fire_at_us > now ? sTimers[i].fire_at_us - now : 0;
        }
        s.elapsed_us = now - sStartUs;
    }
    rt_cap_io(c, "TIME", &s, sizeof(s));
    if (rt_cap_saving(c)) {
        return;
    }
    timer_lock();
    for (int i = 0; i < MAX_TIMERS; i++) {
        sTimers[i] = s.timers[i];
        sTimers[i].fire_at_us += now;
        sTimerAddr[i] = sTimers[i].active ? sTimers[i].addr : 0;
    }
    sStartUs = now - s.elapsed_us;
    sPokeAt = 0;
    sWakeAt = 0;
    timer_unlock();
    sceKernelSignalSema(sTimerWake, 1);
}
