/*
 * rt.h -- shared declarations of the Animal Forest PSP runtime.
 *
 * The runtime stands in for the N64 hardware and its OS (libultra) under the
 * recompiled game code. Each section below belongs to one source file; see
 * runtime/README.md for how they fit together.
 */
#ifndef AFPSP_RT_H
#define AFPSP_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "recomp_psp.h"

/* The number of elements of an array. */
#define RT_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- RDRAM (main.c, pi.c) ----------------------------------------------- */

extern uint8_t* g_rdram;

/* The RDRAM size the game is told about (osMemSize). */
#define RT_OSMEMSIZE 0x00400000u

/*
 * Runtime-owned RDRAM, in the low area the game never uses (the N64's
 * exception vectors, which the libultra HLE doesn't install).
 */
#define RT_SP_STATUS     0x80000010u  /* target of the sched RSP-status patches (recomp/af.jp.toml) */
#define RT_CART_HANDLE   0x80000080u  /* osCartRomInit's OSPiHandle */
#define RT_FLASH_HANDLE  0x80000100u  /* osFlashInit's OSPiHandle */
#define RT_NAMES_SCRATCH 0x80000200u  /* 3 x 64 bytes: expanded English names (english/names.c) */
/* 0x80000300-0x8000035B: libultra's globals and osAppNMIBuffer (the game uses both). */
#define RT_TAG_SCRATCH   0x800003E0u  /* 32 bytes: a word of the letter tag or a free string (english/letters.c) */

/* Word, halfword and byte access to RDRAM by N64 address (see recomp_psp.h). */
static inline uint32_t rd_w32(uint32_t addr) {
    uint8_t* rdram = g_rdram;
    return (uint32_t)MEM_W(0, addr);
}

static inline void wr_w32(uint32_t addr, uint32_t value) {
    uint8_t* rdram = g_rdram;
    MEM_W(0, addr) = (int32_t)value;
}

static inline uint16_t rd_u16(uint32_t addr) {
    uint8_t* rdram = g_rdram;
    return (uint16_t)MEM_HU(0, addr);
}

static inline void wr_u16(uint32_t addr, uint16_t value) {
    uint8_t* rdram = g_rdram;
    MEM_HU(0, addr) = value;
}

static inline uint8_t rd_u8(uint32_t addr) {
    uint8_t* rdram = g_rdram;
    return MEM_BU(0, addr);
}

static inline void wr_u8(uint32_t addr, uint8_t value) {
    uint8_t* rdram = g_rdram;
    MEM_BU(0, addr) = value;
}

/* Big-endian byte buffer <-> RDRAM (handles the word-swapped layout). */
void rt_copy_to_rdram(uint32_t addr, const uint8_t* src, uint32_t size);
void rt_copy_from_rdram(uint32_t addr, uint8_t* dst, uint32_t size);
void rt_fill_rdram(uint32_t addr, uint8_t value, uint32_t size);

/* ---- calling libultra replacements -------------------------------------- */

/* Stack argument n (n >= 4) of the current call (o32 ABI). */
static inline uint32_t rt_stack_arg(recomp_context* ctx, int n) {
    return rd_w32(ctx->r29 + 4 * n);
}

/* A 64-bit argument or result: two registers (or stack words), the high word first. */
static inline uint64_t rt_u64(uint32_t hi, uint32_t lo) {
    return ((uint64_t)hi << 32) | lo;
}

/* A float argument or result as the bits of a general register, and back. */
static inline float rt_f32(uint32_t bits) {
    union {
        uint32_t u;
        float f;
    } v = { .u = bits };
    return v.f;
}

static inline uint32_t rt_f32_bits(float f) {
    union {
        float f;
        uint32_t u;
    } v = { .f = f };
    return v.u;
}

/* A libultra function the runtime replaces with nothing, or with a constant result. */
#define RT_STUB(name) \
    void name(uint8_t* rdram, recomp_context* ctx) {}
#define RT_STUB_RETURN(name, value) \
    void name(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = (gpr)(value); }

/* ---- files next to the EBOOT (files.c) ---------------------------------- */

