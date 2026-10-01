/*
 * sched.c -- N64 threads and message queues on PSP kernel threads.
 *
 * Model (same as N64ModernRuntime's ultramodern): every N64 thread is a PSP
 * thread, but only one of them runs game code at a time. The running thread
 * holds the "baton"; handing over means signalling the next thread's
 * semaphore and waiting on our own. That keeps libultra's single-CPU
 * priority semantics exact and removes any need for locking around game
 * state.
 *
 * Hardware events (VI retrace, timers, RSP completion, ...) are raised by
 * helper PSP threads. They are queued here and delivered into the game's
 * message queues by whichever game thread next enters the scheduler.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rt.h"

#define MAX_THREADS 32
#define THREAD_STACK_SIZE (128 * 1024)

/* OSThread field offsets */
#define OSTHREAD_NEXT 0x00
#define OSTHREAD_PRIORITY 0x04
#define OSTHREAD_QUEUE 0x08
#define OSTHREAD_TLNEXT 0x0C
#define OSTHREAD_STATE 0x10
#define OSTHREAD_FLAGS 0x12
#define OSTHREAD_ID 0x14

#define OS_STATE_STOPPED 1
#define OS_STATE_RUNNABLE 2
#define OS_STATE_RUNNING 4
#define OS_STATE_WAITING 8

/* OSMesgQueue field offsets */
#define MQ_MTQUEUE 0x00
#define MQ_FULLQUEUE 0x04
#define MQ_VALIDCOUNT 0x08
#define MQ_FIRST 0x0C
#define MQ_MSGCOUNT 0x10
#define MQ_MSG 0x14

typedef enum {
    TS_FREE = 0,
    TS_STOPPED,
    TS_RUNNABLE,
    TS_RUNNING,
    TS_WAITING,
    TS_DEAD,
} ThreadState;

typedef enum {
    WAIT_NONE = 0,
    WAIT_RECV,
    WAIT_SEND,
    WAIT_NATIVE, /* blocked in rt_sched_native_wait */
    WAIT_SLEEP,  /* asleep until its sSleepUntil (rt_sched_sleep) */
} WaitKind;

typedef struct GameThread {
    uint32_t addr;        /* the game's OSThread */
    int32_t id;
    uint32_t entry;
    uint32_t arg;
    uint32_t sp;
    bool destroyed;       /* exit as soon as it gets the baton */
    SceUID thid;          /* the PSP thread running it */
    SceUID sema;          /* signalled to hand it the baton */
    uint32_t cpu_us;    /* time spent holding the baton, since the last report */
    uint32_t idle_us;   /* time nothing could run before this thread woke up */
    uint32_t ready_at;  /* when it became runnable, to measure scheduling delay */
    uint32_t wait_max;  /* worst wait for the baton since the last report */
    uint32_t wait_sum;
    uint32_t wait_count;
    uint32_t hold_max;  /* longest unbroken stretch holding the baton */
    uint32_t hold_calls;/* indirect calls made during that stretch */
    recomp_context ctx;
} GameThread;

/*
 * What choosing a thread looks at, for every thread, on every OS call that
 * sends or takes a message. It is kept apart from the rest: a GameThread is
 * 500 bytes (the register file), so going over the 32 of them read 32 cache
 * lines, most of them missing after any stretch of game code -- 16 us on the
 * PSP, twice per call. All of these fit in a few lines.
 */
typedef struct {
    uint32_t wait_queue;  /* the OSMesgQueue it waits on */
    uint32_t order;       /* FIFO order among threads of equal priority */
    int32_t priority;
    uint8_t state;        /* ThreadState */
    uint8_t wait_kind;    /* WaitKind */
    bool idle;            /* the game's idle thread: it sits in pause_self */
} SchedSlot;

static GameThread sThreads[MAX_THREADS];
static SchedSlot sSlots[MAX_THREADS];   /* sSlots[i] goes with sThreads[i] */
static int sNumSlots = 0;               /* slots used so far: the others are TS_FREE */
static GameThread* sCurrent = NULL;
static uint32_t sOrderCounter = 0;

static inline SchedSlot* slot_of(const GameThread* t) {
    return &sSlots[t - sThreads];
}

static void deliver_external(void);

/* ---- messages from other PSP threads ------------------------------------ */

/*
 * Timers, retraces, finished DMAs and the renderer run on PSP threads of their
 * own. What they post waits in this queue until the thread holding the baton
 * delivers it (rt_process_external); whoever waits for it then becomes
 * runnable like any receiver.
 */
#define EXT_QUEUE_SIZE 256

typedef struct {
    uint32_t mq;
    uint32_t msg;
    bool jam;
    bool requeue;    /* the game's queue is full: try again later instead of dropping it */
    bool vi;         /* counts against the retrace backlog */
    uint32_t posted; /* when it was posted (us), for the retrace delivery lag */
} ExtMessage;

