/*
 * capture.c -- SELECT + R: a capture of the running game, and resuming from one.
 *
 * A capture (capture_N.state next to the EBOOT) holds everything the game
 * needs to carry on from the moment it was taken: RDRAM, the stack and
 * registers of every game thread, and the runtime's share of the machine --
 * threads and message queues, timers, the overlay table, the save, the
 * cartridge clock, the audio microcode's state, what the renderer still owes
 * the game. It is taken at the first graphics task the game hands over after
 * the buttons, before the renderer gets it, so its RDRAM is also that task's
 * replay dump (tools/afstate.py extracts it; replay.txt takes the capture
 * itself), and capture_N.bmp is the frame drawn from it.
 *
 * resume.txt next to the EBOOT, holding a capture's file name, starts the
 * game from that capture instead of booting it: new PSP threads take over the
 * captured game threads where they were waiting (sched.c), and the thread
 * that took the capture comes back out of rt_sched_checkpoint below -- it
 * hands the same task to the renderer, so the first frame is the captured one.
 * Debug switches work as in any run (trace_tasks.txt, shot_frames.txt, ...);
 * input scripts count polls from the resume. A resumed game saves to
 * resume_flash.bin and keeps the clock it had in the capture, without
 * touching the player's flash.bin and rtc.bin.
 *
 * Only the build that took a capture can resume it: the stacks hold return
 * addresses into its code and pointers into its data. The header names the
 * build (rt_build_id, made new at every link, also in build/psp/build_id.txt)
 * and where its code was loaded; a mismatch stops with the reason in the log.
 *
 * Taking one needs the game's threads all waiting, nothing in the hands of the
 * renderer, the Media Engine or the cartridge DMA thread, and no game thread
 * waiting for host work: that all runs to its end first (wait_until_quiet).
 * Writing it holds the game for a second or two on a memory stick.
 *
 * File layout: a header (CaptureHeader) in the first 4 KB, RDRAM (4 MB,
 * host-order words, as in a replay dump), then the parts, each a 4-character
 * tag, a 32-bit size and the data padded to 4 bytes, ending with "END ".
 */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>

#include "audio/me_audio.h"
#include "rt.h"

#define CAPTURE_MAGIC "AFPSPCAP"
#define CAPTURE_VERSION 1
#define RDRAM_OFFSET 4096
#define QUIET_TIMEOUT_US 3000000

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t build_id;     /* rt_build_id of the build that took it */
    uint32_t code_addr;    /* where that build's code was: rt_capture_at_submit */
    uint32_t rdram_addr;   /* ... its RDRAM */
    uint32_t stacks_addr;  /* ... and the game threads' stacks */
    uint32_t task;         /* the graphics task it was taken at */
    uint32_t number;       /* N of capture_N */
    uint32_t polls;        /* controller reads before it */
    uint32_t rdram_offset;
    uint32_t rdram_size;
    uint32_t state_offset; /* the parts */
    uint32_t state_size;
    uint32_t uptime_ms;    /* the PSP's uptime then */
    char date[32];         /* and its clock */
} CaptureHeader;

struct RtCapture {
    bool saving;
    bool failed;
    SceUID fd;
    uint32_t bytes;        /* written or read so far, of the parts */
};

static volatile bool sRequested = false;
static uint32_t sLastNumber = 0; /* captures are numbered on from the last one next to the EBOOT */
static bool sResumed = false;
static CaptureHeader sResumedFrom;

void rt_capture_request(void) {
    sRequested = true;
}

bool rt_capture_resumed(void) {
    return sResumed;
}

bool rt_cap_saving(const RtCapture* c) {
    return c->saving;
}

/* ---- the parts ---------------------------------------------------------- */

/* Small parts are gathered here and written together: each write to a memory stick has a cost of its own. */
static uint8_t sStage[16 * 1024] __attribute__((aligned(64)));
static uint32_t sStaged = 0;

static void flush_stage(RtCapture* c) {
    if (sStaged > 0 && !c->failed && sceIoWrite(c->fd, sStage, sStaged) != (int)sStaged) {
        c->failed = true;
    }
    sStaged = 0;
}