/* The EBOOT's directory, from argv[0]. */
void rt_files_init(const char* argv0);
/* out = "<EBOOT directory>/<name>"; returns out. */
const char* rt_data_path(const char* name, char* out, size_t out_size);
/* True if a file of this name sits next to the EBOOT. */
bool rt_data_file_exists(const char* name);
/* Reads up to size bytes of the file; the number read, or -1 if there is no such file. */
int rt_data_read(const char* name, void* buf, int size);
/* The same for a text, which gets a terminating zero (at most size - 1 characters). */
int rt_data_read_text(const char* name, char* text, int size);
/* Creates (or empties) the file for writing; its path is left in `path`. The descriptor, negative on failure. */
int rt_data_create(const char* name, char* path, size_t path_size);
/* Writes the file in one go. */
bool rt_data_write(const char* name, const void* data, uint32_t size);
/* Renames a file, replacing one of the new name (to finish a write that must not be left half done). */
bool rt_data_rename(const char* from, const char* to);
/*
 * Opens a data file of the port for reading: the copy packed into EBOOT.PBP,
 * else the file of that name next to it. Returns the descriptor (negative if
 * there is neither), positioned at the data: `size` bytes at offset `base` of
 * the file whose name is left in `path`.
 */
int rt_data_open(const char* name, char* path, size_t path_size, uint32_t* base, uint32_t* size);

/* Memory stick access while the game runs: standby waits until it is over. */
void rt_io_begin(void);
void rt_io_end(void);
/* Reads at an offset, reopening the file if a standby closed it. */
int rt_read_at(int* fd, const char* path, uint32_t offset, void* buf, int len);

/*
 * A debug switch: true if the named file sits next to the EBOOT. Looked up
 * once, at the first use of each call site. runtime/README.md lists them.
 */
#define RT_SWITCH(name)                                  \
    ({                                                   \
        static int8_t rt_switch__ = -1;                  \
        if (rt_switch__ < 0) {                           \
            rt_switch__ = rt_data_file_exists(name);     \
        }                                                \
        rt_switch__ != 0;                                \
    })

/* The decimal numbers in a file next to the EBOOT (up to max); 0 if it is missing. */
int rt_load_number_list(const char* file, uint32_t* out, int max);

/* A debug switch that lists numbers (frames, tasks, ...), read at the first question. */
typedef struct {
    const char* file;
    int count; /* -1: not read yet */
    uint32_t values[64];
} RtNumbers;
#define RT_NUMBERS(file) { (file), -1, { 0 } }
/* Is value one of the numbers? */
bool rt_numbers_have(RtNumbers* list, uint32_t value);

/* ---- logging (log.c) ---------------------------------------------------- */

void rt_log_init(void);
/* Queues a line for the log; a thread of its own writes it to the memory stick. */
void rt_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/* Writes out what is queued before returning (before exiting, or stopping for good). */
void rt_log_flush(void);

void rt_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Logs a message the first time a given call site reaches it. */
#define RT_LOG_ONCE(...) do {                \
    static bool rt_logged_once__ = false;    \
    if (!rt_logged_once__) {                 \
        rt_logged_once__ = true;             \
        rt_log(__VA_ARGS__);                 \
    }                                        \
} while (0)

/* ---- machine (main.c) --------------------------------------------------- */

/* Game threads and the renderer thread share the CPU at this priority. */
#define RT_GAME_THREAD_PRIORITY 0x20

/*
 * Memory still free after startup. A PSP-2000 has four times the RAM of a
 * PSP-1000; the ROM and texture caches size themselves from this rather than
 * assuming a model.
 */
uint32_t rt_free_memory(void);
/* Share of the CPU nothing wanted since the last call, over span_us. */
uint32_t rt_idle_percent(uint32_t span_us);
/* The CPU time a PSP thread (0: the calling one) has had so far, in us. */
uint64_t rt_thread_cpu_us(int thid);
/* Made new at every link (Makefile: build_id.c); a capture names the build that took it. */
extern const uint32_t rt_build_id;

/* ---- the scratchpad (spram.c) ------------------------------------------ */

/*
 * The PSP's 16 KB scratchpad (0x00010000), which a user program can read, write
 * and run code from, at cache-hit speed and never evicted. RT_SPRAM puts a function in
 * the block that rt_spram_init copies there (spram.c); RT_SPRAM_ENTRY(fn) says where
 * the originals have to jump to it. The hottest code of the renderer lives there, which
 * leaves the instruction cache to the rest.
 */
#define RT_SPRAM __attribute__((section("spram_text")))
#define RT_SPRAM_ENTRY(fn) static void* const fn##_spram_entry __attribute__((section("spram_ptrs"), used)) = (void*)fn
void rt_spram_init(void);
void rt_spram_restore(void);
/* The scratchpad's last 16 bytes say whether it still holds the code (a standby may clear it). */
#define RT_SPRAM_MARKER_ADDR 0x00013FF0u
#define RT_SPRAM_MARKER 0x31525053u /* "SPR1" */
static inline void rt_spram_check(void) {
    if (*(volatile uint32_t*)RT_SPRAM_MARKER_ADDR != RT_SPRAM_MARKER) {
        rt_spram_restore();
    }
}

