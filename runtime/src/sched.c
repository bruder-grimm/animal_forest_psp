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
 *
 * Game threads run on stacks of the runtime's own (sGameStacks), at the same
 * addresses in every run of a build, and each saves its context (RtCtx) when
 * it starts waiting for the baton. Those stacks and contexts are what lets a
 * capture be resumed (capture.c): new PSP threads jump back into them.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rt.h"

#define MAX_THREADS 32

/*
 * The game threads' stacks. A PSP thread starts on a small stack of the
 * kernel's (HOST_STACK_SIZE) and moves to one of these. Animal Forest has
 * about ten threads at a time (its flash thread comes and goes for every
 * 16 KB of a save); none was seen to use more than 5 KB (captures log it:
 * rt_sched_log_stacks), as recompiled code keeps the game's own stack in RDRAM.
 *
 * Where they are matters on hardware: at a thread switch the firmware kills
 * a thread whose stack pointer is below the stack it gave the thread (it
 * takes that for an overflow; PPSSPP doesn't check). So the game stacks are
 * one block reserved straight after the module at startup, at the same address
 * in every run of a build, and the game threads' own stacks are allocated from
 * the bottom of memory (PSP_THREAD_ATTR_LOW_MEM_STACK), into a hole left for
 * them just below it (reserve_stacks).
 */
#define NUM_GAME_STACKS 16
#define GAME_STACK_SIZE (64 * 1024)
#define HOST_STACK_SIZE (8 * 1024)
#define HOST_STACK_ROOM ((NUM_GAME_STACKS + 8) * HOST_STACK_SIZE)
#define STACK_FILL 0xA5A5A5A5u /* what an unused stack holds: shows how much was used */
/* (not in this PSPSDK's pspthreadman.h) the thread's stack from the bottom of its partition */
#define PSP_THREAD_ATTR_LOW_MEM_STACK 0x00400000

/* At the top of each game stack: where the PSP thread on it left its own stack. */
typedef struct {
    RtCtx* exit;     /* rt_ctx_jump there to leave the game stack (thread_exit_now) */
    uint32_t pad[3];
} StackTop;

static uint32_t (*sGameStacks)[GAME_STACK_SIZE / 4];
static SceUID sGameStacksBlock = -1;
/* Taken by osCreateThread, given back by the PSP thread once it has left the stack. */
static volatile bool sStackBusy[NUM_GAME_STACKS];

static void reserve_stacks(void) {
    static bool reserved = false;
    if (reserved) {
        return;
    }
    reserved = true;
    SceUID room = sceKernelAllocPartitionMemory(2, "rt_kstack_room", PSP_SMEM_Low, HOST_STACK_ROOM, NULL);
    sGameStacksBlock = sceKernelAllocPartitionMemory(2, "rt_game_stacks", PSP_SMEM_Low,
                                                     NUM_GAME_STACKS * GAME_STACK_SIZE, NULL);
    if (sGameStacksBlock >= 0) {
        sGameStacks = sceKernelGetBlockHeadAddr(sGameStacksBlock);
    }
    if (room >= 0) {
        sceKernelFreePartitionMemory(room);
    }
}

void* rt_sched_stacks_addr(void) {
    return sGameStacks;
}

/*
 * The C library takes nearly all free memory for its heap at the first
 * malloc, which comes before main (Makefile: -Wl,--wrap=_sbrk). The game
 * stacks are reserved first, so they come straight after the module.
 */
void* __real__sbrk(ptrdiff_t incr);
void* __wrap__sbrk(ptrdiff_t incr) {
    reserve_stacks();
    return __real__sbrk(incr);
}

static StackTop* stack_top(int index) {
    return (StackTop*)&sGameStacks[index][GAME_STACK_SIZE / 4] - 1;
}

/* Fills game stack i with STACK_FILL, so that how much of it gets used shows (rt_sched_log_stacks). */
static void fill_stack(int i) {
    for (uint32_t w = 0; w < GAME_STACK_SIZE / 4; w++) {
        sGameStacks[i][w] = STACK_FILL;
    }
}