/* ExtMessage.mq value that wakes the game thread with index msg (rt_sched_native_wait). */
#define WAKE_THREAD_MQ 0xFFFFFFFFu
/* ... and one that delivers nothing: posting it is what counts (rt_sched_poke). */
#define POKE_MQ 0xFFFFFFFEu

static ExtMessage sExtQueue[EXT_QUEUE_SIZE];
/* Only the baton holder takes messages (moves the head); posters add them under
 * sExtLock and move the tail last, once the message is in place. */
static volatile unsigned sExtHead = 0;
static volatile unsigned sExtTail = 0;
static SceUID sExtLock = -1;
static SceUID sExtSignal = -1;

/* The message registered for each OS event (osSetEventMesg). */
typedef struct {
    uint32_t mq;
    uint32_t msg;
} EventSlot;

static EventSlot sEvents[OS_NUM_EVENTS];

/*
 * Set when something is posted, so running game code notices at its next
 * preemption point (RECOMP_PREEMPT, get_function) instead of at its next OS
 * call -- which could leave a higher-priority thread, the audio manager above
 * all, waiting a long time.
 */
volatile uint8_t g_preempt_hint = 0;

/*
 * Retraces the game has not collected yet. The audio manager makes one task
 * per retrace, so a retrace dropped because its queue was briefly full is a
 * buffer of sound that is never made -- the deficit is permanent. They are
 * delivered late instead, up to a few, which lets the game catch up; beyond
 * that the game really is behind and dropping is right.
 */
#define VI_MAX_PENDING 4
static volatile int sViPending = 0;
static uint32_t sViDropped = 0;

/* Retraces handed to the game and how long each waited for a game thread to take it. */
static uint32_t sViDelivered = 0;
static uint32_t sViLagSum = 0;
static uint32_t sViLagMax = 0;

/* False if the queue is full and the message was dropped. */
static bool push_external(const ExtMessage* m) {
    sceKernelWaitSema(sExtLock, 1, NULL);
    unsigned next = (sExtTail + 1) % EXT_QUEUE_SIZE;
    bool room = next != sExtHead;
    if (room) {
        sExtQueue[sExtTail] = *m;
        sExtTail = next;
    }
    sceKernelSignalSema(sExtLock, 1);
    g_preempt_hint = 1;
    sceKernelSignalSema(sExtSignal, 1);
    return room;
}

void rt_post_message(uint32_t mq, uint32_t msg, bool jam, bool requeue) {
    if (mq == 0) {
        return;
    }
    ExtMessage m = { mq, msg, jam, requeue, false, 0 };
    push_external(&m);
}

void rt_post_vi_message(uint32_t mq, uint32_t msg) {
    if (mq == 0) {
        return;
    }
    if (sViPending >= VI_MAX_PENDING) {
        sViDropped++;
        return;
    }
    sViPending++;
    ExtMessage m = { mq, msg, false, true, true, sceKernelGetSystemTimeLow() };
    if (!push_external(&m)) {
        sViPending--;
        sViDropped++;
    }
}

void rt_post_event(int event) {
    if (event < 0 || event >= OS_NUM_EVENTS) {
        return;
    }
    /* VI and AI messages are dropped if the queue is full, like on hardware. */
    bool requeue = event != OS_EVENT_VI && event != OS_EVENT_AI;
    rt_post_message(sEvents[event].mq, sEvents[event].msg, false, requeue);
}

void rt_sched_vi_report(char* buf, int size) {
    snprintf(buf, size, "vi %u delivered, lag avg %u max %u us", (unsigned)sViDelivered,
             (unsigned)(sViDelivered ? sViLagSum / sViDelivered : 0), (unsigned)sViLagMax);
    sViDelivered = sViLagSum = sViLagMax = 0;
}

uint32_t rt_sched_vi_dropped(void) {
    uint32_t n = sViDropped;
    sViDropped = 0;
    return n;
}

/* ---- CPU accounting (the stats line) ------------------------------------ */

uint64_t g_idle_us = 0;

static uint32_t sRunStart = 0;  /* when the current thread took the baton */
static uint32_t sSwitches = 0;
static uint32_t sHoldCalls = 0; /* g_indirect_calls then */
static uint32_t sHoldVram = 0;

/* Charges the time since the last hand-over to the thread that held the baton. */
static void charge_current(void) {
    uint32_t now = sceKernelGetSystemTimeLow();
    if (sCurrent != NULL) {
        uint32_t held = now - sRunStart;
        sCurrent->cpu_us += held;
        /*
         * A thread only gives up the baton at an OS call or a preemption
         * point. A long stretch here means the game ran a long way past
         * neither, and everything else -- the audio manager above all -- was
         * stuck behind it.
         */
        if (held > sCurrent->hold_max) {
            sCurrent->hold_max = held;
            sCurrent->hold_calls = g_indirect_calls - sHoldCalls;
        }
        if (held > 100000) {
            rt_log("sched: thread %d held the CPU for %u ms with %u indirect calls (last one to %08X)",
                   sCurrent->id, held / 1000, (unsigned)(g_indirect_calls - sHoldCalls),
                   (unsigned)sHoldVram);
        }
    }
    sHoldCalls = g_indirect_calls;
    sHoldVram = g_last_indirect_vram;
    sRunStart = now;
}

