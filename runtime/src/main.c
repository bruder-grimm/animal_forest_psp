/*
 * main.c -- PSP entry point: memory, ROM, subsystems, then the game's boot code.
 *
 * Also the PSP-side lifecycle: the HOME-button exit, standby and resume.
 */
#include <pspkernel.h>
#include <psppower.h>
#include <stdlib.h>
#include <string.h>

#include "audio/me_audio.h"
#include "rt.h"

PSP_MODULE_INFO("AnimalForestPSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
/* Leave room outside the newlib heap for thread stacks and kernel objects
 * (the PSP-1000 has 24MB for everything). The game threads' stacks are the
 * runtime's own (sched.c), so this is for the helper threads. */
PSP_HEAP_THRESHOLD_SIZE_KB(1024);

/*
 * RDRAM, with zeros on both sides for generated code that steps just outside
 * it (MEM_PTR). Static, so it is at the same address in every run of a build:
 * the game threads' stacks hold pointers into it, and a capture keeps those
 * (capture.c).
 */
static uint8_t sRdramBlock[RDRAM_GUARD + RDRAM_SIZE + RDRAM_GUARD] __attribute__((aligned(64)));
uint8_t* g_rdram = sRdramBlock + RDRAM_GUARD;

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

uint64_t rt_thread_cpu_us(int thid) {
    SceKernelThreadRunStatus st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(thid, &st) < 0) {
        return 0;
    }
    return rt_u64(st.runClocks.hi, st.runClocks.low);
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

static int exit_callback(int arg1, int arg2, void* common) {
    rt_log("exit requested");
    /* The ME first: it runs code from, and writes into, memory the exit frees. */
    rt_me_audio_shutdown();
    rt_save_flush();
    rt_rtc_tick();
    rt_log_flush();
    sceKernelExitGame();
    return 0;
}

/* After a resume, on a thread of its own: starts the ME again. */
static int resume_thread(SceSize args, void* argp) {
    rt_spram_check();
    rt_me_audio_resume();
    rt_log("power: resumed");
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
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        SceUID thid = sceKernelCreateThread("rt_resume", resume_thread, 0x10, 32 * 1024, PSP_THREAD_ATTR_USER, NULL);
        if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
            rt_me_audio_resume();
        }
    }
    return 0;
}

static int callback_thread(SceSize args, void* argp) {
    int cbid = sceKernelCreateCallback("exit_callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    int pcbid = sceKernelCreateCallback("power_callback", (SceKernelCallbackFunction)power_callback, NULL);
    scePowerRegisterCallback(-1, pcbid);
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
    rt_files_init(argc > 0 ? argv[0] : NULL);
    setup_callbacks();
    rt_log_init();
    char base[256];
    rt_log("Animal Forest PSP runtime starting (in %s)", rt_data_path("", base, sizeof(base)));
    {
        /* cpu_mhz.txt (222 or 266) tests the slower clocks, which the bus follows at half the rate. */
        uint32_t mhz[1] = {333};
        rt_load_number_list("cpu_mhz.txt", mhz, 1);
        if (mhz[0] != 222 && mhz[0] != 266) {
            mhz[0] = 333;
        }
        scePowerSetClockFrequency(mhz[0], mhz[0], mhz[0] / 2);
        rt_log("clock %u MHz", (unsigned)mhz[0]);
    }
    rt_spram_init();

    start_idle_counter();
    sFreeMemory = probe_free_memory();
    rt_log("free memory %u KB", (unsigned)(sFreeMemory / 1024));
    rt_log("build %08X, code at %p, RDRAM at %p, kernel free %u KB (largest %u KB)", (unsigned)rt_build_id,
           (void*)main, g_rdram, (unsigned)(sceKernelTotalFreeMemSize() / 1024),
           (unsigned)(sceKernelMaxFreeMemSize() / 1024));

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
    rt_gfx_replay();  /* debug: replay.txt, never returns if present */
    rt_audio_init();
    rt_me_audio_init();
    rt_vi_init();

    rt_capture_resume_if_asked(); /* debug: resume.txt, never returns if present */
    rt_sched_run_boot(BOOTPROC_VRAM, BOOT_STACK_TOP);
    return 0;
}
