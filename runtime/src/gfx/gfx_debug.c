/*
 * gfx_debug.c -- the renderer's debugging aids, all switched on by files next
 * to the EBOOT (runtime/README.md lists them):
 *
 *   shot_frames.txt   save these frames as shot_NNNNN.bmp
 *   SELECT + R        save the frame of the task a capture was taken at as capture_N.bmp (capture.c)
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

/* Draw numbers listed in skip_draws.txt never reach the GE. */
bool gfx_draw_enabled(void) {
    static RtNumbers sSkipDraws = RT_NUMBERS("skip_draws.txt");
    if (sDrawLimit >= 0 && (int)gStats.draw_calls >= sDrawLimit) {
        return false;
    }
    /* the list is 1-based so that draw 0 can be named */
    return !rt_numbers_have(&sSkipDraws, gStats.draw_calls + 1);
}

/* ---- screenshots -------------------------------------------------------- */

/* Debug aid: frames listed in shot_frames.txt are copied back from VRAM and saved as BMP. */
static RtNumbers sShotFrames = RT_NUMBERS("shot_frames.txt");
static bool sShotDue;          /* a listed frame came by with no picture drawn: the next one drawn is saved */
static uint32_t* sShotPixels = NULL; /* allocated on the first screenshot */

static bool sCaptureShot;      /* the frame being drawn goes to capture_<sCaptureNumber>.bmp */
static uint32_t sCaptureNumber;
static volatile uint32_t sCaptureNext; /* ... that of the next task (rt_gfx_capture_frame), 0 if none */

static void write_bmp(uint32_t frame) {
    char name[64];
    char path[256];
    if (sCaptureShot) {
        snprintf(name, sizeof(name), "capture_%u.bmp", (unsigned)sCaptureNumber);
        sCaptureShot = false;
    } else {
        snprintf(name, sizeof(name), "shot_%05u.bmp", (unsigned)frame);
    }
    SceUID fd = rt_data_create(name, path, sizeof(path));
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

uint32_t* gfx_debug_shot_buffer(uint32_t frame, bool drawn) {
    sShotDue = sShotDue || rt_numbers_have(&sShotFrames, frame);
    /* A present with nothing drawn since the last one shows no new picture:
     * the buffer it would be read from holds an old frame. */
    if (!drawn || (!sShotDue && !sCaptureShot)) {
        return NULL;
    }
    sShotDue = false;
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

/*
 * SELECT + R (capture.c): the frame drawn from the task a capture was taken
 * at -- the next one the worker starts -- is saved as capture_N.bmp.
 */
void rt_gfx_capture_frame(uint32_t n) {
    sCaptureNext = n;
}

void gfx_debug_task_start(uint32_t task_number, uint32_t task) {
    static RtNumbers sDumpTasks = RT_NUMBERS("dump_frames.txt");
    static RtNumbers sTraceTasks = RT_NUMBERS("trace_tasks.txt");
    if (sCaptureNext != 0) {
        sCaptureNumber = sCaptureNext;
        sCaptureNext = 0;
        sCaptureShot = true;
    }
    if (rt_numbers_have(&sDumpTasks, task_number)) {
        char name[64];
        snprintf(name, sizeof(name), "dump_task%u_%08X.bin", (unsigned)task_number, (unsigned)task);
        if (rt_data_write(name, g_rdram, RDRAM_SIZE)) {
            rt_log("dumped RDRAM to %s", name);
        }
    }
    gTracing = rt_numbers_have(&sTraceTasks, task_number);
    if (gTracing) {
        rt_log("trace of gfx task %u", task_number);
    }
}

/* ---- replay ------------------------------------------------------------- */

/*
 * replay.txt ("<dump file> <task hex> [step]") renders one dumped gfx task
 * over and over instead of booting the game, so renderer changes can be
 * checked without playing to the scene again. The task's last colour image is
 * taken as the displayed framebuffer. In step mode frame N shows only the
 * first N draws. The dump can also be a capture (capture_N.state), whose task
 * is used if none is given ("capture_3.state - step").
 */
bool rt_gfx_replay(void) {
    char text[256];
    if (rt_data_read_text("replay.txt", text, sizeof(text)) <= 0) {
        return false;
    }
    char* dump = strtok(text, " \t\r\n");
    char* task_str = strtok(NULL, " \t\r\n");
    char* mode = strtok(NULL, " \t\r\n");
    bool step = mode != NULL && strcmp(mode, "step") == 0;
    if (dump == NULL || task_str == NULL) {
        return false;
    }
    uint32_t task = (uint32_t)strtoul(task_str, NULL, 16);
    char path[256];
    uint32_t capture_task;
    if (rt_capture_read_rdram(rt_data_path(dump, path, sizeof(path)), &capture_task)) {
        if (task == 0) {
            task = capture_task;
        }
    } else if (rt_data_read(dump, g_rdram, RDRAM_SIZE) < 0) {
        rt_log("replay: cannot open %s", path);
        return false;
    }
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
