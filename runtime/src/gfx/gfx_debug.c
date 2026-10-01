/*
 * gfx_debug.c -- the renderer's debugging aids, all switched on by files next
 * to the EBOOT (runtime/README.md lists them):
 *
 *   shot_frames.txt   save these frames as shot_NNNNN.bmp
 *   SELECT + R        save the next task's RDRAM (capture_N_<task>.bin) and its frame (capture_N.bmp)
 *   dump_frames.txt   save the RDRAM of these graphics tasks (dump_task<N>_<task>.bin)
 *   trace_tasks.txt   log every draw of these graphics tasks
 *   skip_draws.txt    leave these draws (1-based, per frame) out
 *   replay.txt        render a dumped task forever instead of running the game
 */
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_internal.h"

bool gTracing = false;
static int sDrawLimit = -1; /* replay step mode: draws per frame that reach the GE (-1: all) */

static uint32_t sTraceTasks[32];
static int sNumTraceTasks = -1;

/* Draw numbers listed in skip_draws.txt never reach the GE. */
static uint32_t sSkipDraws[32];
static int sNumSkipDraws = -1;

bool gfx_draw_enabled(void) {
    if (sDrawLimit >= 0 && (int)gStats.draw_calls >= sDrawLimit) {
        return false;
    }
    if (sNumSkipDraws < 0) {
        sNumSkipDraws = rt_load_number_list("skip_draws.txt", sSkipDraws, 32);
    }
    for (int i = 0; i < sNumSkipDraws; i++) {
        /* the list is 1-based so that draw 0 can be named */
        if (sSkipDraws[i] == gStats.draw_calls + 1) {
            return false;
        }
    }
    return true;
}

/* ---- screenshots -------------------------------------------------------- */

/* Debug aid: frames listed in shot_frames.txt are copied back from VRAM and saved as BMP. */
static uint32_t sShotFrames[32];
static int sNumShotFrames = -1;
static uint32_t* sShotPixels = NULL; /* allocated on the first screenshot */

static bool sCaptureShot;      /* see maybe_capture */
static uint32_t sCaptureCount;

static bool shot_wanted(uint32_t frame) {
    if (sNumShotFrames < 0) {
        sNumShotFrames = rt_load_number_list("shot_frames.txt", sShotFrames, 32);
    }
    for (int i = 0; i < sNumShotFrames; i++) {
        if (sShotFrames[i] == frame) {
            return true;
        }
    }
    return false;
}

static void write_bmp(uint32_t frame) {
    char name[64];
    char path[256];
    if (sCaptureShot) {
        snprintf(name, sizeof(name), "capture_%u.bmp", (unsigned)sCaptureCount);
        sCaptureShot = false;
    } else {
        snprintf(name, sizeof(name), "shot_%05u.bmp", (unsigned)frame);
    }
    SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    uint32_t w = PSP_SCREEN_W, h = PSP_SCREEN_H;
    uint32_t row_bytes = w * 3;
    uint32_t pad = (4 - row_bytes % 4) % 4;
    uint32_t data_size = (row_bytes + pad) * h;
    uint8_t header[54] = { 'B', 'M' };
    uint32_t file_size = 54 + data_size;
    memcpy(header + 2, &file_size, 4);
    header[10] = 54;
    header[14] = 40;
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    header[26] = 1;
    header[28] = 24;
    memcpy(header + 34, &data_size, 4);
    sceIoWrite(fd, header, sizeof(header));
    static uint8_t row[BUF_WIDTH * 3 + 4];
    for (int y = (int)h - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = sShotPixels[y * BUF_WIDTH + x];
            row[x * 3] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = c & 0xFF;
        }
        memset(row + row_bytes, 0, pad);
        sceIoWrite(fd, row, row_bytes + pad);
    }
    sceIoClose(fd);
    rt_log("screenshot %s", path);
}

uint32_t* gfx_debug_shot_buffer(uint32_t frame) {
    if (!shot_wanted(frame) && !sCaptureShot) {
        return NULL;
    }
    if (sShotPixels == NULL) {
        /* Whole cache lines: the invalidate after the copy must not drop a neighbour's. */
        sShotPixels = memalign(64, BUF_WIDTH * PSP_SCREEN_H * 4);
    }
    return sShotPixels;
}