static void put(RtCapture* c, const void* data, uint32_t size) {
    if (sStaged + size > sizeof(sStage)) {
        flush_stage(c);
    }
    if (size > sizeof(sStage)) {
        if (!c->failed && sceIoWrite(c->fd, data, size) != (int)size) {
            c->failed = true;
        }
    } else {
        memcpy(sStage + sStaged, data, size);
        sStaged += size;
    }
    c->bytes += size;
}

static bool get(RtCapture* c, void* data, uint32_t size) {
    if (c->failed || sceIoRead(c->fd, data, size) != (int)size) {
        c->failed = true;
        return false;
    }
    c->bytes += size;
    return true;
}

void rt_cap_io(RtCapture* c, const char* tag, void* data, uint32_t size) {
    static const uint8_t kZeros[4] = { 0 };
    uint32_t head[2];
    memcpy(&head[0], tag, 4);
    head[1] = size;
    uint32_t pad = (4 - size % 4) % 4;
    if (c->saving) {
        put(c, head, sizeof(head));
        put(c, data, size);
        put(c, kZeros, pad);
        return;
    }
    if (c->failed) {
        return;
    }
    uint32_t found[2];
    uint8_t skip[4];
    if (!get(c, found, sizeof(found)) || found[0] != head[0] || found[1] != size) {
        rt_log("capture: expected part %.4s of %u bytes at byte %u, found %.4s of %u", tag, (unsigned)size,
               (unsigned)c->bytes, (const char*)&found[0], (unsigned)found[1]);
        c->failed = true;
        return;
    }
    get(c, data, size);
    get(c, skip, pad);
}

/* Every part, in order: the same code writes a capture and reads it back. */
static void capture_state(RtCapture* c) {
    /* The timers and the messages they post are copied at one moment: neither twice nor lost. */
    if (c->saving) {
        rt_timer_hold(true);
    }
    rt_sched_capture(c);
    rt_timer_capture(c);
    if (c->saving) {
        rt_timer_hold(false);
    }
    rt_vi_capture(c);
    rt_si_capture(c);
    rt_save_capture(c);
    rt_sections_capture(c);
    rt_misc_capture(c);
    rt_audio_capture(c);
    rt_gfx_capture(c);
    rt_en_dialogue_capture(c);
    rt_en_names_capture(c);
    rt_en_dates_capture(c);
    uint32_t end = 0;
    rt_cap_io(c, "END ", &end, sizeof(end));
}

/* ---- taking one ---------------------------------------------------------- */

/* The next number with neither a capture nor a picture of that number next to the EBOOT. */
static uint32_t next_number(void) {
    char name[64];
    for (uint32_t n = sLastNumber + 1;; n++) {
        snprintf(name, sizeof(name), "capture_%u.state", (unsigned)n);
        bool taken = rt_data_file_exists(name);
        if (!taken) {
            snprintf(name, sizeof(name), "capture_%u.bmp", (unsigned)n);
            taken = rt_data_file_exists(name);
        }
        if (!taken) {
            sLastNumber = n;
            return n;
        }
    }
}

/* Lets everything outside the game's threads finish what it has in hand (see the top of the file). */
static bool wait_until_quiet(void) {
    uint32_t t0 = sceKernelGetSystemTimeLow();
    for (;;) {
        if (rt_gfx_idle() && rt_pi_idle() && !rt_me_audio_busy() && rt_sched_quiet()) {
            return true;
        }
        if (sceKernelGetSystemTimeLow() - t0 > QUIET_TIMEOUT_US) {
            return false;
        }
        sceKernelDelayThread(1000);
    }
}

