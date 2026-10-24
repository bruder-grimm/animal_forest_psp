/*
 * hwcount.c -- the Allegrex's performance counters.
 *
 * The CPU counts cycles, cache misses, writebacks, stalls and bus traffic in
 * 21 registers at 0xBC400000 (pspdebug.h's PspDebugProfilerRegs; word 0
 * turns counting on). They are kernel addresses: hwcread.prx (runtime/kmod,
 * next to the PRX) reads them with one system call, about a microsecond.
 * (kcall.prx could too, but it invalidates the whole I-cache every call, and
 * the live game hung when its threads used it.) The registers count for
 * whatever runs, every thread included; the firmware's per-thread copies
 * (sceKernelReferThreadProfiler) only exist when it was booted for profiling.
 *
 * hwc_bench.txt: at start-up, run workloads whose misses are known and log
 * what the counters say about them (rt_hwc_bench).
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspdebug.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "hwcount.h"

int hwcRead(uint32_t* out); /* hwcread.prx (hwcread_stubs.S) */

static int sState = -1;                 /* -1: not set up; 0: none; 1: hwcread.prx; 2: per-thread copy */
static const volatile uint32_t* sThreadRegs = NULL;

static bool load_module(void) {
    char path[256];
    rt_data_path("hwcread.prx", path, sizeof(path));
    SceUID mod = sceKernelLoadModule(path, 0, NULL);
    if (mod < 0) {
        rt_log("hwc: can't load %s (%08X)", path, (unsigned)mod);
        return false;
    }
    int status;
    if (sceKernelStartModule(mod, 0, NULL, &status, NULL) < 0) {
        rt_log("hwc: can't start hwcread.prx");
        return false;
    }
    return true;
}

bool rt_hwc_init(void) {
    if (sState >= 0) {
        return sState > 0;
    }
    sState = 0;
    const volatile uint32_t* t = (const volatile uint32_t*)sceKernelReferThreadProfiler();
    if (t != NULL) {
        sThreadRegs = t;
        sState = 2;
    } else if (load_module()) {
        sState = 1;
    }
    rt_log("hwc: counters %s", sState == 2 ? "per thread (firmware)" : sState == 1 ? "in hardware (all threads)" : "unavailable");
    return sState > 0;
}

void rt_hwc_read(RtHwc* out) {
    if (sState == 1) {
        hwcRead(out->w);
    } else if (sState == 2) {
        for (int i = 0; i < HWC_WORDS; i++) {
            out->w[i] = sThreadRegs[i];
        }
    } else {
        memset(out, 0, sizeof(*out));
        out->w[2] = sceKernelGetSystemTimeLow();
    }
}

void rt_hwc_delta(const RtHwc* a, const RtHwc* b, RtHwc* d) {
    for (int i = 0; i < HWC_WORDS; i++) {
        d->w[i] = b->w[i] - a->w[i];
    }
}

void rt_hwc_log(const char* what, const RtHwc* d, uint32_t us) {
    rt_log("hwc %s: %u us, cpuck %u, systemck %u, i_miss %u, d_miss %u, d_wb %u, memory %u, internal %u, bus %u, "
           "loads %u/%u (cached/uncached), stores %u/%u, sleep %u, local_bus %u",
           what, (unsigned)us, (unsigned)d->w[2], (unsigned)d->w[1], (unsigned)d->w[13], (unsigned)d->w[14],
           (unsigned)d->w[15], (unsigned)d->w[4], (unsigned)d->w[3], (unsigned)d->w[8], (unsigned)d->w[11],
           (unsigned)d->w[9], (unsigned)d->w[12], (unsigned)d->w[10], (unsigned)d->w[7], (unsigned)d->w[19]);
}

/* ---- known workloads ---------------------------------------------------- */

/* Straight-line code: 32 KB (twice the I-cache) and 8 KB (half of it), one pass a call. */
#define BIG_CODE_INSNS 8192
#define SMALL_CODE_INSNS 2048
__asm__(".text\n"
        ".align 6\n"
        ".globl hwc_big_code\n"
        "hwc_big_code:\n"
        ".set noreorder\n"
        ".rept 8190\n"
        "addu $v0, $v0, $a0\n"
        ".endr\n"
        "jr $ra\n"
        "nop\n"
        ".set reorder\n"
        ".align 6\n"
        ".globl hwc_small_code\n"
        "hwc_small_code:\n"
        ".set noreorder\n"
        ".rept 2046\n"
        "addu $v0, $v0, $a0\n"
        ".endr\n"
        "jr $ra\n"
        "nop\n"
        ".set reorder\n");
