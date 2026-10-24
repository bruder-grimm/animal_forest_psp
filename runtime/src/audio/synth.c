/*
 * synth.c -- the audio engine's synthesis on the Media Engine.
 *
 * Every retrace AF's audio thread builds an audio task (func_800EE050_jp).
 * func_800E319C_jp (the GameCube decomp's Nas_smzAudioFrame) does the work:
 * for each of the frame's updates the sequencer, then for each update again
 * the synthesis -- the reverbs (func_800E2D20_jp) and the RSP command list for
 * the notes (func_800E4600_jp, Nas_DriveRsp). The synthesis is ~40% of the
 * audio thread, and nothing reads what it makes but the RSP task, which runs
 * on the ME (me_audio.c) a retrace later. So the ME makes it:
 *
 *   func_800E319C_jp, here: the sequencer on the main CPU, the synthesis left
 *     as a job; the game is told the command list is empty for now.
 *   rt_synth_start, when the task is built (a hook in func_800EE020_jp): the
 *     job goes to the ME. Not earlier: the rest of the build writes next to
 *     what the synthesis writes, and the ME writes back whole cache lines.
 *   rt_synth_before_task, when the task is submitted at the next retrace (or
 *     the next build, whichever comes first): the job is finished -- by the
 *     ME, or by the main CPU if the ME had not started it (it was busy with
 *     the previous task) -- and the task gets its command list's size.
 *
 * The ME runs copies of the game's own functions (scripts/me_copy.py), so the
 * result is the same; check_me_synth.txt compares the two (check_job below).
 * What they would call outside the game's code:
 *   - The sample DMA (DmaMgr_AudioDmaHandler, called through a pointer):
 *     recorded. The main CPU copies the ROM data, posts the completion
 *     messages and counts the bytes when the job is finished, before the task
 *     that reads the samples runs.
 *   - The game's callbacks for samples made by code and at the end of each
 *     update: set only by audio commands, never seen set. While one is set, or
 *     the DMA or sync pointers are not the game's own, the synthesis stays on
 *     the main CPU.
 * The copies keep their stack outside RDRAM (RT_ME_STACK_TOP), their FPU
 * rounding mode to themselves, and run without preemption points.
 *
 * Memory: the main CPU writes its cache back before handing over the job and
 * drops it before looking at the results. The ME writes back whole 64-byte
 * lines, so while it runs the job nothing else may write the lines it writes:
 * the audio heap's notes and channels, its DMA lists, the audio context's
 * reverb and DMA fields -- the audio thread's alone, and it has finished its
 * frame. (Checked on the PSP against a reference run of the job: no other
 * writes to those lines in 640 jobs.)
 *
 * no_me_synth.txt keeps the synthesis on the main CPU, as without an ME.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspkernel.h>

#include "me_audio.h"
#include "synth.h"
#include "rt.h"

/* ---- the game's ------------------------------------------------------- */

#define AG 0x801494E0u                 /* the audio context (gAudioContext) */
#define AG_NUM_REVERBS (AG + 0x0001)   /* s8 */
#define AG_ADPCM_BOOK (AG + 0x0010)
#define AG_REVERBS (AG + 0x0018)       /* 0x2D0 each */
#define REVERB_SIZE 0x2D0
#define REVERB_USE 0x01                /* u8 */
#define REVERB_FRAMES_TO_IGNORE 0x02   /* u8 */
#define REVERB_CUR_FRAME 0x03          /* u8 */
#define AG_UPDATES (AG + 0x2878)       /* s16 updates a frame */
#define AG_PER_UPDATE (AG + 0x287A)    /* s16 samples an update ... */
#define AG_PER_UPDATE_MAX (AG + 0x287C)
#define AG_PER_UPDATE_MIN (AG + 0x287E)
#define AG_CUR_TASK (AG + 0x28DC)      /* the task being built (its OSTask first) */
#define TASK_DATA_SIZE 0x34

#define SOUND_CALLBACK 0x80152464u     /* samples made by code (codec 4) */
#define UPDATE_CALLBACK 0x80152468u    /* called at the end of each update */
#define SYNC_PROC 0x801130E8u          /* for RAM samples flagged so */
#define SYNC_PROC_DEFAULT 0x800EBF90u  /* func_800EBF90_jp: returns 0 */
#define DMA_PROC 0x801130E0u
#define DMA_PROC_DEFAULT 0x80034AF0u   /* osEPiStartDma */
#define DMA_PROC_GAME 0x800266C4u      /* DmaMgr_AudioDmaHandler: the same, counting the bytes in these two */
#define DMA_BYTES 0x800406B0u
#define DMA_AUDIO_BYTES 0x800406B4u