static void fill_header(CaptureHeader* h, uint32_t task, uint32_t number) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, CAPTURE_MAGIC, sizeof(h->magic));
    h->version = CAPTURE_VERSION;
    h->build_id = rt_build_id;
    h->code_addr = (uint32_t)(uintptr_t)rt_capture_at_submit;
    h->rdram_addr = (uint32_t)(uintptr_t)g_rdram;
    h->stacks_addr = (uint32_t)(uintptr_t)rt_sched_stacks_addr();
    h->task = task;
    h->number = number;
    h->polls = rt_input_polls();
    h->rdram_offset = RDRAM_OFFSET;
    h->rdram_size = RDRAM_SIZE;
    h->state_offset = RDRAM_OFFSET + RDRAM_SIZE;
    h->uptime_ms = (uint32_t)(sceKernelGetSystemTimeWide() / 1000);
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    snprintf(h->date, sizeof(h->date), "%04d-%02d-%02d %02d:%02d:%02d", now.year, now.month, now.day, now.hour,
             now.minute, now.second);
}

typedef struct {
    uint32_t task;
    uint32_t number;
    bool ok;
} CaptureJob;

/* Writes the capture (rt_sched_checkpoint calls this with the calling thread's context saved). */
static void write_capture(void* arg) {
    CaptureJob* job = arg;
    static uint8_t block[RDRAM_OFFSET] __attribute__((aligned(64)));
    CaptureHeader* h = (CaptureHeader*)block;
    memset(block, 0, sizeof(block));
    fill_header(h, job->task, job->number);

    /* Written under a name of its own and renamed when it is complete. */
    char tmp_path[256];
    RtCapture c = { .saving = true, .failed = false };
    rt_io_begin();
    c.fd = rt_data_create("capture.tmp", tmp_path, sizeof(tmp_path));
    if (c.fd < 0) {
        rt_io_end();
        rt_log("capture: cannot create %s (%08X)", tmp_path, (unsigned)c.fd);
        return;
    }
    c.failed = sceIoWrite(c.fd, block, sizeof(block)) != (int)sizeof(block) ||
               sceIoWrite(c.fd, g_rdram, RDRAM_SIZE) != (int)RDRAM_SIZE;
    sStaged = 0;
    capture_state(&c);
    flush_stage(&c);
    h->state_size = c.bytes;
    if (!c.failed) {
        c.failed = sceIoLseek32(c.fd, 0, PSP_SEEK_SET) != 0 || sceIoWrite(c.fd, block, sizeof(block)) != (int)sizeof(block);
    }
    sceIoClose(c.fd);
    if (!c.failed) {
        char name[64];
        snprintf(name, sizeof(name), "capture_%u.state", (unsigned)job->number);
        c.failed = !rt_data_rename("capture.tmp", name);
    }
    rt_io_end();
    job->ok = !c.failed;
}

/* The RDRAM alone, as capture_N_<task>.bin, when the game can't be captured whole. */
static void write_rdram_only(uint32_t task, uint32_t number) {
    char name[64];
    snprintf(name, sizeof(name), "capture_%u_%08X.bin", (unsigned)number, (unsigned)task);
    rt_data_write(name, g_rdram, RDRAM_SIZE);
    rt_log("capture %u: RDRAM only for task %08X in %s", (unsigned)number, (unsigned)task, name);
}

void rt_capture_at_submit(uint32_t task) {
    if (!sRequested) {
        return;
    }
    sRequested = false;
    uint32_t t0 = sceKernelGetSystemTimeLow();
    uint32_t number = next_number();
    if (!wait_until_quiet()) {
        rt_log("capture %u: the game didn't come to rest within %u s, so it can't be resumed", (unsigned)number,
               QUIET_TIMEOUT_US / 1000000);
        write_rdram_only(task, number);
        rt_gfx_capture_frame(number);
        return;
    }
    uint32_t quiet_us = sceKernelGetSystemTimeLow() - t0;
    /* The Media Engine wrote parts of RDRAM behind this core's cache. */
    sceKernelDcacheWritebackInvalidateAll();

    CaptureJob job = { task, number, false };
    if (rt_sched_checkpoint(write_capture, &job)) {
        /* Resumed: this is a new run, carrying on from the capture. */
        rt_log("resumed from capture %u (task %08X, taken %s)", (unsigned)sResumedFrom.number,
               (unsigned)sResumedFrom.task, sResumedFrom.date);
        return;
    }
    rt_gfx_capture_frame(number);
    if (!job.ok) {
        rt_log("capture %u: writing it failed", (unsigned)number);
        write_rdram_only(task, number);
        return;
    }
    rt_log("capture %u: capture_%u.state, task %08X, poll %u (waited %u ms for the game to rest, %u ms in all)",
           (unsigned)number, (unsigned)number, (unsigned)task, (unsigned)rt_input_polls(), (unsigned)(quiet_us / 1000),
           (unsigned)((sceKernelGetSystemTimeLow() - t0) / 1000));
    rt_sched_log_stacks();
}