uint32_t hwc_big_code(uint32_t x);
uint32_t hwc_small_code(uint32_t x);

static volatile uint32_t sSink;

typedef struct {
    RtHwc a;
    uint32_t t;
} Mark;

static void mark(Mark* m) {
    rt_hwc_read(&m->a);
    m->t = sceKernelGetSystemTimeLow();
}

static void done(const char* what, const Mark* m) {
    Mark e;
    mark(&e);
    RtHwc d;
    rt_hwc_delta(&m->a, &e.a, &d);
    rt_hwc_log(what, &d, e.t - m->t);
}

void rt_hwc_bench(void) {
    if (!RT_SWITCH("hwc_bench.txt") || !rt_hwc_init()) {
        return;
    }
    /* run alone: nothing else of ours is started yet, but keep the thread from being preempted by equals */
    Mark m;

    mark(&m);
    done("read cost (two reads, nothing between)", &m);

    mark(&m);
    uint32_t x = 0;
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile("addu %0, %0, %1" : "+r"(x) : "r"(i));
    }
    sSink = x;
    done("hot loop, 1M iterations (expect ~0 misses)", &m);

    hwc_small_code(1);
    mark(&m);
    for (int i = 0; i < 100; i++) {
        sSink = hwc_small_code((uint32_t)i);
    }
    done("8 KB code x100, warm (expect ~0 i_miss)", &m);

    mark(&m);
    for (int i = 0; i < 100; i++) {
        sSink = hwc_big_code((uint32_t)i);
    }
    done("32 KB code x100 (expect 51200 i_miss: 512 lines a pass)", &m);

    const uint32_t bytes = 4u << 20;
    uint8_t* buf = memalign(64, bytes);
    if (buf == NULL) {
        rt_log("hwc: no memory for the data workloads");
        return;
    }
    memset(buf, 1, bytes);
    sceKernelDcacheWritebackInvalidateAll();

    mark(&m);
    uint32_t s = 0;
    for (uint32_t i = 0; i < bytes; i += 64) {
        s += buf[i];
    }
    sSink = s;
    done("4 MB read, one load a 64-byte line (expect 65536 d_miss)", &m);

    mark(&m);
    s = 0;
    for (uint32_t i = 0; i < 16384; i += 4) {
        s += *(volatile uint32_t*)(buf + i);
    }
    for (int r = 0; r < 100; r++) {
        for (uint32_t i = 0; i < 8192; i += 64) {
            s += *(volatile uint32_t*)(buf + i);
        }
    }
    sSink = s;
    done("8 KB read x100 after one cold pass (expect 256 d_miss)", &m);

    /* pointer chase over 65536 lines in a random order: one miss a step, nothing to overlap */
    const uint32_t lines = bytes / 64;
    uint32_t* order = malloc(lines * sizeof(uint32_t));
    if (order != NULL) {
        for (uint32_t i = 0; i < lines; i++) {
            order[i] = i;
        }
        uint32_t r = 12345;
        for (uint32_t i = lines - 1; i > 0; i--) {
            r = r * 1103515245u + 12345u;
            uint32_t j = (r >> 8) % (i + 1);
            uint32_t tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (uint32_t i = 0; i < lines; i++) {
            *(uint8_t**)(buf + order[i] * 64) = buf + order[(i + 1) % lines] * 64;
        }
        free(order);
        sceKernelDcacheWritebackInvalidateAll();
        mark(&m);
        uint8_t* p = buf;
        for (uint32_t i = 0; i < lines; i++) {
            p = *(uint8_t**)p;
        }
        sSink = (uint32_t)(uintptr_t)p;
        done("pointer chase, 65536 random lines (expect 65536 d_miss)", &m);
    }

    /* dirty lines: every store to a fresh line evicts a dirty one */
    mark(&m);
    for (uint32_t i = 0; i < bytes; i += 64) {
        buf[i] = (uint8_t)i;
    }
    done("4 MB write, one store a line (expect 65536 d_miss, ~65536 d_wb)", &m);

    free(buf);
    rt_log("hwc: benchmarks done");
}