#define CMD_FRAME 0x40                 /* func_800E319C_jp's stack frame */

/* PI handle and OSIoMesg fields (as pi.c) */
#define PIHANDLE_BASE 0x0C
#define IOMESG_RETQUEUE 0x04
#define IOMESG_DRAMADDR 0x08
#define IOMESG_DEVADDR 0x0C
#define IOMESG_SIZE 0x10

void func_800F6034_jp(uint8_t* rdram, recomp_context* ctx); /* the sequencer, one update */
void func_800E30D4_jp(uint8_t* rdram, recomp_context* ctx); /* the notes' state for one update */
void func_800E2D20_jp(uint8_t* rdram, recomp_context* ctx); /* a reverb, one update */
void func_800E4600_jp(uint8_t* rdram, recomp_context* ctx); /* the command list, one update */
/* The ME's copies (scripts/me_copy.py). */
void me_func_800E2D20_jp(uint8_t* rdram, recomp_context* ctx);
void me_func_800E4600_jp(uint8_t* rdram, recomp_context* ctx);
void me_func_800EBF90_jp(uint8_t* rdram, recomp_context* ctx);

static uint32_t call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* fn, uint32_t sp, uint32_t a0, uint32_t a1,
                     uint32_t a2, uint32_t a3) {
    ctx->r4 = a0;
    ctx->r5 = a1;
    ctx->r6 = a2;
    ctx->r7 = a3;
    ctx->r29 = sp;
    fn(rdram, ctx);
    return ctx->r2;
}

/*
 * func_800E319C_jp's synthesis: for each update the reverbs and the command
 * list, then the reverbs' frame counters. Returns the end of the commands.
 * Runs on either CPU, with either CPU's functions.
 */
static uint32_t synthesize(uint8_t* rdram, recomp_context* ctx, recomp_func_t* reverb, recomp_func_t* commands,
                           uint32_t sp, uint32_t cmd, uint32_t ai_buf, int32_t ai_len) {
    MEM_W(0, AG_ADPCM_BOOK) = 0;
    for (int32_t left = (int16_t)MEM_HU(0, AG_UPDATES); left > 0; left--) {
        int32_t n;
        if (left == 1) {
            n = ai_len;
        } else {
            n = ai_len / left;
            if (n >= (int16_t)MEM_HU(0, AG_PER_UPDATE_MAX)) {
                n = (int16_t)MEM_HU(0, AG_PER_UPDATE_MAX);
            } else if (n <= (int16_t)MEM_HU(0, AG_PER_UPDATE_MIN)) {
                n = (int16_t)MEM_HU(0, AG_PER_UPDATE_MIN);
            } else {
                n = (int16_t)MEM_HU(0, AG_PER_UPDATE);
            }
        }
        for (int32_t i = 0; i < (int8_t)MEM_BU(0, AG_NUM_REVERBS); i++) {
            if (MEM_BU(0, AG_REVERBS + i * REVERB_SIZE + REVERB_USE) != 0) {
                call(rdram, ctx, reverb, sp, (uint32_t)n, (uint32_t)((int16_t)MEM_HU(0, AG_UPDATES) - left), (uint32_t)i, 0);
            }
        }
        cmd = call(rdram, ctx, commands, sp, ai_buf, (uint32_t)n, cmd, (uint32_t)((int16_t)MEM_HU(0, AG_UPDATES) - left));
        ai_len -= n;
        ai_buf += (uint32_t)n * 4;
    }
    for (int32_t i = 0; i < (int8_t)MEM_BU(0, AG_NUM_REVERBS); i++) {
        uint32_t r = AG_REVERBS + i * REVERB_SIZE;
        uint8_t ignore = MEM_BU(0, r + REVERB_FRAMES_TO_IGNORE);
        if (ignore != 0) {
            MEM_BU(0, r + REVERB_FRAMES_TO_IGNORE) = ignore - 1;
        }
        MEM_BU(0, r + REVERB_CUR_FRAME) ^= 1;
    }
    return cmd;
}

/* ---- the job ---------------------------------------------------------- */

#define MAX_DMA 64 /* the game's own limit a frame (its OSIoMesg array) */

typedef struct {
    uint32_t phys, ram, size, mq, mb;
} SynthDma;