/* ---- register contexts (ctx.S) ------------------------------------------ */

/* What a call keeps: s0-s7, fp, sp, ra, gp, f20-f31 and the FPU control word (offsets in ctx.S). */
typedef struct {
    uint32_t s[8];
    uint32_t fp, sp, ra, gp;
    float f[12];
    uint32_t fcr31;
} RtCtx;

/* Like setjmp: 0, then 1 each time rt_ctx_jump(c) comes back -- on any PSP thread. */
int rt_ctx_save(RtCtx* c) __attribute__((returns_twice));
void rt_ctx_jump(const RtCtx* c) __attribute__((noreturn));
/* Calls fn(arg) with the stack pointer at top. */
void rt_call_on_stack(void* top, void (*fn)(void*), void* arg);

/* ---- captures: SELECT + R, and resuming from one (capture.c) ------------ */

/*
 * A capture is written and read by the same code: each part of the runtime
 * that holds state of the game has a function that passes it through
 * rt_cap_io, which writes it when a capture is taken and reads it back when
 * one is resumed (rt_cap_saving says which).
 */
typedef struct RtCapture RtCapture;
bool rt_cap_saving(const RtCapture* c);
/* Writes or reads size bytes at data, as the part named by tag (four characters). */
void rt_cap_io(RtCapture* c, const char* tag, void* data, uint32_t size);

void rt_capture_request(void);             /* SELECT + R (si.c) */
void rt_capture_at_submit(uint32_t task);  /* takes a requested capture (gfx/gfx_worker.c) */
/* resume.txt: starts the game from the capture it names instead of booting it; never returns then. */
void rt_capture_resume_if_asked(void);
/* True when the game was resumed from a capture: the save and the clock are then not written back. */
bool rt_capture_resumed(void);
/* Reads a capture's RDRAM into g_rdram for the replay mode; *task gets its graphics task. */
bool rt_capture_read_rdram(const char* path, uint32_t* task);

/* The parts of a capture, in the order they are written. */
void rt_sched_capture(RtCapture* c);
void rt_timer_capture(RtCapture* c);
void rt_vi_capture(RtCapture* c);
void rt_si_capture(RtCapture* c);
void rt_save_capture(RtCapture* c);
void rt_sections_capture(RtCapture* c);
void rt_misc_capture(RtCapture* c);
void rt_audio_capture(RtCapture* c);
void rt_gfx_capture(RtCapture* c);
void rt_en_dialogue_capture(RtCapture* c);
void rt_en_names_capture(RtCapture* c);
void rt_en_dates_capture(RtCapture* c);

/* ---- function lookup and overlays (sections.c) -------------------------- */

void rt_sections_init(void);
/* Indirect calls made by game code, and the last target (for stall reports). */
extern volatile uint32_t g_indirect_calls;
extern volatile uint32_t g_last_indirect_vram;

/* ---- threads and messages (sched.c) ------------------------------------- */

#define OS_EVENT_SW1 0
#define OS_EVENT_SW2 1
#define OS_EVENT_CART 2
#define OS_EVENT_COUNTER 3
#define OS_EVENT_SP 4
#define OS_EVENT_SI 5
#define OS_EVENT_AI 6
#define OS_EVENT_VI 7
#define OS_EVENT_PI 8
#define OS_EVENT_DP 9
#define OS_EVENT_CPU_BREAK 10
#define OS_EVENT_SP_BREAK 11
#define OS_EVENT_FAULT 12
#define OS_EVENT_THREADSTATUS 13
#define OS_EVENT_PRENMI 14
#define OS_NUM_EVENTS 15

/* The game threads' stacks: reserved before the C library's heap, at the same address in every run. */
void* rt_sched_stacks_addr(void);
void rt_sched_init(void);
/* Runs the game's boot code on the calling PSP thread; never returns. */
void rt_sched_run_boot(uint32_t entry_vram, uint32_t sp);

/* Posts a message from any PSP thread; delivered at the next scheduling point. */
void rt_post_message(uint32_t mq, uint32_t msg, bool jam, bool requeue);
/* A retrace message: delivered late rather than lost if the queue is full. */
void rt_post_vi_message(uint32_t mq, uint32_t msg);
/* Posts the message the game registered for an OS event (osSetEventMesg). */
void rt_post_event(int event);
/* Non-blocking send from the running game thread. */
bool rt_send_now(uint32_t mq, uint32_t msg, bool jam);