/* Waits for something to be posted, for at most timeout_us if that isn't 0:
 * with no game thread runnable, the CPU is idle. Returns how long that took (us). */
static uint32_t wait_external(uint32_t timeout_us) {
    uint64_t t0 = sceKernelGetSystemTimeWide();
    SceUInt timeout = timeout_us;
    sceKernelWaitSema(sExtSignal, 1, timeout_us != 0 ? &timeout : NULL);
    uint64_t t1 = sceKernelGetSystemTimeWide();
    g_idle_us += t1 - t0;
    sRunStart = (uint32_t)t1;
    return (uint32_t)(t1 - t0);
}

/*
 * Per thread since the last report: " t<id> <CPU share>%/w<average>,<worst
 * wait for the baton>/h<longest hold>,<indirect calls in it>", then the
 * number of switches.
 */
void rt_sched_report(char* buf, int size, uint32_t span_us) {
    int len = 0;
    charge_current();
    for (int i = 0; i < sNumSlots && len < size; i++) {
        GameThread* t = &sThreads[i];
        if (sSlots[i].state == TS_FREE || sSlots[i].state == TS_DEAD) {
            continue;
        }
        unsigned pct = span_us ? (unsigned)((uint64_t)t->cpu_us * 100 / span_us) : 0;
        unsigned idle = span_us ? (unsigned)((uint64_t)t->idle_us * 100 / span_us) : 0;
        if (pct > 0 || idle > 1) {
            unsigned avg = t->wait_count ? t->wait_sum / t->wait_count : 0;
            len += snprintf(buf + len, size - len, " t%d %u%%/w%u,%u/h%u,%u", t->id, pct, avg,
                            (unsigned)t->wait_max, (unsigned)t->hold_max, (unsigned)t->hold_calls);
        }
        t->wait_max = t->wait_sum = t->wait_count = 0;
        t->hold_max = t->hold_calls = 0;
        t->cpu_us = 0;
        t->idle_us = 0;
    }
    if (len < size) {
        snprintf(buf + len, size - len, " sw %u", (unsigned)sSwitches);
    }
    sSwitches = 0;
}

/* ---- run queue ---------------------------------------------------------- */

void rt_sched_init(void) {
    memset(sThreads, 0, sizeof(sThreads));
    memset(sSlots, 0, sizeof(sSlots));
    sExtLock = sceKernelCreateSema("rt_ext_lock", 0, 1, 1, NULL);
    sExtSignal = sceKernelCreateSema("rt_ext_signal", 0, 0, 0x7FFFFFFF, NULL);
}

bool rt_on_game_thread(void) {
    return sCurrent != NULL && sCurrent->thid == sceKernelGetThreadId();
}

static GameThread* find_thread(uint32_t addr) {
    for (int i = 0; i < sNumSlots; i++) {
        if (sSlots[i].state != TS_FREE && sThreads[i].addr == addr) {
            return &sThreads[i];
        }
    }
    return NULL;
}

static void write_thread_state(GameThread* t, uint16_t state) {
    if (t->addr != 0) {
        wr_u16(t->addr + OSTHREAD_STATE, state);
    }
}

static void make_runnable(GameThread* t) {
    SchedSlot* slot = slot_of(t);
    if (slot->state != TS_RUNNABLE) {
        t->ready_at = sceKernelGetSystemTimeLow();
    }
    slot->state = TS_RUNNABLE;
    slot->wait_kind = WAIT_NONE;
    slot->wait_queue = 0;
    slot->order = ++sOrderCounter;
    write_thread_state(t, OS_STATE_RUNNABLE);
}

/* Does a come before b: higher priority, or the same and earlier in line? */
static inline bool goes_first(const SchedSlot* a, const SchedSlot* b) {
    return a->priority > b->priority || (a->priority == b->priority && a->order < b->order);
}

static GameThread* best_runnable(void) {
    const SchedSlot* best = NULL;
    for (const SchedSlot* slot = sSlots; slot < sSlots + sNumSlots; slot++) {
        if (slot->state == TS_RUNNABLE && (best == NULL || goes_first(slot, best))) {
            best = slot;
        }
    }
    return best != NULL ? &sThreads[best - sSlots] : NULL;
}