/* Written by the main CPU before the job is handed over, by the ME while it runs. Both use it uncached. */
typedef struct {
    uint32_t cmd, ai_buf, task; /* the command list's start, the samples, the task */
    int32_t ai_len;
    uint32_t round_mode;        /* the game's FPU rounding mode */
    uint32_t cmd_end;
    uint32_t faults, fault_vram; /* calls the copies could not make */
    uint32_t dma_count;
    uint32_t dma_counted; /* through DmaMgr_AudioDmaHandler */
    SynthDma dma[MAX_DMA];
} __attribute__((aligned(64))) SynthJob;

static SynthJob sJobMem;
#define JOB ((volatile SynthJob*)(0x40000000u | (uintptr_t)&sJobMem))

/* The copies' registers and rounding mode: theirs alone, whole cache lines. */
static struct {
    recomp_context ctx;
} __attribute__((aligned(64))) sMe;
uint32_t me_fpu_round_mode[16] __attribute__((aligned(64)));

static enum { JOB_NONE, JOB_WAITING, JOB_HANDED } sJob = JOB_NONE;
/* The last command list made, for a task submitted after the game's build hook ran the finish. */
static uint32_t sLastCmd = 0, sLastSize = 0;

static uint32_t sOnMe, sOnMain, sTakenBack, sKept, sFaults;

/* osEPiStartDma(handle, mb, direction), for the copies: the request is recorded. */
static void me_start_dma(uint8_t* rdram, recomp_context* ctx) {
    volatile SynthJob* job = JOB;
    uint32_t mb = ctx->r5;
    if (job->dma_count < MAX_DMA) {
        volatile SynthDma* d = &job->dma[job->dma_count];
        d->phys = (uint32_t)MEM_W(PIHANDLE_BASE, ctx->r4) | (uint32_t)MEM_W(IOMESG_DEVADDR, mb);
        d->ram = (uint32_t)MEM_W(IOMESG_DRAMADDR, mb);
        d->size = (uint32_t)MEM_W(IOMESG_SIZE, mb);
        d->mq = (uint32_t)MEM_W(IOMESG_RETQUEUE, mb);
        d->mb = mb;
        job->dma_count = job->dma_count + 1;
    } else {
        job->faults = job->faults + 1;
        job->fault_vram = DMA_PROC_DEFAULT;
    }
    ctx->r2 = 0;
}

static void me_audio_dma_handler(uint8_t* rdram, recomp_context* ctx) {
    JOB->dma_counted = 1;
    me_start_dma(rdram, ctx);
}

static void me_nothing(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0;
}

recomp_func_t* me_lookup_func(int32_t vram) {
    switch ((uint32_t)vram) {
        case DMA_PROC_DEFAULT:
            return me_start_dma;
        case DMA_PROC_GAME:
            return me_audio_dma_handler;
        case SYNC_PROC_DEFAULT:
            return me_func_800EBF90_jp;
        default:
            JOB->faults = JOB->faults + 1;
            JOB->fault_vram = (uint32_t)vram;
            return me_nothing;
    }
}

void me_do_break(uint32_t vram) {
    JOB->faults = JOB->faults + 1;
    JOB->fault_vram = vram;
}

void me_switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    me_do_break(vram);
}

/* The job, on whichever CPU: the copies, with their own registers and stack. */
void rt_synth_me_run(uint8_t* rdram) {
    volatile SynthJob* job = JOB;
    recomp_context* ctx = &sMe.ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->status_reg = 0xFF03;
    ctx->f_odd = &ctx->f0.u32h;
    me_fpu_round_mode[0] = job->round_mode;
    job->dma_count = 0;
    job->dma_counted = 0;
    job->faults = 0;
    job->cmd_end = synthesize(rdram, ctx, me_func_800E2D20_jp, me_func_800E4600_jp, RT_ME_STACK_TOP - 0x10, job->cmd,
                              job->ai_buf, job->ai_len);
}

/* Can the copies do this frame? Only the game's own DMA and sync functions, no callbacks. */
static bool copies_can_run(void) {
    uint32_t dma = rd_w32(DMA_PROC);
    return rd_w32(SOUND_CALLBACK) == 0 && rd_w32(UPDATE_CALLBACK) == 0 && rd_w32(SYNC_PROC) == SYNC_PROC_DEFAULT &&
           (dma == DMA_PROC_DEFAULT || dma == DMA_PROC_GAME);
}

