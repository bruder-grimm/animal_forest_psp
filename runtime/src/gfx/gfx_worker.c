/*
 * gfx_worker.c -- the renderer thread.
 *
 * Graphics tasks (osSpTaskStartGo) and presents (osViSwapBuffer) are queued
 * for a PSP thread of their own, which runs them in order and posts the SP and
 * DP events when a task is done, as the RSP and RDP would.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#include "gfx_internal.h"

/*
 * On the N64 the RSP and RDP chew through a display list while the CPU runs on,
 * so the game's scheduler hands over a graphics task and goes back to its
 * queue -- where the next audio task is waiting. Running the display list on
 * the game's own thread instead made a heavy frame delay audio by its whole
 * duration, which is what audio underruns sound like. A worker thread restores
 * the hardware's arrangement: it runs at the same priority as the game threads,
 * so the two share the CPU, and it reports SP and DP done when the frame is
 * built. Anything else that touches the GE (presenting, the texture cache)
 * waits for it first, and so does the next task.
 */
static SceUID sGfxSignal = -1;   /* counts commands waiting for the worker */
static SceUID sGfxFree = -1;     /* counts free slots in the command queue */
static SceUID sGfxIdle = -1;     /* held while the worker has a task */

/*
 * The worker takes commands in order: render a task, or present a frame.
 * Presents come from the game's scheduler thread (osViSwapBuffer), which also
 * dispatches audio. Queued behind the task, a present shows exactly what it
 * would have shown, but the scheduler doesn't sit out the render: waiting
 * there cost 11-17 ms of every village frame, and AF's audio manager discards
 * the retraces that pile up meanwhile (audioMgr.c) -- 23 kHz of sound instead
 * of 32 kHz.
 */
typedef enum { GFX_CMD_TASK, GFX_CMD_PRESENT } GfxCmdKind;
typedef struct {
    GfxCmdKind kind;
    uint32_t arg;
} GfxCmd;
#define GFX_QUEUE_SIZE 4
static GfxCmd sGfxQueue[GFX_QUEUE_SIZE];
static uint32_t sGfxQueueHead = 0, sGfxQueueTail = 0;

/*
 * Yielding the graphics task (see rt_gfx_yield below).
 *
 * The N64's scheduler preempts a running graphics task every retrace that has
 * an audio task due: __scHandleRetrace calls __scYield, the RSP stops and
 * reports done, the audio task runs, and the graphics task is re-dispatched and
 * resumes. Our renderer is a worker thread and our audio runs on the Media
 * Engine, so nothing actually has to stop -- but the game will not dispatch
 * audio until it believes the RSP is free, so the report has to happen.
 *
 * The contract with libultra is: exactly one SP event per osSpTaskStartGo, and
 * exactly one DP event when the task really finishes. These flags track what
 * the current dispatch still owes. sGfxLock guards them because the worker
 * thread and the game's scheduler thread both change them.
 */
static SceUID sGfxLock = -1;
static uint32_t sGfxTaskAddr = 0; /* the task the worker holds, 0 if none */
static bool sGfxRendering = false;
static bool sGfxSpOwed = false;   /* this dispatch still owes an SP event */
static bool sGfxDpOwed = false;   /* the task still owes its DP event */
static bool sGfxYieldEnabled = true;

static void worker_lock(void) {
    if (sGfxLock >= 0) {
        sceKernelWaitSema(sGfxLock, 1, NULL);
    }
}

static void worker_unlock(void) {
    if (sGfxLock >= 0) {
        sceKernelSignalSema(sGfxLock, 1);
    }
}

/* Posts whatever events this task still owes. Safe to call more than once. */
static void post_owed_events(void) {
    worker_lock();
    bool sp = sGfxSpOwed;
    bool dp = sGfxDpOwed && !sGfxRendering;
    sGfxSpOwed = false;
    if (dp) {
        sGfxDpOwed = false;
    }
    worker_unlock();
    if (sp) {
        rt_post_event(OS_EVENT_SP);
    }
    if (dp) {
        rt_post_event(OS_EVENT_DP);
    }
}

static uint64_t worker_run_clock(void) {
    SceKernelThreadRunStatus st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(0, &st) < 0) {
        return 0;
    }
    return ((uint64_t)st.runClocks.hi << 32) | st.runClocks.low;
}

static int worker_thread(SceSize args, void* argp) {
    for (;;) {
        sceKernelWaitSema(sGfxSignal, 1, NULL);
        worker_lock();
        GfxCmd cmd = sGfxQueue[sGfxQueueHead];
        sGfxQueueHead = (sGfxQueueHead + 1) % GFX_QUEUE_SIZE;
        worker_unlock();
        sceKernelSignalSema(sGfxFree, 1);
        if (cmd.kind == GFX_CMD_PRESENT) {
            gfx_present_frame(cmd.arg);
            continue;
        }
        uint64_t c0 = worker_run_clock();
        gfx_run_task(cmd.arg);
        gStats.render_cpu_us += worker_run_clock() - c0;
        worker_lock();
        sGfxRendering = false;
        worker_unlock();
        sceKernelSignalSema(sGfxIdle, 1);
        /*
         * If a yield already reported this dispatch's SP, the game owes us a
         * re-dispatch; it collects the completion there (rt_gfx_submit_task).
         */
        post_owed_events();
    }
    return 0;
}

