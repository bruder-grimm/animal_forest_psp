/*
 * main.c -- PSP entry point: memory, ROM, subsystems, then the game's boot code.
 *
 * Also the PSP-side lifecycle: the HOME-button exit, standby and resume.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "audio/me_audio.h"
#include "rt.h"

PSP_MODULE_INFO("AnimalForestPSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
/* Leave room outside the newlib heap for thread stacks and kernel objects
 * (the PSP-1000 has 24MB for everything). */
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

uint8_t* g_rdram = NULL;

static char sBaseDir[192] = "ms0:/PSP/GAME/AFPSP";

/* ROM layout facts (makerom entry code: bss clear + stack setup). */
#define IPL3_LOAD_SIZE 0x100000u
#define BOOT_BSS_START 0x8003E940u
#define BOOT_BSS_SIZE  0x00005D50u
#define BOOT_STACK_TOP 0x8003F330u
#define BOOTPROC_VRAM  0x80025CC0u

/* libultra low-memory globals set by IPL3 */
#define OS_TV_TYPE     0x80000300u
#define OS_ROM_TYPE    0x80000304u
#define OS_ROM_BASE    0x80000308u
#define OS_RESET_TYPE  0x8000030Cu
#define OS_CIC_ID      0x80000310u
#define OS_VERSION     0x80000314u
#define OS_MEM_SIZE    0x80000318u

/* How much is left for the caches, found by asking for it a megabyte at a time. */
static uint32_t sFreeMemory = 0;

uint32_t rt_free_memory(void) {
    return sFreeMemory;
}

/*
 * A thread at the lowest priority that does nothing but count. Calibrated at
 * startup while the machine is otherwise idle, its rate afterwards says how
 * much CPU nothing else wanted -- the honest answer to "are we out of CPU?".
 */
static volatile uint32_t sIdleTicks = 0;
static uint32_t sIdleRate = 0;   /* ticks per second with the CPU all to itself */

static int idle_counter(SceSize args, void* argp) {
    for (;;) {
        sIdleTicks++;
    }
    return 0;
}

uint32_t rt_idle_percent(uint32_t span_us) {
    static uint32_t last = 0;
    uint32_t now = sIdleTicks;
    uint32_t ticks = now - last;
    last = now;
    if (sIdleRate == 0 || span_us == 0) {
        return 0;
    }
    uint64_t idle_us = (uint64_t)ticks * 1000000u / sIdleRate;
    return idle_us >= span_us ? 100 : (uint32_t)(idle_us * 100 / span_us);
}