/* ---- who the counts belong to ------------------------------------------- */

/*
 * hwc_sections.txt: the counters are read whenever the CPU passes from one of
 * our workers to another -- a game thread taking the baton (sched.c), the
 * renderer starting or ending a task or waiting on the GE (gfx_worker.c,
 * gfx_frame.c), the scheduler going idle -- and what was counted since the
 * last read goes to whoever had the CPU. Firmware and helper threads (timer,
 * VI, audio output) are counted in with whoever they interrupted. A read costs
 * little and its own misses (rt_hwc_bench), which are taken off again.
 */
static int8_t sSections = -1;
static int sOwner = HWC_IDLE;
static RtHwc sLast;
static struct {
    RtHwc sum;
    uint32_t reads;
} sAcc[HWC_OWNERS];

/* The cost of one read, measured once (two reads back to back). */
static RtHwc sReadCost;

/* At start-up, on the main thread: no file or module loading later, from inside the scheduler. */
void rt_hwc_sections_init(void) {
    sSections = RT_SWITCH("hwc_sections.txt") && rt_hwc_init();
    if (sSections) {
        RtHwc a, b;
        rt_hwc_read(&a);
        rt_hwc_read(&b);
        rt_hwc_delta(&a, &b, &sReadCost);
        sLast = b;
        rt_log("hwc: counting by section");
    }
}

/* The renderer worker is in the middle of a command (it may have been preempted by a game thread). */
volatile bool gHwcRenderActive = false;

/* Whoever runs when the game's scheduler goes idle: the renderer, if it has work in hand. */
int rt_hwc_idle_owner(void) {
    return gHwcRenderActive ? HWC_RENDER : HWC_IDLE;
}

void rt_hwc_owner(int owner) {
    static volatile int busy = 0;
    if (sSections <= 0 || owner == sOwner || busy) {
        return;
    }
    busy = 1;
    RtHwc now, d;
    rt_hwc_read(&now);
    rt_hwc_delta(&sLast, &now, &d);
    sLast = now;
    for (int i = 0; i < HWC_WORDS; i++) {
        sAcc[sOwner].sum.w[i] += d.w[i];
    }
    sAcc[sOwner].reads++;
    sOwner = owner;
    busy = 0;
}

static int sum_minus_reads(int o, int w) {
    int64_t v = (int64_t)sAcc[o].sum.w[w] - (int64_t)sAcc[o].reads * sReadCost.w[w];
    return v < 0 ? 0 : (int)v;
}

void rt_hwc_report(uint32_t frames) {
    if (sSections <= 0 || frames == 0) {
        return;
    }
    rt_hwc_owner(sOwner == HWC_RENDER ? HWC_IDLE : HWC_RENDER); /* close the current stretch */
    for (int o = 0; o < HWC_OWNERS; o++) {
        if (sAcc[o].reads == 0) {
            continue;
        }
        char name[16];
        if (o == HWC_RENDER) {
            snprintf(name, sizeof(name), "render");
        } else if (o == HWC_IDLE) {
            snprintf(name, sizeof(name), "idle");
        } else {
            snprintf(name, sizeof(name), "t%d", o);
        }
        int cyc = sum_minus_reads(o, 2), im = sum_minus_reads(o, 13), dm = sum_minus_reads(o, 14);
        int wb = sum_minus_reads(o, 15), mem = sum_minus_reads(o, 4);
        /* a frame's worth: cycles, I and D misses, writebacks, memory stall cycles and their share */
        rt_log("hwc %-6s %7u kcyc/f, i_miss %6u/f, d_miss %6u/f, d_wb %5u/f, mem stall %5u kcyc/f (%u%%), %u reads/f",
               name, (unsigned)(cyc / frames / 1000), (unsigned)(im / frames), (unsigned)(dm / frames),
               (unsigned)(wb / frames), (unsigned)(mem / frames / 1000), cyc ? (unsigned)((int64_t)mem * 100 / cyc) : 0,
               (unsigned)(sAcc[o].reads / frames));
    }
    memset(sAcc, 0, sizeof(sAcc));
}