/*
 * osSpTaskYield: the game wants the RSP back so it can run an audio task.
 * Report the dispatch finished and let the worker carry on; the game will
 * re-dispatch the same task, which rt_gfx_submit_task treats as a resume.
 */
void rt_gfx_yield(void) {
    worker_lock();
    bool honour = sGfxYieldEnabled && sGfxRendering && sGfxSpOwed;
    if (honour) {
        sGfxSpOwed = false;
        gStats.yields++;
    }
    worker_unlock();
    if (honour) {
        rt_post_event(OS_EVENT_SP);
    }
}

/* osSpTaskYielded: true while the frame we reported as yielded is still being
 * built, which tells the scheduler to re-queue it rather than complete it. */
bool rt_gfx_yielded(void) {
    worker_lock();
    bool rendering = sGfxRendering;
    worker_unlock();
    if (!rendering) {
        /* It finished between the yield and this question: the scheduler is
         * about to treat the task as complete, so settle what is still owed. */
        post_owed_events();
    }
    return rendering;
}

uint32_t rt_gfx_frame_count(void) {
    return gStats.frames;
}

static void start_worker(void) {
    if (sGfxIdle >= 0) {
        return;
    }
    sGfxIdle = sceKernelCreateSema("rt_gfx_idle", 0, 1, 1, NULL);
    sGfxSignal = sceKernelCreateSema("rt_gfx", 0, 0, GFX_QUEUE_SIZE, NULL);
    sGfxFree = sceKernelCreateSema("rt_gfx_free", 0, GFX_QUEUE_SIZE, GFX_QUEUE_SIZE, NULL);
    sGfxLock = sceKernelCreateSema("rt_gfx_lock", 0, 1, 1, NULL);
    if (rt_data_file_exists("no_yield.txt")) {
        sGfxYieldEnabled = false;
        rt_log("gfx: task yield disabled by no_yield.txt");
    }
    /*
     * One step below the game threads: when there is not enough CPU for both,
     * the game keeps running -- including the audio manager, which has to make
     * its task every retrace -- and the renderer takes what is left. A late
     * frame is worth much less than a gap in the sound.
     */
    SceUID thid = sceKernelCreateThread("rt_gfx", worker_thread, RT_GAME_THREAD_PRIORITY + 1, 32 * 1024,
                                        PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

static void take_idle(void* arg) {
    sceKernelWaitSema(sGfxIdle, 1, NULL);
}

static void take_free_slot(void* arg) {
    sceKernelWaitSema(sGfxFree, 1, NULL);
}

/* Queues a command for the worker; waits (letting the game run) only if the
 * queue is full, i.e. the worker is several presents behind. */
static void enqueue(GfxCmdKind kind, uint32_t arg) {
    if (sceKernelPollSema(sGfxFree, 1) < 0) {
        rt_sched_native_wait(take_free_slot, NULL);
    }
    worker_lock();
    sGfxQueue[sGfxQueueTail] = (GfxCmd){ kind, arg };
    sGfxQueueTail = (sGfxQueueTail + 1) % GFX_QUEUE_SIZE;
    worker_unlock();
    sceKernelSignalSema(sGfxSignal, 1);
}

/*
 * Hands a graphics task to the worker and returns; SP and DP follow when the
 * frame is built.
 *
 * Reporting them at submit instead -- so the game's scheduler would see the RSP
 * free and dispatch audio without waiting for the frame -- was tried and was
 * much worse: the game ran ahead and then blocked in rt_gfx_present for the
 * whole of the render instead (blocked went from 16-19% to 83-98%), and
 * underruns over the scripted run went from 211 to 4350. The wait belongs where
 * the game expects it.
 */
void rt_gfx_submit_task(uint32_t task) {
    start_worker();
    /*
     * A re-dispatch of the task we already hold is the resume half of a yield:
     * the frame is still being built (or has just finished), so take on a fresh
     * SP for this dispatch and return without waiting.
     */
    worker_lock();
    bool resume = (task == sGfxTaskAddr) && (sGfxRendering || sGfxDpOwed);
    bool finished = resume && !sGfxRendering;
    if (resume) {
        sGfxSpOwed = false;
    }
    worker_unlock();
    if (resume) {
        /*
         * Report this dispatch done straight away and keep building the frame.
         * The task still owes its DP, and the game's graph thread waits for the
         * whole task, so it cannot run ahead -- but the scheduler now sees a
         * free RSP for the rest of the frame and dispatches audio at every
         * retrace without having to ask for another yield.
         */
        rt_post_event(OS_EVENT_SP);
        if (finished) {
            post_owed_events();
        }
        return;
    }

    uint64_t t0 = sceKernelGetSystemTimeWide();
    rt_sched_native_wait(take_idle, NULL);
    uint32_t d = (uint32_t)(sceKernelGetSystemTimeWide() - t0);
    gStats.blocked_us += d;
    gStats.submit_wait_sum += d;
    gStats.submit_wait_n++;
    if (d > gStats.submit_wait_max) {
        gStats.submit_wait_max = d;
    }
    worker_lock();
    sGfxTaskAddr = task;
    sGfxRendering = true;
    sGfxSpOwed = true;
    sGfxDpOwed = true;
    worker_unlock();
    enqueue(GFX_CMD_TASK, task);
}

void rt_gfx_present(uint32_t framebuffer) {
    if (sGfxIdle < 0) {
        /* no worker (replay): present here */
        gfx_present_frame(framebuffer);
        return;
    }
    enqueue(GFX_CMD_PRESENT, framebuffer);
}