/* Game thread only: delivers pending messages from other PSP threads and wakes sleepers that are due... */
void rt_process_external(void);
/* ... and lets a higher-priority runnable game thread take over. */
void rt_check_preempt(void);
/* Runs blocking host work for the current game thread, letting the others run meanwhile. */
void rt_sched_native_wait(void (*fn)(void*), void* arg);
/* Game thread only: sleeps for us microseconds, letting the others run meanwhile. */
void rt_sched_sleep(uint64_t us);
/* Makes running game code enter the scheduler at its next preemption point (from any PSP thread). */
void rt_sched_poke(void);

/*
 * Captures (capture.c). rt_sched_quiet: whether every other game thread is
 * waiting for the baton with its context saved, none of them on host work
 * (delivering what was posted first). rt_sched_checkpoint saves the calling
 * game thread's context and calls write(arg); it returns false then, and
 * true when the game is resumed from what write saved. rt_sched_resume
 * starts the threads of a capture just read and never returns.
 */
bool rt_sched_quiet(void);
bool rt_sched_checkpoint(void (*write)(void*), void* arg);
void rt_sched_resume(void) __attribute__((noreturn));
/* Logs how much of its stack each game thread has used. */
void rt_sched_log_stacks(void);

/* Time game threads spent with nothing to run (the CPU was idle). */
extern uint64_t g_idle_us;

/* Stats line helpers: each call reports and resets. */
void rt_sched_report(char* buf, int size, uint32_t span_us);
void rt_sched_vi_report(char* buf, int size);
uint32_t rt_sched_vi_dropped(void);

/* ---- timers (timer.c) --------------------------------------------------- */

void rt_timer_init(void);
/* Has rt_sched_poke called at system time at_us (or at an earlier time already asked for). */
void rt_timer_poke(uint64_t at_us);
/* Every vblank (vi.c): wakes the timer thread for a deadline before the next one (timer.c). */
void rt_timer_vblank(void);
/* Keeps timers from firing (a capture copies them with the messages they post). */
void rt_timer_hold(bool hold);

/* ---- video interface (vi.c) --------------------------------------------- */

void rt_vi_init(void);

/* ---- cartridge ROM and PI DMA (pi.c) ------------------------------------ */

bool rt_rom_open(void);
/* ROM bytes at offset into a big-endian buffer, or straight into RDRAM at addr. */
void rt_rom_read(uint32_t offset, uint8_t* dst, uint32_t size);
void rt_rom_read_to_rdram(uint32_t offset, uint32_t addr, uint32_t size);
uint32_t rt_rom_block_loads(void);
uint32_t rt_rom_block_us(void); /* time spent reading ROM blocks */
void rt_dma_report(char* buf, int size);
bool rt_pi_idle(void); /* no PI DMA queued or in progress */

/* ---- controllers and the cartridge clock (si.c) ------------------------- */

void rt_input_init(void);
uint32_t rt_input_polls(void); /* controller reads so far (the input script's time base) */
void rt_rtc_init(void);
void rt_rtc_tick(void);        /* writes rtc.bin if the game changed the clock */

/* ---- FlashRAM saves (flash.c) ------------------------------------------- */

void rt_save_init(void);
void rt_save_tick(void);  /* called twice a second; writes a save once writes have settled */
void rt_save_flush(void); /* writes a pending save now */

/* ---- renderer (gfx/) ---------------------------------------------------- */

void rt_gfx_init(void);
void rt_gfx_submit_task(uint32_t task); /* runs a graphics task on the renderer thread */
void rt_gfx_present(uint32_t framebuffer);
/* osSpTaskYield/osSpTaskYielded: let the game preempt the frame to run audio. */
void rt_gfx_yield(void);
bool rt_gfx_yielded(void);
/* Nothing queued for the renderer thread or in its hands (gfx_worker.c). */
bool rt_gfx_idle(void);
/* Debug tools */
void rt_gfx_capture_frame(uint32_t n); /* the next task's frame goes to capture_<n>.bmp (gfx_debug.c) */
void rt_gfx_toggle_stretch(void);  /* START + SELECT: the picture stretched, or at 4:3 (gfx_frame.c) */
bool rt_gfx_replay(void);          /* replay.txt: renders a dumped frame forever (gfx_debug.c) */

/* ---- audio (audio.c; the Media Engine in audio/me_audio.h) -------------- */

void rt_audio_init(void);
void rt_audio_run_task(uint32_t task);
/* Called for every message sent, to time the game's audio cycle (see audio.c). */
void rt_audio_note_send(uint32_t mq, uint32_t msg);

extern uint64_t g_audio_us; /* time spent in audio tasks */

/* ---- the game in English (english/) ------------------------------------- */

/* Is the GameCube's English dialogue in use? */
bool rt_en_dialogue_active(void);

#endif
