/*
 * vi.c -- video interface: retrace messages and framebuffer swaps.
 *
 * A PSP thread waits for the display's vblank (59.94 Hz, same as NTSC) and
 * posts the VI retrace message every `retrace_count` vblanks. The same
 * thread paces the audio interface's event and the save file writes.
 */
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "rt.h"

static uint32_t sViMq = 0;
static uint32_t sViMsg = 0;
static uint32_t sViRetraceCount = 1;
static uint32_t sCurrentFb = 0;
static bool sViStarted = false;
static uint32_t sViCount = 0;

static int vi_thread(SceSize args, void* argp) {
    uint32_t remaining = 1;
    for (;;) {
        sceDisplayWaitVblankStart();
        sViCount++;

        if (sViStarted && sViMq != 0) {
            if (--remaining == 0) {
                rt_post_vi_message(sViMq, sViMsg);
                remaining = sViRetraceCount != 0 ? sViRetraceCount : 1;
            }
        }
        rt_post_event(OS_EVENT_AI);
        rt_timer_vblank();

        if ((sViCount % 30) == 0) {
            rt_save_tick();
            rt_rtc_tick();
        }
    }
    return 0;
}

void rt_vi_init(void) {
    SceUID thid = sceKernelCreateThread("rt_vi", vi_thread, 0x11, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    sViStarted = true;
}

void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx) {
    sViMq = ctx->r4;
    sViMsg = ctx->r5;
    sViRetraceCount = ctx->r6;
    sViStarted = true;
    rt_log("osViSetEvent mq=%08X msg=%08X retrace=%u", sViMq, sViMsg, sViRetraceCount);
}

void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    sCurrentFb = ctx->r4;
    rt_gfx_present(ctx->r4);
}

void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = sCurrentFb;
}

/* Swaps take effect at once, so the next framebuffer is the current one. */
void osViGetNextFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = sCurrentFb;
}

/* The PSP's display is set up by the renderer; the N64's video modes don't apply. */
RT_STUB(osViSetMode_recomp)
RT_STUB(osViBlack_recomp)
RT_STUB(osViSetSpecialFeatures_recomp)
RT_STUB(osViSetXScale_recomp)
RT_STUB(osViSetYScale_recomp)

/* ---- captures (capture.c) ----------------------------------------------- */

void rt_vi_capture(RtCapture* c) {
    struct {
        uint32_t mq, msg, retrace_count, current_fb;
        bool started;
    } s = { sViMq, sViMsg, sViRetraceCount, sCurrentFb, sViStarted };
    rt_cap_io(c, "VI  ", &s, sizeof(s));
    if (!rt_cap_saving(c)) {
        sViMq = s.mq;
        sViMsg = s.msg;
        sViRetraceCount = s.retrace_count;
        sCurrentFb = s.current_fb;
        sViStarted = s.started;
    }
}