static GameThread* best_waiter(uint32_t mq, WaitKind kind) {
    const SchedSlot* best = NULL;
    for (const SchedSlot* slot = sSlots; slot < sSlots + sNumSlots; slot++) {
        if (slot->state == TS_WAITING && slot->wait_queue == mq && slot->wait_kind == kind &&
            (best == NULL || goes_first(slot, best))) {
            best = slot;
        }
    }
    return best != NULL ? &sThreads[best - sSlots] : NULL;
}

/* ---- sleeping ----------------------------------------------------------- */

/*
 * A sleeping thread (the game's csleep) is woken by the scheduler itself:
 * whenever a game thread enters it, the sleepers whose time has come are made
 * runnable, and a thread with nothing to hand over to waits for external
 * events only until the next sleeper is due. No other PSP thread is involved.
 *
 * The game's graph thread sleeps 100 us at a time while it waits for a
 * framebuffer slot to come free, some 900 times a second. As the OSTimer and
 * the wait for its message that csleep is made of, each of those woke the
 * timer thread twice: four switches between PSP threads, 5% of the CPU, taken
 * from the renderer exactly when a frame has time to spare.
 *
 * One case needs the timer thread after all: a sleeper that outranks the
 * thread running when its time comes has to interrupt it, and running game
 * code only enters the scheduler when something is posted. poke_for_sleepers
 * arranges for that.
 */
static uint64_t sSleepUntil[MAX_THREADS]; /* when a WAIT_SLEEP thread is due (system time, us) */
static int sSleepers = 0;             /* threads asleep, as of the last wake_sleepers */

/* Makes the sleepers that are due runnable. Returns the time until the next
 * one is (us), 0 if none are left asleep. */
static uint32_t wake_sleepers(void) {
    uint64_t now = sceKernelGetSystemTimeWide();
    uint64_t first = UINT64_MAX;
    int asleep = 0;
    for (int i = 0; i < sNumSlots; i++) {
        if (sSlots[i].state != TS_WAITING || sSlots[i].wait_kind != WAIT_SLEEP) {
            continue;
        }
        if (sSleepUntil[i] <= now) {
            make_runnable(&sThreads[i]);
        } else {
            asleep++;
            if (sSleepUntil[i] < first) {
                first = sSleepUntil[i];
            }
        }
    }
    sSleepers = asleep;
    return asleep != 0 ? (uint32_t)(first - now) : 0;
}

/* `running` is about to run: if a sleeper outranks it, has something posted when the first such is due. */
static void poke_for_sleepers(const GameThread* running) {
    uint64_t first = UINT64_MAX;
    for (int i = 0; i < sNumSlots; i++) {
        if (sSlots[i].state == TS_WAITING && sSlots[i].wait_kind == WAIT_SLEEP &&
            sSlots[i].priority > slot_of(running)->priority && sSleepUntil[i] < first) {
            first = sSleepUntil[i];
        }
    }
    if (first != UINT64_MAX) {
        rt_timer_poke(first);
    }
}

void rt_sched_poke(void) {
    ExtMessage m = { POKE_MQ, 0, false, false, false, 0 };
    push_external(&m);
}

/* The host thread deletes its own semaphore: nobody else signals it once the slot is dead. */
static void thread_exit_now(SceUID sema) {
    sceKernelDeleteSema(sema);
    sceKernelExitDeleteThread(0);
}

/*
 * Wait on our own semaphore until another thread hands us the baton. The
 * semaphore is passed in rather than read from the slot, and ownership is
 * checked by host thread id: a retired slot can be cleared and reused for a
 * new thread while its old host thread is still on its way out.
 */
static void wait_for_baton(GameThread* self, SceUID sema) {
    sceKernelWaitSema(sema, 1, NULL);
    if (self->thid != sceKernelGetThreadId()) {
        thread_exit_now(sema);
    }
    if (self->destroyed) {
        slot_of(self)->state = TS_DEAD;
        thread_exit_now(sema);
    }
}

/*
 * Give up the CPU: the current thread takes on new_state, and the best
 * runnable thread (possibly the current one again) continues.
 */
