/*
 * sp.c -- RSP task submission.
 *
 * Graphics tasks go to the renderer thread (gfx/gfx_worker.c), which posts the
 * SP and DP events when the frame is built. Audio tasks run through the
 * microcode HLE straight away, on the Media Engine where there is one, and SP
 * is posted at once. The game's scheduler picks the events up at its next OS
 * call.
 */
#include "rt.h"

#define M_GFXTASK 1
#define M_AUDTASK 2

#define TASK_TYPE 0x00

static uint32_t sTaskCount[4];

RT_STUB(osSpTaskLoad_recomp)

void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t task = ctx->r4;
    uint32_t type = rd_w32(task + TASK_TYPE);

    if (type < 4 && sTaskCount[type]++ == 0) {
        rt_log("first RSP task of type %u at %08X", type, task);
    }

    switch (type) {
        case M_GFXTASK:
            rt_gfx_submit_task(task);
            break;
        case M_AUDTASK:
            rt_audio_run_task(task);
            rt_post_event(OS_EVENT_SP);
            break;
        default:
            RT_LOG_ONCE("unsupported RSP task type %u", type);
            rt_post_event(OS_EVENT_SP);
            break;
    }
}

/*
 * The game's scheduler preempts a running graphics task whenever an audio task
 * is due (__scHandleRetrace -> __scYield). On the N64 the RSP stops, reports
 * done, runs the audio task, and the graphics task is re-dispatched to resume.
 * Without this the scheduler would wait for the whole frame before it
 * dispatched audio. The renderer reports the dispatch finished and carries on
 * building the frame (rt_gfx_yield).
 */
void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    rt_gfx_yield();
}

void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = rt_gfx_yielded() ? 1 : 0;
}

RT_STUB_RETURN(__osSpSetPc_recomp, 0)
RT_STUB(__osSpSetStatus_recomp)
RT_STUB_RETURN(__osSpGetStatus_recomp, 0x1) /* halted */
RT_STUB_RETURN(__osSpDeviceBusy_recomp, 0)
RT_STUB_RETURN(__osSpRawStartDma_recomp, 0)