/* The ROM data the job asked for, and the messages saying it is there, as the PI would. */
static void do_dma(void) {
    volatile SynthJob* job = JOB;
    uint32_t n = job->dma_count;
    for (uint32_t i = 0; i < n; i++) {
        volatile SynthDma* d = &job->dma[i];
        rt_pi_read_now(d->phys, d->ram, d->size);
        if (job->dma_counted) {
            wr_w32(DMA_BYTES, rd_w32(DMA_BYTES) + d->size);
            wr_w32(DMA_AUDIO_BYTES, rd_w32(DMA_AUDIO_BYTES) + d->size);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        rt_post_message(job->dma[i].mq, job->dma[i].mb, false, true);
    }
}

/* Finishes the job, wherever it is. The main CPU's cache may hold what the ME has since changed. */
static void finish(bool checking) {
    if (sJob == JOB_NONE) {
        return;
    }
    volatile SynthJob* job = JOB;
    bool on_me = sJob == JOB_HANDED && rt_me_synth_collect(checking);
    sceKernelDcacheWritebackInvalidateAll();
    if (on_me) {
        sOnMe++;
    } else {
        rt_synth_me_run(g_rdram);
        sTakenBack += sJob == JOB_HANDED;
        sOnMain++;
    }
    sJob = JOB_NONE;
    if (job->faults != 0) {
        sFaults += job->faults;
        RT_LOG_ONCE("synth: the copies could not call %08X (%u times)", (unsigned)job->fault_vram, (unsigned)job->faults);
    }
    do_dma();
    sLastCmd = job->cmd;
    sLastSize = job->cmd_end - job->cmd;
    wr_w32(job->task + TASK_DATA_SIZE, sLastSize);
}

/* ---- the game's side -------------------------------------------------- */

/* Without an ME the job is still made (by the main CPU) when checking, so PPSSPP can check the copies. */
static bool job_wanted(void) {
    return (rt_me_audio_ready() || RT_SWITCH("check_me_synth.txt")) && !RT_SWITCH("no_me_synth.txt");
}

static void check_job(uint8_t* rdram, recomp_context* ctx, uint32_t sp, uint32_t count_ptr);

/*
 * Acmd* func_800E319C_jp(Acmd* cmd, s32* cmd_count, s16* ai_buf, s32 ai_len)
 * (Nas_smzAudioFrame), on the audio thread.
 */
void func_800E319C_jp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t caller_sp = ctx->r29, sp = caller_sp - CMD_FRAME;
    const uint32_t cmd = ctx->r4, count_ptr = ctx->r5, ai_buf = ctx->r6;
    const int32_t ai_len = (int32_t)ctx->r7;

    for (int32_t left = (int16_t)MEM_HU(0, AG_UPDATES); left > 0; left--) {
        call(rdram, ctx, func_800F6034_jp, sp, (uint32_t)(left - 1), 0, 0, 0);
        call(rdram, ctx, func_800E30D4_jp, sp, (uint32_t)((int16_t)MEM_HU(0, AG_UPDATES) - left), 0, 0, 0);
    }

    uint32_t end;
    if (job_wanted() && copies_can_run()) {
        volatile SynthJob* job = JOB;
        job->task = rd_w32(AG_CUR_TASK);
        job->cmd = cmd;
        job->ai_buf = ai_buf;
        job->ai_len = ai_len;
        job->round_mode = recomp_fpu_round_mode;
        sJob = JOB_WAITING;
        if (RT_SWITCH("check_me_synth.txt")) {
            check_job(rdram, ctx, sp, count_ptr);
            return;
        }
        end = cmd; /* the list is made later */
    } else {
        end = synthesize(rdram, ctx, func_800E2D20_jp, func_800E4600_jp, sp, cmd, ai_buf, ai_len);
        sKept++;
    }
    MEM_W(0, count_ptr) = (int32_t)(end - cmd) >> 3;
    ctx->r2 = end;
    ctx->r29 = caller_sp;
}

/* After func_800EE050_jp has built the task (a hook in func_800EE020_jp): the job goes to the ME. */
void rt_synth_start(uint8_t* rdram, recomp_context* ctx) {
    if (sJob != JOB_WAITING) {
        return;
    }
    /* The ME reads the game's state from memory. */
    sceKernelDcacheWritebackAll();
    if (rt_me_synth_submit()) {
        sJob = JOB_HANDED;
    }
}

/* Before func_800EE050_jp builds the next task: the last job is finished, whoever submits its task. */
void rt_synth_before_build(uint8_t* rdram, recomp_context* ctx) {
    finish(false);
}

void rt_synth_before_task(uint32_t data_ptr, uint32_t* data_size) {
    finish(false);
    if (data_ptr == sLastCmd && *data_size == 0) {
        *data_size = sLastSize;
    }
}