static void switch_out(ThreadState new_state) {
    GameThread* self = sCurrent;
    /* Read while we still hold the baton: once it is passed on, the slot may be retired and reused. */
    SceUID my_sema = self->sema;
    charge_current();

    if (new_state == TS_RUNNABLE) {
        make_runnable(self);
    } else {
        slot_of(self)->state = new_state;
        if (new_state == TS_WAITING) {
            slot_of(self)->order = ++sOrderCounter;
            write_thread_state(self, OS_STATE_WAITING);
        } else if (new_state == TS_STOPPED) {
            write_thread_state(self, OS_STATE_STOPPED);
        }
    }

    /*
     * The game's idle thread is always runnable, and all it does is wait for
     * the next external event (pause_self). So when it is the best there is,
     * this thread waits for the event itself instead of handing over: the
     * event wakes some thread -- often this one -- and going through the idle
     * thread cost two switches between PSP threads each time, 15 us apiece,
     * some 900 times a second.
     */
    GameThread* next;
    uint32_t idle_us = 0;
    uint32_t waiting_on = slot_of(self)->wait_queue;
    for (;;) {
        deliver_external();
        uint32_t sleeper_due = sSleepers != 0 ? wake_sleepers() : 0;
        next = best_runnable();
        if (next != NULL && (!slot_of(next)->idle || next == self)) {
            break;
        }
        /* Nothing can run until an external event arrives or a sleeper is due. */
        idle_us += wait_external(sleeper_due);
    }
    if (sSleepers != 0) {
        poke_for_sleepers(next);
    }
    /* Charge the wait to whoever we were waiting for. */
    next->idle_us += idle_us;
    if (idle_us > 20000) {
        rt_log("sched: nothing to run for %u ms, then thread %d (thread %d was waiting on %08X)", idle_us / 1000,
               next->id, self->id, (unsigned)waiting_on);
    }

    if (next->ready_at != 0) {
        uint32_t waited = sceKernelGetSystemTimeLow() - next->ready_at;
        next->ready_at = 0;
        next->wait_sum += waited;
        next->wait_count++;
        if (waited > next->wait_max) {
            next->wait_max = waited;
        }
    }
    slot_of(next)->state = TS_RUNNING;
    write_thread_state(next, OS_STATE_RUNNING);
    sCurrent = next;

    if (next == self) {
        return;
    }
    sSwitches++;

    sceKernelSignalSema(next->sema, 1);

    if (new_state == TS_DEAD) {
        thread_exit_now(my_sema);
    }
    wait_for_baton(self, my_sema);
}

void rt_check_preempt(void) {
    GameThread* best = best_runnable();
    if (best != NULL && sCurrent != NULL && slot_of(best)->priority > slot_of(sCurrent)->priority) {
        switch_out(TS_RUNNABLE);
    }
}

/* ---- message queue core ------------------------------------------------- */

static bool do_send(uint32_t mq, uint32_t msg, bool jam, bool block) {
    rt_audio_note_send(mq, msg);
    for (;;) {
        int32_t valid = (int32_t)rd_w32(mq + MQ_VALIDCOUNT);
        int32_t count = (int32_t)rd_w32(mq + MQ_MSGCOUNT);
        if (valid < count) {
            int32_t first = (int32_t)rd_w32(mq + MQ_FIRST);
            uint32_t buffer = rd_w32(mq + MQ_MSG);
            if (jam) {
                first = (first + count - 1) % count;
                wr_w32(buffer + 4 * first, msg);
                wr_w32(mq + MQ_FIRST, (uint32_t)first);
            } else {
                int32_t last = (first + valid) % count;
                wr_w32(buffer + 4 * last, msg);
            }
            wr_w32(mq + MQ_VALIDCOUNT, (uint32_t)(valid + 1));

            GameThread* waiter = best_waiter(mq, WAIT_RECV);
            if (waiter != NULL) {
                make_runnable(waiter);
            }
            return true;
        }
        if (!block) {
            return false;
        }
        slot_of(sCurrent)->wait_kind = WAIT_SEND;
        slot_of(sCurrent)->wait_queue = mq;
        switch_out(TS_WAITING);
    }
}

static bool do_recv(uint32_t mq, uint32_t msg_out, bool block) {
    for (;;) {
        int32_t valid = (int32_t)rd_w32(mq + MQ_VALIDCOUNT);
        if (valid > 0) {
            int32_t first = (int32_t)rd_w32(mq + MQ_FIRST);
            int32_t count = (int32_t)rd_w32(mq + MQ_MSGCOUNT);
            uint32_t buffer = rd_w32(mq + MQ_MSG);
            if (msg_out != 0) {
                wr_w32(msg_out, rd_w32(buffer + 4 * first));
            }
            wr_w32(mq + MQ_FIRST, (uint32_t)((first + 1) % count));
            wr_w32(mq + MQ_VALIDCOUNT, (uint32_t)(valid - 1));

            GameThread* waiter = best_waiter(mq, WAIT_SEND);
            if (waiter != NULL) {
                make_runnable(waiter);
            }
            return true;
        }
        if (!block) {
            return false;
        }
        slot_of(sCurrent)->wait_kind = WAIT_RECV;
        slot_of(sCurrent)->wait_queue = mq;
        switch_out(TS_WAITING);
    }
}

bool rt_send_now(uint32_t mq, uint32_t msg, bool jam) {
    if (mq == 0) {
        return false;
    }
    return do_send(mq, msg, jam, false);
}

/*
 * Delivers the messages posted by other PSP threads. Only the baton holder may
 * call this. There is usually nothing to do, so an empty queue is seen without
 * the lock: a message posted just after the look is delivered at the next
 * scheduling point, which its post asks for (g_preempt_hint), and it has
 * signalled sExtSignal, so no wait misses it.
 */