/* The game stack the caller is on, or -1. */
static int current_stack(void) {
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t base = (uintptr_t)sGameStacks;
    if (sp < base || sp >= base + NUM_GAME_STACKS * GAME_STACK_SIZE) {
        return -1;
    }
    return (int)((sp - base) / GAME_STACK_SIZE);
}

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
    int stack;            /* its game stack (sGameStacks) */
    volatile bool parked; /* waiting for the baton, with its context in park */
    RtCtx park;
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

/* A slot with a thread that hasn't ended. */
static bool is_live(const SchedSlot* slot) {
    return slot->state != TS_FREE && slot->state != TS_DEAD;
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

static void reset_thread_stats(GameThread* t) {
    t->cpu_us = t->idle_us = 0;
    t->wait_max = t->wait_sum = t->wait_count = 0;
    t->hold_max = t->hold_calls = 0;
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
        if (!is_live(&sSlots[i])) {
            continue;
        }
        unsigned pct = span_us ? (unsigned)((uint64_t)t->cpu_us * 100 / span_us) : 0;
        unsigned idle = span_us ? (unsigned)((uint64_t)t->idle_us * 100 / span_us) : 0;
        if (pct > 0 || idle > 1) {
            unsigned avg = t->wait_count ? t->wait_sum / t->wait_count : 0;
            len += snprintf(buf + len, size - len, " t%d %u%%/w%u,%u/h%u,%u", t->id, pct, avg,
                            (unsigned)t->wait_max, (unsigned)t->hold_max, (unsigned)t->hold_calls);
        }
        reset_thread_stats(t);
    }
    if (len < size) {
        snprintf(buf + len, size - len, " sw %u", (unsigned)sSwitches);
    }
    sSwitches = 0;
}

/* ---- run queue ---------------------------------------------------------- */

void rt_sched_init(void) {
    if (sGameStacks == NULL) {
        rt_fatal("no room for the game threads' stacks (%08X)", (unsigned)sGameStacksBlock);
    }
    rt_log("game stacks at %p", (void*)sGameStacks);
    memset(sThreads, 0, sizeof(sThreads));
    memset(sSlots, 0, sizeof(sSlots));
    for (int i = 0; i < NUM_GAME_STACKS; i++) {
        fill_stack(i);
    }
    sExtLock = sceKernelCreateSema("rt_ext_lock", 0, 1, 1, NULL);
    sExtSignal = sceKernelCreateSema("rt_ext_signal", 0, 0, 0x7FFFFFFF, NULL);
}