/* ---- resuming ----------------------------------------------------------- */

/* Opens a capture and reads its header: the descriptor, or -1 if path isn't one. */
static SceUID open_capture(const char* path, CaptureHeader* h) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }
    if (sceIoRead(fd, h, sizeof(*h)) != (int)sizeof(*h) || memcmp(h->magic, CAPTURE_MAGIC, sizeof(h->magic)) != 0) {
        sceIoClose(fd);
        return -1;
    }
    return fd;
}

bool rt_capture_read_rdram(const char* path, uint32_t* task) {
    CaptureHeader h;
    SceUID fd = open_capture(path, &h);
    if (fd < 0) {
        return false;
    }
    bool ok = h.rdram_size == RDRAM_SIZE && sceIoLseek32(fd, (int)h.rdram_offset, PSP_SEEK_SET) == (int)h.rdram_offset &&
              sceIoRead(fd, g_rdram, RDRAM_SIZE) == (int)RDRAM_SIZE;
    sceIoClose(fd);
    *task = h.task;
    return ok;
}

void rt_capture_resume_if_asked(void) {
    char text[256];
    if (rt_data_read_text("resume.txt", text, sizeof(text)) < 0) {
        return;
    }
    char* name = strtok(text, " \t\r\n");
    if (name == NULL) {
        return;
    }
    char path[256];
    rt_data_path(name, path, sizeof(path));

    CaptureHeader* h = &sResumedFrom;
    RtCapture c = { .saving = false, .failed = false };
    c.fd = open_capture(path, h);
    if (c.fd < 0) {
        rt_fatal("resume.txt: %s is not a capture", path);
    }
    if (h->version != CAPTURE_VERSION) {
        rt_fatal("resume.txt: %s is a version %u capture, this build reads version %u", path, (unsigned)h->version,
                 CAPTURE_VERSION);
    }
    if (h->build_id != rt_build_id) {
        rt_fatal("resume.txt: %s was taken by build %08X, this is build %08X: only that build can resume it", path,
                 (unsigned)h->build_id, (unsigned)rt_build_id);
    }
    uint32_t stacks = (uint32_t)(uintptr_t)rt_sched_stacks_addr();
    if (h->code_addr != (uint32_t)(uintptr_t)rt_capture_at_submit || h->rdram_addr != (uint32_t)(uintptr_t)g_rdram ||
        h->stacks_addr != stacks) {
        rt_fatal("resume.txt: %s was taken with the code at %08X, RDRAM at %08X and the game stacks at %08X; here "
                 "they are at %08X, %08X and %08X",
                 path, (unsigned)h->code_addr, (unsigned)h->rdram_addr, (unsigned)h->stacks_addr,
                 (unsigned)(uintptr_t)rt_capture_at_submit, (unsigned)(uintptr_t)g_rdram, (unsigned)stacks);
    }
    rt_log("resuming capture %u from %s (task %08X, taken %s)", (unsigned)h->number, path, (unsigned)h->task, h->date);
    if (sceIoLseek32(c.fd, (int)h->rdram_offset, PSP_SEEK_SET) != (int)h->rdram_offset ||
        sceIoRead(c.fd, g_rdram, RDRAM_SIZE) != (int)RDRAM_SIZE ||
        sceIoLseek32(c.fd, (int)h->state_offset, PSP_SEEK_SET) != (int)h->state_offset) {
        rt_fatal("resume.txt: %s is cut short", path);
    }
    sResumed = true;
    capture_state(&c);
    sceIoClose(c.fd);
    if (c.failed) {
        rt_fatal("resume.txt: %s could not be read", path);
    }
    sceKernelDcacheWritebackAll();
    rt_sched_resume();
}