static void deliver_external(void) {
    if (sExtHead == sExtTail) {
        return;
    }
    ExtMessage requeued[EXT_QUEUE_SIZE];
    unsigned num_requeued = 0;

    for (;;) {
        ExtMessage m;
        sceKernelWaitSema(sExtLock, 1, NULL);
        if (sExtHead == sExtTail) {
            sceKernelSignalSema(sExtLock, 1);
            break;
        }
        m = sExtQueue[sExtHead];
        sExtHead = (sExtHead + 1) % EXT_QUEUE_SIZE;
        sceKernelSignalSema(sExtLock, 1);

        if (m.mq == WAKE_THREAD_MQ) {
            GameThread* t = &sThreads[m.msg % MAX_THREADS];
            if (slot_of(t)->state == TS_WAITING && slot_of(t)->wait_kind == WAIT_NATIVE) {
                make_runnable(t);
            }
            continue;
        }
        if (m.mq == POKE_MQ) {
            continue;
        }

        if (do_send(m.mq, m.msg, m.jam, false)) {
            if (m.vi) {
                sViPending--;
                uint32_t lag = sceKernelGetSystemTimeLow() - m.posted;
                sViDelivered++;
                sViLagSum += lag;
                if (lag > sViLagMax) {
                    sViLagMax = lag;
                }
            }
        } else if (m.requeue && num_requeued < EXT_QUEUE_SIZE) {
            requeued[num_requeued++] = m;
        } else if (m.vi) {
            sViPending--;
            sViDropped++;
        }
    }

    if (num_requeued > 0) {
        sceKernelWaitSema(sExtLock, 1, NULL);
        for (unsigned i = 0; i < num_requeued; i++) {
            unsigned next = (sExtTail + 1) % EXT_QUEUE_SIZE;
            if (next == sExtHead) {
                break;
            }
            sExtQueue[sExtTail] = requeued[i];
            sExtTail = next;
        }
        sceKernelSignalSema(sExtLock, 1);
    }
}

/* What every OS call starts with: whatever happened outside the game's threads
 * since the last one -- messages posted, sleepers due -- takes effect. */
void rt_process_external(void) {
    if (sSleepers != 0) {
        wake_sleepers();
    }
    deliver_external();
}

/* Puts the current game thread to sleep for us microseconds; the others run meanwhile. */
void rt_sched_sleep(uint64_t us) {
    sSleepUntil[sCurrent - sThreads] = sceKernelGetSystemTimeWide() + us;
    slot_of(sCurrent)->wait_kind = WAIT_SLEEP;
    slot_of(sCurrent)->wait_queue = 0;
    sSleepers++;
    switch_out(TS_WAITING);
}

/* ---- blocking native work ----------------------------------------------- */

typedef struct {
    void (*fn)(void*);
    void* arg;
    uint32_t thread_index;
    SceUID signal;
} NativeJob;

/* One helper per game thread: several can be waiting on host work at once. */
static NativeJob sNativeJobs[MAX_THREADS];

static int native_thread(SceSize args, void* argp) {
    NativeJob* job = *(NativeJob**)argp;
    for (;;) {
        sceKernelWaitSema(job->signal, 1, NULL);
        job->fn(job->arg);
        rt_post_message(WAKE_THREAD_MQ, job->thread_index, false, false);
    }
    return 0;
}

/*
 * Runs blocking host work (e.g. waiting for the GE or the Media Engine) for
 * the current game thread on a helper thread, letting other game threads use
 * the CPU in the meantime. Returns once fn has finished and this thread holds
 * the baton.
 */
void rt_sched_native_wait(void (*fn)(void*), void* arg) {
    if (!rt_on_game_thread()) {
        fn(arg);
        return;
    }
    uint32_t index = (uint32_t)(sCurrent - sThreads);
    NativeJob* job = &sNativeJobs[index];
    if (job->signal <= 0) {
        job->signal = sceKernelCreateSema("rt_native", 0, 0, 1, NULL);
        job->thread_index = index;
        SceUID thid = sceKernelCreateThread("rt_native", native_thread, 0x1C, 8 * 1024, PSP_THREAD_ATTR_USER, NULL);
        sceKernelStartThread(thid, sizeof(job), &job);
    }
    job->fn = fn;
    job->arg = arg;
    slot_of(sCurrent)->wait_kind = WAIT_NATIVE;
    slot_of(sCurrent)->wait_queue = 0;
    sceKernelSignalSema(job->signal, 1);
    switch_out(TS_WAITING);
}

/* ---- game thread bodies ------------------------------------------------- */