void gfx_debug_save_shot(uint32_t frame) {
    sceKernelDcacheInvalidateRange(sShotPixels, BUF_WIDTH * PSP_SCREEN_H * 4);
    write_bmp(frame);
}

/* ---- RDRAM dumps and captures ------------------------------------------- */

static uint32_t sDumpTasks[16];
static int sNumDumpTasks = -1;

/*
 * SELECT + R (si.c): the next graphics task is dumped as capture_N.bin -- the
 * RDRAM it is rendered from, loadable with the replay mode
 * (replay.txt) -- and the frame it ends up in is saved as
 * capture_N.bmp. For bugs that only show up in moments no script reaches.
 */
static volatile bool sCaptureRequested = false;

void rt_gfx_request_capture(void) {
    sCaptureRequested = true;
}

static void maybe_capture(uint32_t task) {
    if (!sCaptureRequested) {
        return;
    }
    sCaptureRequested = false;
    char name[64];
    char path[256];
    snprintf(name, sizeof(name), "capture_%u_%08X.bin", (unsigned)++sCaptureCount, (unsigned)task);
    SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_rdram, RDRAM_SIZE);
        sceIoClose(fd);
        rt_log("capture %u: RDRAM for task %08X (frame %u) in %s", (unsigned)sCaptureCount, (unsigned)task,
               (unsigned)gStats.frames + 1, path);
    }
    sCaptureShot = true;
}

void gfx_debug_task_start(uint32_t task_number, uint32_t task) {
    maybe_capture(task);
    if (sNumDumpTasks < 0) {
        sNumDumpTasks = rt_load_number_list("dump_frames.txt", sDumpTasks, 16);
    }
    for (int i = 0; i < sNumDumpTasks; i++) {
        if (sDumpTasks[i] != task_number) {
            continue;
        }
        char name[64];
        char path[256];
        snprintf(name, sizeof(name), "dump_task%u_%08X.bin", (unsigned)task_number, (unsigned)task);
        SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, g_rdram, RDRAM_SIZE);
            sceIoClose(fd);
            rt_log("dumped RDRAM to %s", path);
        }
    }
    if (sNumTraceTasks < 0) {
        sNumTraceTasks = rt_load_number_list("trace_tasks.txt", sTraceTasks, 32);
    }
    gTracing = false;
    for (int i = 0; i < sNumTraceTasks; i++) {
        if (sTraceTasks[i] == task_number) {
            gTracing = true;
            rt_log("trace of gfx task %u", task_number);
        }
    }
}

/* ---- replay ------------------------------------------------------------- */

/*
 * replay.txt ("<dump file> <task hex> [step]") renders one dumped gfx task
 * over and over instead of booting the game, so renderer changes can be
 * checked without playing to the scene again. The task's last colour image is
 * taken as the displayed framebuffer. In step mode frame N shows only the
 * first N draws.
 */
bool rt_gfx_replay(void) {
    char path[256];
    char text[256];
    SceUID fd = sceIoOpen(rt_data_path("replay.txt", path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    int len = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (len <= 0) {
        return false;
    }
    text[len] = 0;
    char* p = strtok(text, " \t\r\n");
    char* task_str = strtok(NULL, " \t\r\n");
    char* mode = strtok(NULL, " \t\r\n");
    bool step = mode != NULL && strcmp(mode, "step") == 0;
    if (p == NULL || task_str == NULL) {
        return false;
    }
    uint32_t task = (uint32_t)strtoul(task_str, NULL, 16);
    fd = sceIoOpen(rt_data_path(p, path, sizeof(path)), PSP_O_RDONLY, 0);
    if (fd < 0) {
        rt_log("replay: cannot open %s", path);
        return false;
    }
    sceIoRead(fd, g_rdram, RDRAM_SIZE);
    sceIoClose(fd);
    gfx_run_task(task);
    uint32_t fb = gRdp.cimg | 0x80000000u;
    rt_log("replay: task %08X from %s, framebuffer %08X", task, path, fb);
    rt_gfx_present(fb);
    for (;;) {
        if (step) {
            sDrawLimit = (int)gStats.frames;
        }
        gfx_run_task(task);
        rt_gfx_present(fb);
        sceDisplayWaitVblankStart();
    }
}