/* A capture has no job out: the one there is is finished, as the next task's submit would. */
void rt_synth_capture(RtCapture* c) {
    if (rt_cap_saving(c)) {
        finish(true);
    } else {
        sJob = JOB_NONE;
    }
    uint32_t last[2] = { sLastCmd, sLastSize };
    rt_cap_io(c, "SYN ", last, sizeof(last));
    sLastCmd = last[0];
    sLastSize = last[1];
}

void rt_synth_report(char* buf, int size) {
    snprintf(buf, size, " | synth me %u main %u (taken back %u, kept %u)%s", (unsigned)sOnMe, (unsigned)sOnMain,
             (unsigned)sTakenBack, (unsigned)sKept, sFaults ? " FAULTS" : "");
    sOnMe = sOnMain = sTakenBack = sKept = 0;
}

/* ---- check_me_synth.txt ------------------------------------------------ */

/*
 * Every 16th frame: the synthesis both ways from the same RDRAM, the game's
 * own code on this CPU and the copies on the ME (or here), compared word by
 * word. Two 4 MB copies; the game crawls.
 */
static void check_job(uint8_t* rdram, recomp_context* ctx, uint32_t sp, uint32_t count_ptr) {
    static uint32_t* before = NULL;
    static uint32_t* expect = NULL;
    static uint32_t frames = 0, checks = 0, bad = 0;
    volatile SynthJob* job = JOB;
    uint32_t cmd = job->cmd;
    /* Let the ME finish the task in hand, so that it takes the job. */
    while (rt_me_audio_busy()) {
        sceKernelDelayThread(100);
    }
    if ((frames++ & 15) != 0 || (before == NULL && (before = malloc(RDRAM_SIZE)) == NULL) ||
        (expect == NULL && (expect = malloc(RDRAM_SIZE)) == NULL)) {
        rt_synth_start(rdram, ctx);
        finish(true);
        goto done;
    }
    while (!rt_pi_idle()) {
        sceKernelDelayThread(100);
    }
    memcpy(before, rdram, RDRAM_SIZE);
    /* Nothing else may run in between, as on the ME. */
    rt_preempt_hold(true);
    uint32_t end = synthesize(rdram, ctx, func_800E2D20_jp, func_800E4600_jp, sp, cmd, job->ai_buf, job->ai_len);
    rt_preempt_hold(false);
    while (!rt_pi_idle()) {
        sceKernelDelayThread(100);
    }
    memcpy(expect, rdram, RDRAM_SIZE);
    memcpy(rdram, before, RDRAM_SIZE);
    rt_synth_start(rdram, ctx);
    finish(true);

    /* Not compared: the DMA completion messages, delivered later in either
     * case; the stack below the audio thread's (with the argument slots), which
     * the game's code used and the copies did not; the task's size, which only
     * the job sets here. */
    const uint32_t skip_from = (AG + 0x1EF0) & RDRAM_MASK, skip_to = (AG + 0x2008) & RDRAM_MASK;
    const uint32_t stack_from = (sp - 0x2000) & RDRAM_MASK, stack_to = (sp + 0x10) & RDRAM_MASK;
    const uint32_t size_word = (job->task + TASK_DATA_SIZE) & RDRAM_MASK;
    uint32_t differ = 0;
    const uint32_t* now = (const uint32_t*)rdram;
    for (uint32_t i = 0; i < RDRAM_SIZE / 4; i++) {
        if (now[i] != expect[i] && !(i * 4 >= skip_from && i * 4 < skip_to) &&
            !(i * 4 >= stack_from && i * 4 < stack_to) && i * 4 != size_word) {
            if (differ++ < 8) {
                rt_log("synth check: %08X is %08X, the game's code made %08X", (unsigned)(0x80000000u + i * 4),
                       (unsigned)now[i], (unsigned)expect[i]);
            }
        }
    }
    checks++;
    bad += differ != 0 || job->cmd_end != end;
    rt_log("synth check %u: %u commands (game %u), %u words differ; %u of %u checks differed", (unsigned)checks,
           (unsigned)((job->cmd_end - cmd) >> 3), (unsigned)((end - cmd) >> 3), (unsigned)differ, (unsigned)bad,
           (unsigned)checks);
done:
    MEM_W(0, count_ptr) = (int32_t)sLastSize >> 3;
    ctx->r2 = cmd + sLastSize;
    ctx->r29 = sp + CMD_FRAME;
}