/* A fresh register file: stack pointer sp, first argument arg. */
static void init_context(recomp_context* ctx, uint32_t sp, uint32_t arg) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->r4 = arg;
    ctx->r29 = sp;
    ctx->status_reg = 0xFF03;
    /* IDO -mips2 code uses the 32-bit FPU register model: odd registers hold the
       upper half of the preceding even register's double (e.g. mtc1 $f9 / $f8). */
    ctx->f_odd = &ctx->f0.u32h;
    ctx->mips3_float_mode = 0;
}

typedef struct {
    GameThread* self;
    SceUID sema;
} ThreadStart;

static int game_thread_main(SceSize args, void* argp) {
    ThreadStart start = *(ThreadStart*)argp;
    GameThread* self = start.self;

    wait_for_baton(self, start.sema);
    init_context(&self->ctx, self->sp, self->arg);
    rt_log("thread %d (%08X) starting at %08X, pri %d", self->id, self->addr, self->entry,
           (int)slot_of(self)->priority);
    recomp_func_t* func = get_function((int32_t)self->entry);
    func(g_rdram, &self->ctx);

    rt_log("thread %d (%08X) returned", self->id, self->addr);
    switch_out(TS_DEAD);
    return 0;
}

static GameThread* alloc_thread(void) {
    /* Prefer never-used slots so a dead thread's host thread has time to exit. */
    for (int pass = 0; pass < 2; pass++) {
        ThreadState wanted = pass == 0 ? TS_FREE : TS_DEAD;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (sSlots[i].state == wanted && &sThreads[i] != sCurrent) {
                memset(&sThreads[i], 0, sizeof(sThreads[i]));
                memset(&sSlots[i], 0, sizeof(sSlots[i]));
                if (i >= sNumSlots) {
                    sNumSlots = i + 1;
                }
                return &sThreads[i];
            }
        }
    }
    rt_fatal("out of game thread slots");
}

/* ---- libultra: threads -------------------------------------------------- */

void osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4;
    int32_t id = (int32_t)ctx->r5;
    uint32_t entry = ctx->r6;
    uint32_t arg = ctx->r7;
    uint32_t sp = rt_stack_arg(ctx, 4);
    int32_t pri = (int32_t)rt_stack_arg(ctx, 5);

    /*
     * Re-creating a thread on the same OSThread (AF's flash thread does this for
     * every 16 KB of a save, after osDestroyThread on the finished one): detach
     * the old slot and always start from a cleared one. Reusing the old slot in
     * place kept its `destroyed` flag, so the new thread exited as soon as it got
     * the baton and every game thread waited forever.
     */
    GameThread* existing = find_thread(addr);
    if (existing != NULL) {
        existing->addr = 0;
        if (slot_of(existing)->state != TS_DEAD) {
            existing->destroyed = true;
            if (existing != sCurrent) {
                slot_of(existing)->state = TS_DEAD;
                existing->thid = -1;
                sceKernelSignalSema(existing->sema, 1);
            }
        }
    }

    GameThread* t = alloc_thread();
    t->addr = addr;
    t->id = id;
    t->entry = entry;
    t->arg = arg;
    t->sp = sp - 16;
    slot_of(t)->priority = pri;
    slot_of(t)->state = TS_STOPPED;
    t->sema = sceKernelCreateSema("rt_thread", 0, 0, 1, NULL);

    wr_w32(addr + OSTHREAD_NEXT, 0);
    wr_w32(addr + OSTHREAD_PRIORITY, (uint32_t)pri);
    wr_w32(addr + OSTHREAD_QUEUE, 0);
    wr_w32(addr + OSTHREAD_TLNEXT, 0);
    wr_u16(addr + OSTHREAD_STATE, OS_STATE_STOPPED);
    wr_u16(addr + OSTHREAD_FLAGS, 0);
    wr_w32(addr + OSTHREAD_ID, (uint32_t)id);

    char name[32];
    snprintf(name, sizeof(name), "n64_thread_%d", id);
    /* No PSP_THREAD_ATTR_VFPU: only the renderer uses the VFPU (on its own thread), and the
     * kernel saves and restores all of its registers on every switch to or from a thread that
     * may -- 3.5 us of each of the 15 us a switch between two such threads took on the PSP. */
    t->thid = sceKernelCreateThread(name, game_thread_main, RT_GAME_THREAD_PRIORITY, THREAD_STACK_SIZE,
                                    PSP_THREAD_ATTR_USER, NULL);
    if (t->thid < 0) {
        rt_fatal("sceKernelCreateThread failed: %08X", (unsigned)t->thid);
    }
    ThreadStart start = { t, t->sema };
    sceKernelStartThread(t->thid, sizeof(start), &start);

    rt_log("osCreateThread id=%d addr=%08X entry=%08X pri=%d", id, addr, entry, pri);
}