static void start_idle_counter(void) {
    SceUID thid = sceKernelCreateThread("rt_idle", idle_counter, 0x6F, 2048, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
    rt_log_flush(); /* the log's writer must not be busy while the rate is taken */
    uint32_t t0 = sceKernelGetSystemTimeLow();
    uint32_t start = sIdleTicks;
    sceKernelDelayThread(200000);
    uint32_t span = sceKernelGetSystemTimeLow() - t0;
    sIdleRate = (uint32_t)((uint64_t)(sIdleTicks - start) * 1000000u / span);
    rt_log("idle counter: %u ticks/s with the CPU free", (unsigned)sIdleRate);
}

static uint32_t probe_free_memory(void) {
    enum { CHUNK = 1024 * 1024, MAX_CHUNKS = 64 };
    void* got[MAX_CHUNKS];
    int n = 0;
    while (n < MAX_CHUNKS) {
        void* p = malloc(CHUNK);
        if (p == NULL) {
            break;
        }
        got[n++] = p;
    }
    for (int i = 0; i < n; i++) {
        free(got[i]);
    }
    return (uint32_t)n * CHUNK;
}

const char* rt_data_path(const char* name, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", sBaseDir, name);
    return out;
}

static void exit_game(bool stop_me) {
    rt_log("exit requested");
    /* The ME first: it runs code from, and writes into, memory the exit frees. */
    if (stop_me) {
        rt_me_audio_shutdown();
    }
    rt_save_flush();
    rt_rtc_tick();
    rt_log_flush();
    sceKernelExitGame();
}

static int exit_callback(int arg1, int arg2, void* common) {
    exit_game(true);
    return 0;
}

/*
 * After a resume, on a thread of its own: starts the ME again, then (debug)
 * logs twice a second for 20 s whether the game runs and what every thread of
 * ours is waiting for. The log is written out before and after the ME's
 * start, so that it shows how far things got if the PSP stops there.
 */
static int resume_thread(SceSize args, void* argp) {
    rt_log_flush();
    rt_me_audio_resume();
    rt_log("power: resume handled");
    rt_log_flush();
    for (int i = 0; i < 40; i++) {
        rt_log("resume +%d.%ds: frame %u, polls %u, indirect calls %u (last %08X)", i / 2, (i % 2) * 5,
               (unsigned)rt_gfx_frame_count(), (unsigned)rt_input_polls(), (unsigned)g_indirect_calls,
               (unsigned)g_last_indirect_vram);
        if (i == 4 || i == 20) {
            SceUID ids[64];
            int n = 0;
            sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &n);
            for (int k = 0; k < n && k < 64; k++) {
                SceKernelThreadInfo info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (sceKernelReferThreadStatus(ids[k], &info) == 0) {
                    rt_log("  thread %-16s pri %02X status %X wait type %d on %08X", info.name,
                           (unsigned)info.currentPriority, (unsigned)info.status, info.waitType,
                           (unsigned)info.waitId);
                }
            }
        }
        sceKernelDelayThread(500000);
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

/*
 * Standby: memory stick I/O holds the power lock (rt_io_begin), so none of it
 * overlaps the suspend; files kept open (ROM, text) are reopened when a read
 * fails afterwards (rt_read_at), and the log is never left open. The ME is
 * stopped by me_audio.c's system event handler and started again by
 * resume_thread.
 */
static int power_callback(int unknown, int flags, void* common) {
    rt_log("power: callback flags %08X", (unsigned)flags);
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        SceUID thid = sceKernelCreateThread("rt_resume", resume_thread, 0x10, 32 * 1024, PSP_THREAD_ATTR_USER, NULL);
        if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
            rt_me_audio_resume();
        }
    }
    return 0;
}

/* In libpsppower, but not in its header: a standby the firmware ends by itself. */
int scePowerRequestSuspendTouchAndGo(void);