static bool on_game_thread(void) {
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

/*
 * The host thread deletes its own semaphore: nobody else signals it once the
 * slot is dead. It leaves its game stack for its own before it exits
 * (game_thread_main), which frees the game stack for another thread.
 */
static void thread_exit_now(SceUID sema) {
    sceKernelDeleteSema(sema);
    int stack = current_stack();
    if (stack >= 0) {
        rt_ctx_jump(stack_top(stack)->exit);
    }
    sceKernelExitDeleteThread(0);
}

/* Just woken with the baton: false if this host thread is to exit instead (see wait_for_baton). */
static bool keep_baton(GameThread* self) {
    if (self->thid != sceKernelGetThreadId()) {
        return false;
    }
    self->parked = false;
    if (self->destroyed) {
        slot_of(self)->state = TS_DEAD;
        return false;
    }
    return true;
}

/*
 * Wait on our own semaphore until another thread hands us the baton. The
 * semaphore is passed in rather than read from the slot, and ownership is
 * checked by host thread id: a retired slot can be cleared and reused for a
 * new thread while its old host thread is still on its way out.
 *
 * The context saved here is where a capture's threads resume (rt_sched_resume):
 * there, a new host thread holding the baton comes out of rt_ctx_save.
 */
static void wait_for_baton(GameThread* self, SceUID sema) {
    if (sGameStacks[self->stack][0] != STACK_FILL) {
        rt_fatal("game thread %d has overflowed its stack", self->id);
    }
    if (rt_ctx_save(&self->park) != 0) {
        return;
    }
    self->parked = true;
    sceKernelWaitSema(sema, 1, NULL);
    if (!keep_baton(self)) {
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
    if (!on_game_thread()) {
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
    int stack;
    bool resume; /* a thread of a capture: jump into self->park once it has the baton */
} ThreadStart;

/* A game thread, on its game stack. */
static void game_thread_body(void* arg) {
    /* (start is on the host thread's own stack, which a capture doesn't keep: nothing of it is used after the wait) */
    const ThreadStart* start = arg;
    GameThread* self = start->self;

    wait_for_baton(self, start->sema);
    init_context(&self->ctx, self->sp, self->arg);
    rt_log("thread %d (%08X) starting at %08X, pri %d", self->id, self->addr, self->entry,
           (int)slot_of(self)->priority);
    recomp_func_t* func = get_function((int32_t)self->entry);
    func(g_rdram, &self->ctx);

    rt_log("thread %d (%08X) returned", self->id, self->addr);
    switch_out(TS_DEAD);
}

/*
 * The host thread of a game thread. It moves to the thread's game stack, or,
 * for a thread of a capture, waits for the baton here and jumps to where the
 * thread was waiting for it. It comes back here only to exit (thread_exit_now).
 */
static int game_thread_main(SceSize args, void* argp) {
    ThreadStart start = *(ThreadStart*)argp;
    RtCtx exit;
    stack_top(start.stack)->exit = &exit;
    if (rt_ctx_save(&exit) == 0) {
        if (!start.resume) {
            rt_call_on_stack(stack_top(start.stack), game_thread_body, &start);
        } else {
            sceKernelWaitSema(start.sema, 1, NULL);
            if (keep_baton(start.self)) {
                rt_ctx_jump(&start.self->park);
            }
            sceKernelDeleteSema(start.sema);
        }
    }
    sStackBusy[start.stack] = false;
    sceKernelExitDeleteThread(0);
    return 0;
}

/* Starts the host thread of game thread t. */
static void start_host_thread(GameThread* t, bool resume) {
    char name[32];
    snprintf(name, sizeof(name), "n64_thread_%d", t->id);
    /* No PSP_THREAD_ATTR_VFPU: only the renderer uses the VFPU (on its own thread), and the
     * kernel saves and restores all of its registers on every switch to or from a thread that
     * may -- 3.5 us of each of the 15 us a switch between two such threads took on the PSP. */
    t->thid = sceKernelCreateThread(name, game_thread_main, RT_GAME_THREAD_PRIORITY, HOST_STACK_SIZE,
                                    PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_LOW_MEM_STACK, NULL);
    if (t->thid < 0) {
        rt_fatal("sceKernelCreateThread failed: %08X", (unsigned)t->thid);
    }
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelReferThreadStatus(t->thid, &info) == 0 && (uintptr_t)info.stack >= (uintptr_t)sGameStacks) {
        /* The firmware would take the game stack for an overflow of this one and kill the thread. */
        rt_fatal("thread %d's own stack is at %p, not below the game stacks", t->id, info.stack);
    }
    ThreadStart start = { t, t->sema, t->stack, resume };
    sceKernelStartThread(t->thid, sizeof(start), &start);
}

static int alloc_stack(void) {
    for (int tries = 0; tries < 1000; tries++) {
        for (int i = 0; i < NUM_GAME_STACKS; i++) {
            if (!sStackBusy[i]) {
                sStackBusy[i] = true;
                fill_stack(i);
                return i;
            }
        }
        /* A host thread on its way out still has one: give it a moment. */
        sceKernelDelayThread(1000);
    }
    rt_fatal("out of game thread stacks");
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

/* The thread an OSThread* argument names: NULL is the calling one. */
static GameThread* thread_arg(uint32_t addr) {
    return addr == 0 ? sCurrent : find_thread(addr);
}

/* Ends a thread other than the calling one: its host thread exits as soon as it gets the baton. */
static void retire_thread(GameThread* t) {
    t->destroyed = true;
    slot_of(t)->state = TS_DEAD;
    t->thid = -1;
    sceKernelSignalSema(t->sema, 1);
}

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
        if (existing == sCurrent) {
            existing->destroyed = true;
        } else if (slot_of(existing)->state != TS_DEAD) {
            retire_thread(existing);
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
    t->stack = alloc_stack();

    wr_w32(addr + OSTHREAD_NEXT, 0);
    wr_w32(addr + OSTHREAD_PRIORITY, (uint32_t)pri);
    wr_w32(addr + OSTHREAD_QUEUE, 0);
    wr_w32(addr + OSTHREAD_TLNEXT, 0);
    wr_u16(addr + OSTHREAD_STATE, OS_STATE_STOPPED);
    wr_u16(addr + OSTHREAD_FLAGS, 0);
    wr_w32(addr + OSTHREAD_ID, (uint32_t)id);

    start_host_thread(t, false);

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
    GameThread* t = thread_arg(ctx->r4);
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
    GameThread* t = thread_arg(ctx->r4);
    if (t == NULL) {
        return;
    }
    if (t == sCurrent) {
        t->destroyed = true;
        switch_out(TS_DEAD);
    } else if (slot_of(t)->state != TS_DEAD) {
        /* (a thread that has returned -- AF destroys its finished flash thread -- has no host thread
           left, which has deleted its semaphore) */
        retire_thread(t);
    }
}

void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = thread_arg(ctx->r4);
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
    GameThread* t = thread_arg(ctx->r4);
    ctx->r2 = t != NULL ? (gpr)slot_of(t)->priority : 0;
}

void osGetThreadId_recomp(uint8_t* rdram, recomp_context* ctx) {
    GameThread* t = thread_arg(ctx->r4);
    ctx->r2 = t != NULL ? (gpr)t->id : 0;
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

/* s32 osSendMesg/osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) */
static void send_mesg(recomp_context* ctx, bool jam) {
    rt_process_external();
    bool sent = do_send(ctx->r4, ctx->r5, jam, ctx->r6 == OS_MESG_BLOCK);
    rt_check_preempt();
    ctx->r2 = sent ? 0 : (gpr)-1;
}

void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    send_mesg(ctx, false);
}

void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    send_mesg(ctx, true);
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

/* ---- captures (capture.c) ----------------------------------------------- */

bool rt_sched_quiet(void) {
    deliver_external();
    for (int i = 0; i < sNumSlots; i++) {
        if (!is_live(&sSlots[i]) || &sThreads[i] == sCurrent) {
            continue;
        }
        /* Host work in progress would end in a wake-up that the capture couldn't hold. */
        if (!sThreads[i].parked || (sSlots[i].state == TS_WAITING && sSlots[i].wait_kind == WAIT_NATIVE)) {
            return false;
        }
    }
    return true;
}

bool rt_sched_checkpoint(void (*write)(void*), void* arg) {
    GameThread* self = sCurrent;
    if (!on_game_thread()) {
        rt_fatal("capture taken off a game thread");
    }
    if (rt_ctx_save(&self->park) != 0) {
        return true;
    }
    self->parked = true;
    write(arg);
    self->parked = false;
    /* (the capture's own time is not the game holding the CPU) */
    sRunStart = sceKernelGetSystemTimeLow();
    return false;
}

/* The bytes of t's game stack that are in use while it waits: from its saved stack pointer up. */
static uint8_t* parked_stack(const GameThread* t, uint32_t* size) {
    if (t->stack < 0 || t->stack >= NUM_GAME_STACKS) {
        rt_fatal("capture: thread %d has no game stack", t->id);
    }
    uintptr_t base = (uintptr_t)sGameStacks[t->stack];
    uintptr_t top = base + GAME_STACK_SIZE;
    if (t->park.sp < base || t->park.sp >= top) {
        rt_fatal("capture: thread %d's stack pointer %08X is off its stack", t->id, (unsigned)t->park.sp);
    }
    *size = (uint32_t)(top - t->park.sp);
    return (uint8_t*)t->park.sp;
}

void rt_sched_capture(RtCapture* c) {
    static struct {
        int32_t num_slots;
        int32_t current;
        uint32_t order_counter;
        int64_t sleep_left[MAX_THREADS]; /* us until a sleeper is due */
        EventSlot events[OS_NUM_EVENTS];
        uint32_t num_ext;
        ExtMessage ext[EXT_QUEUE_SIZE];  /* posted and not yet delivered */
    } s;
    bool saving = rt_cap_saving(c);
    uint64_t now = sceKernelGetSystemTimeWide();

    if (saving) {
        s.num_slots = sNumSlots;
        s.current = (int32_t)(sCurrent - sThreads);
        s.order_counter = sOrderCounter;
        for (int i = 0; i < MAX_THREADS; i++) {
            s.sleep_left[i] = (int64_t)(sSleepUntil[i] - now);
        }
        memcpy(s.events, sEvents, sizeof(s.events));
        sceKernelWaitSema(sExtLock, 1, NULL);
        s.num_ext = 0;
        for (unsigned i = sExtHead; i != sExtTail; i = (i + 1) % EXT_QUEUE_SIZE) {
            s.ext[s.num_ext++] = sExtQueue[i];
        }
        sceKernelSignalSema(sExtLock, 1);
    }
    rt_cap_io(c, "THRD", sThreads, sizeof(sThreads));
    rt_cap_io(c, "SLOT", sSlots, sizeof(sSlots));
    rt_cap_io(c, "SCHD", &s, sizeof(s));
    for (int i = 0; i < MAX_THREADS; i++) {
        if (is_live(&sSlots[i])) {
            uint32_t size;
            uint8_t* stack = parked_stack(&sThreads[i], &size);
            rt_cap_io(c, "STAK", stack, size);
        }
    }
    if (saving) {
        return;
    }

    /* What belongs to the run that took the capture: host threads, semaphores, statistics. */
    for (int i = 0; i < MAX_THREADS; i++) {
        GameThread* t = &sThreads[i];
        t->thid = -1;
        t->sema = -1;
        t->ready_at = 0;
        reset_thread_stats(t);
    }
    for (int i = 0; i < NUM_GAME_STACKS; i++) {
        sStackBusy[i] = false;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        if (is_live(&sSlots[i])) {
            sStackBusy[sThreads[i].stack] = true;
        }
    }
    sNumSlots = s.num_slots;
    sCurrent = &sThreads[s.current];
    sOrderCounter = s.order_counter;
    sSleepers = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        sSleepUntil[i] = s.sleep_left[i] > 0 ? now + (uint64_t)s.sleep_left[i] : now;
        if (is_live(&sSlots[i]) && sSlots[i].state == TS_WAITING && sSlots[i].wait_kind == WAIT_SLEEP) {
            sSleepers++;
        }
    }
    memcpy(sEvents, s.events, sizeof(sEvents));
    sceKernelWaitSema(sExtLock, 1, NULL);
    sExtHead = sExtTail = 0;
    sViPending = 0;
    for (uint32_t i = 0; i < s.num_ext && i + 1 < EXT_QUEUE_SIZE; i++) {
        ExtMessage m = s.ext[i];
        if (m.vi) {
            m.posted = (uint32_t)now;
            sViPending++;
        }
        sExtQueue[sExtTail++] = m;
    }
    sceKernelSignalSema(sExtLock, 1);
}

void rt_sched_resume(void) {
    for (int i = 0; i < sNumSlots; i++) {
        GameThread* t = &sThreads[i];
        if (is_live(&sSlots[i])) {
            t->sema = sceKernelCreateSema("rt_thread", 0, 0, 1, NULL);
            start_host_thread(t, true);
        }
    }
    rt_log("resuming thread %d", sCurrent->id);
    sRunStart = sceKernelGetSystemTimeLow();
    g_preempt_hint = 1;
    sceKernelSignalSema(sExtSignal, 1);
    sceKernelSignalSema(sCurrent->sema, 1);
    sceKernelExitDeleteThread(0);
    for (;;) {
    }
}

void rt_sched_log_stacks(void) {
    char line[256];
    int len = snprintf(line, sizeof(line), "game stacks used (of %u KB):", GAME_STACK_SIZE / 1024);
    for (int i = 0; i < sNumSlots && len < (int)sizeof(line); i++) {
        if (!is_live(&sSlots[i])) {
            continue;
        }
        const uint32_t* words = sGameStacks[sThreads[i].stack];
        uint32_t unused = 0;
        while (unused < GAME_STACK_SIZE / 4 && words[unused] == STACK_FILL) {
            unused++;
        }
        len += snprintf(line + len, sizeof(line) - len, " t%d %u", sThreads[i].id,
                        (unsigned)(GAME_STACK_SIZE - unused * 4 + 1023) / 1024);
    }
    rt_log("%s; kernel free %u KB", line, (unsigned)(sceKernelTotalFreeMemSize() / 1024));
}