void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = find_thread(ctx->r4);
    if (t == NULL) {
        rt_log("osStartThread: unknown thread %08X", ctx->r4);
        return;
    }

    if (sCurrent == NULL) {
        /* Called from the boot context, which never resumes on hardware. */
        slot_of(t)->state = TS_RUNNING;
        write_thread_state(t, OS_STATE_RUNNING);
        sCurrent = t;
        sRunStart = sceKernelGetSystemTimeLow();
        sceKernelSignalSema(t->sema, 1);
        rt_log("boot context handed over to thread %d", t->id);
        sceKernelExitDeleteThread(0);
        return;
    }

    rt_process_external();
    if (slot_of(t)->state == TS_STOPPED) {
        make_runnable(t);
    }
    rt_check_preempt();
}

void osStopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = ctx->r4 == 0 ? sCurrent : find_thread(ctx->r4);
    if (t == NULL) {
        return;
    }
    if (t == sCurrent) {
        switch_out(TS_STOPPED);
    } else if (slot_of(t)->state == TS_RUNNABLE || slot_of(t)->state == TS_WAITING) {
        slot_of(t)->state = TS_STOPPED;
        slot_of(t)->wait_kind = WAIT_NONE;
        slot_of(t)->wait_queue = 0;
        write_thread_state(t, OS_STATE_STOPPED);
    }
}

void osDestroyThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = ctx->r4 == 0 ? sCurrent : find_thread(ctx->r4);
    if (t == NULL) {
        return;
    }
    if (t == sCurrent) {
        t->destroyed = true;
        switch_out(TS_DEAD);
        return;
    }
    if (slot_of(t)->state == TS_DEAD) {
        /* Already returned (AF destroys its finished flash thread): its host thread
           is gone and has deleted its semaphore. */
        return;
    }
    t->destroyed = true;
    slot_of(t)->state = TS_DEAD;
    t->thid = -1;
    sceKernelSignalSema(t->sema, 1);
}

void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_process_external();
    switch_out(TS_RUNNABLE);
}

void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = ctx->r4 == 0 ? sCurrent : find_thread(ctx->r4);
    if (t == NULL) {
        return;
    }
    slot_of(t)->priority = (int32_t)ctx->r5;
    if (t->addr != 0) {
        wr_w32(t->addr + OSTHREAD_PRIORITY, ctx->r5);
    }
    rt_check_preempt();
}

void osGetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = ctx->r4 == 0 ? sCurrent : find_thread(ctx->r4);
    ctx->r2 = t != NULL ? (gpr)slot_of(t)->priority : 0;
}

void osGetThreadId_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = ctx->r4 == 0 ? sCurrent : find_thread(ctx->r4);
    ctx->r2 = t != NULL ? (gpr)t->id : 0;
}

void __osCleanupThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    sCurrent->destroyed = true;
    switch_out(TS_DEAD);
}

/* The game's idle loop (the recompiler turns its `for (;;);` into this). */
void pause_self(uint8_t* rdram) {
    slot_of(sCurrent)->idle = true;
    for (;;) {
        wait_external(sSleepers != 0 ? wake_sleepers() : 0);
        rt_process_external();
        rt_check_preempt();
    }
}

/* ---- libultra: messages ------------------------------------------------- */

void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mq = ctx->r4;
    wr_w32(mq + MQ_MTQUEUE, 0);
    wr_w32(mq + MQ_FULLQUEUE, 0);
    wr_w32(mq + MQ_VALIDCOUNT, 0);
    wr_w32(mq + MQ_FIRST, 0);
    wr_w32(mq + MQ_MSGCOUNT, ctx->r6);
    wr_w32(mq + MQ_MSG, ctx->r5);
}

#define OS_MESG_BLOCK 1

void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_process_external();
    bool sent = do_send(ctx->r4, ctx->r5, false, ctx->r6 == OS_MESG_BLOCK);
    rt_check_preempt();
    ctx->r2 = sent ? 0 : (gpr)-1;
}

void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_process_external();
    bool sent = do_send(ctx->r4, ctx->r5, true, ctx->r6 == OS_MESG_BLOCK);
    rt_check_preempt();
    ctx->r2 = sent ? 0 : (gpr)-1;
}

void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_process_external();
    bool received = do_recv(ctx->r4, ctx->r5, ctx->r6 == OS_MESG_BLOCK);
    rt_check_preempt();
    ctx->r2 = received ? 0 : (gpr)-1;
}

void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t event = ctx->r4;
    if (event < OS_NUM_EVENTS) {
        sEvents[event].mq = ctx->r5;
        sEvents[event].msg = ctx->r6;
    }
}

/* ---- boot --------------------------------------------------------------- */

void rt_sched_run_boot(uint32_t entry_vram, uint32_t sp) {
    static recomp_context boot_ctx;
    init_context(&boot_ctx, sp, 0);
    rt_log("running boot code at %08X, sp %08X", entry_vram, sp);
    get_function((int32_t)entry_vram)(g_rdram, &boot_ctx);
    rt_fatal("boot code returned without starting a thread");
}