static int callback_thread(SceSize args, void* argp) {
    int cbid = sceKernelCreateCallback("exit_callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    int pcbid = sceKernelCreateCallback("power_callback", (SceKernelCallbackFunction)power_callback, NULL);
    scePowerRegisterCallback(-1, pcbid);
    /* Debug: standby_test.txt "<seconds> [times]" puts the PSP into standby
     * that long after the start (and that long after each resume); the
     * firmware wakes it again by itself at once, so nobody has to work the
     * power switch. The ME's power stays on, unlike in a real standby. */
    uint32_t standby[2] = {0, 1};
    if (rt_load_number_list("standby_test.txt", standby, 2) > 0) {
        for (uint32_t i = 0; i < standby[1]; i++) {
            sceKernelDelayThreadCB(standby[0] * 1000000u);
            rt_log("power: standby test %u of %u", (unsigned)(i + 1), (unsigned)standby[1]);
            scePowerRequestSuspendTouchAndGo();
        }
    }
    /* Debug: exit_test.txt "<seconds> [1]" takes the HOME exit path by itself
     * after that long (1 = leave the ME running, as before the fix), for
     * testing it over PSPLink, where nobody presses HOME. It is also how a
     * test run there has to end: PSPLink's own reset leaves the ME running. */
    uint32_t test[2] = {0, 0};
    if (rt_load_number_list("exit_test.txt", test, 2) > 0) {
        sceKernelDelayThreadCB(test[0] * 1000000u);
        exit_game(test[1] != 1);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    /* The exit callback logs, stops the ME and saves: 4 KB was too tight. */
    SceUID thid = sceKernelCreateThread("rt_callbacks", callback_thread, 0x11, 32 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}

static void set_base_dir(const char* argv0) {
    if (argv0 == NULL) {
        return;
    }
    const char* slash = strrchr(argv0, '/');
    if (slash == NULL) {
        return;
    }
    size_t len = (size_t)(slash - argv0);
    if (len >= sizeof(sBaseDir)) {
        return;
    }
    memcpy(sBaseDir, argv0, len);
    sBaseDir[len] = '\0';
}

static void show_error(const char* message) {
    rt_log("%s", message);
    rt_gfx_init();
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

/* What IPL3 does: copy the boot segment to its entry point and set libultra's low-memory globals. */
static void load_boot_segment(void) {
    uint8_t header[0x40];
    rt_rom_read(0, header, sizeof(header));
    uint32_t entrypoint = ((uint32_t)header[8] << 24) | ((uint32_t)header[9] << 16) | ((uint32_t)header[10] << 8) | header[11];
    rt_log("ROM entrypoint %08X, title %.20s", entrypoint, (const char*)header + 0x20);
    rt_rom_read_to_rdram(0x1000, entrypoint, IPL3_LOAD_SIZE);

    wr_w32(OS_TV_TYPE, 1);            /* NTSC */
    wr_w32(OS_ROM_TYPE, 0);
    wr_w32(OS_ROM_BASE, 0xB0000000);
    wr_w32(OS_RESET_TYPE, 0);         /* cold boot */
    wr_w32(OS_CIC_ID, 6102);
    wr_w32(OS_VERSION, 0);
    wr_w32(OS_MEM_SIZE, RT_OSMEMSIZE);
    wr_w32(RT_SP_STATUS, 1);          /* RSP always halted between tasks */

    /* makerom's entry code clears the boot segment's bss */
    rt_fill_rdram(BOOT_BSS_START, 0, BOOT_BSS_SIZE);
}

int main(int argc, char* argv[]) {
    set_base_dir(argc > 0 ? argv[0] : NULL);
    setup_callbacks();
    rt_log_init();
    rt_log("Animal Forest PSP runtime starting (base %s)", sBaseDir);
    /* cpu_mhz.txt: run at another CPU clock (222, 266, ...), to see how much headroom there is. */
    uint32_t mhz[1] = { 333 };
    rt_load_number_list("cpu_mhz.txt", mhz, 1);
    scePowerSetClockFrequency((int)mhz[0], (int)mhz[0], (int)mhz[0] / 2);

    /* RDRAM with zeros on both sides for generated code that steps just outside it (MEM_PTR). */
    uint8_t* rdram_block = memalign(64, RDRAM_GUARD + RDRAM_SIZE + RDRAM_GUARD);
    if (rdram_block == NULL) {
        rt_fatal("cannot allocate RDRAM");
    }
    memset(rdram_block, 0, RDRAM_GUARD + RDRAM_SIZE + RDRAM_GUARD);
    g_rdram = rdram_block + RDRAM_GUARD;
    start_idle_counter();
    sFreeMemory = probe_free_memory();
    rt_log("free memory %u KB", (unsigned)(sFreeMemory / 1024));
    rt_log("RDRAM at %p, kernel free %u KB (largest %u KB)", g_rdram,
           (unsigned)(sceKernelTotalFreeMemSize() / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    if (!rt_rom_open()) {
        show_error("ROM not found: place baserom.z64 next to EBOOT.PBP");
    }
    load_boot_segment();

    rt_sections_init();
    rt_sched_init();
    rt_timer_init();
    rt_save_init();
    rt_rtc_init();
    rt_input_init();
    rt_gfx_init();
    rt_gfx_bench();   /* debug: bench_vtx.txt */
    rt_gfx_replay();  /* debug: replay.txt, never returns if present */
    rt_audio_init();
    rt_me_audio_init();
    rt_vi_init();
    rt_preempt_init();

    rt_sched_run_boot(BOOTPROC_VRAM, BOOT_STACK_TOP);
    return 0;
}
