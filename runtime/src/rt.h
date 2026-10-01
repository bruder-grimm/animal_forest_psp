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
#define RT_SCRATCH       0x80000180u  /* 128 bytes: letter_test.txt's scratch (names_en.c) */
#define RT_NAMES_SCRATCH 0x80000200u  /* 3 x 64 bytes: expanded English names (names_en.c) */
/* 0x80000300-0x8000035B: libultra's globals and osAppNMIBuffer (the game uses both). */
#define RT_TEST_MAIL     0x80000360u  /* 122 bytes: letter_test.txt's letter (names_en.c) */
#define RT_TAG_SCRATCH   0x800003E0u  /* 32 bytes: a word of the pocket menu's letter tag (names_en.c) */

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

/* A libultra function the runtime replaces with nothing, or with a constant result. */
#define RT_STUB(name) \
    void name(uint8_t* rdram, recomp_context* ctx) {}
#define RT_STUB_RETURN(name, value) \
    void name(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = (gpr)(value); }

/* ---- files next to the EBOOT (main.c, log.c) ---------------------------- */

/* out = "<EBOOT directory>/<name>"; returns out. */
const char* rt_data_path(const char* name, char* out, size_t out_size);
/* True if a file of this name sits next to the EBOOT. */
bool rt_data_file_exists(const char* name);
/*
 * Opens a data file of the port for reading (data.c): the copy packed into
 * EBOOT.PBP, else the file of that name next to it. Returns the descriptor
 * (negative if there is neither), positioned at the data: `size` bytes at
 * offset `base` of the file whose name is left in `path`.
 */
int rt_data_open(const char* name, char* path, size_t path_size, uint32_t* base, uint32_t* size);
/* Reads the nonzero decimal numbers in a file next to the EBOOT; 0 if it is missing. */
int rt_load_number_list(const char* file, uint32_t* out, int max);

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

/* Memory stick access while the game runs: standby waits until it is over. */
void rt_io_begin(void);
void rt_io_end(void);
/* Reads at an offset, reopening the file if a standby closed it. */
int rt_read_at(int* fd, const char* path, uint32_t offset, void* buf, int len);

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
bool rt_on_game_thread(void);
/* Runs blocking host work for the current game thread, letting the others run meanwhile. */
void rt_sched_native_wait(void (*fn)(void*), void* arg);
/* Game thread only: sleeps for us microseconds, letting the others run meanwhile. */
void rt_sched_sleep(uint64_t us);
/* Makes running game code enter the scheduler at its next preemption point (from any PSP thread). */
void rt_sched_poke(void);

/* Time game threads spent with nothing to run (the CPU was idle). */
extern uint64_t g_idle_us;

/* Stats line helpers: each call reports and resets. */
void rt_sched_report(char* buf, int size, uint32_t span_us);
void rt_sched_vi_report(char* buf, int size);
uint32_t rt_sched_vi_dropped(void);

/* ---- preemption points (preempt.c) -------------------------------------- */

void rt_preempt_init(void);

/* ---- timers (timer.c) --------------------------------------------------- */

void rt_timer_init(void);
uint64_t rt_os_time(void); /* N64 counter ticks since boot */
/* Has rt_sched_poke called at system time at_us (or at an earlier time already asked for). */
void rt_timer_poke(uint64_t at_us);

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

/* ---- controllers and the cartridge clock (si.c) ------------------------- */

void rt_input_init(void);
uint32_t rt_input_polls(void); /* controller reads so far (the input script's time base) */
uint16_t rt_input_buttons(void); /* N64 buttons of the last read */
void rt_rtc_init(void);
void rt_rtc_tick(void);        /* writes rtc.bin if the game changed the clock */

/* ---- FlashRAM saves (flash.c) ------------------------------------------- */

void rt_save_init(void);
void rt_save_tick(void);  /* called twice a second; writes a save once writes have settled */
void rt_save_flush(void); /* writes a pending save now */
/* Debug: flash_test.txt, run once the game's threads are up (vi.c). */
void rt_debug_flash_test(recomp_context* ctx);

/* ---- renderer (gfx/) ---------------------------------------------------- */

void rt_gfx_init(void);
void rt_gfx_submit_task(uint32_t task); /* runs a graphics task on the renderer thread */
void rt_gfx_present(uint32_t framebuffer);
/* osSpTaskYield/osSpTaskYielded: let the game preempt the frame to run audio. */
void rt_gfx_yield(void);
bool rt_gfx_yielded(void);
uint32_t rt_gfx_frame_count(void);
/* Debug tools */
void rt_gfx_request_capture(void); /* SELECT + R: dump the next frame (gfx_debug.c) */
void rt_gfx_toggle_stretch(void);  /* START + SELECT: the picture stretched, or at 4:3 (gfx_frame.c) */
void rt_gfx_bench(void);           /* bench_vtx.txt: times the vertex stage (gfx_vertex.c) */
bool rt_gfx_replay(void);          /* replay.txt: renders a dumped frame forever (gfx_debug.c) */

/* ---- audio (audio.c; the Media Engine in audio/me_audio.h) -------------- */

void rt_audio_init(void);
void rt_audio_run_task(uint32_t task);
/* Called for every message sent, to time the game's audio cycle (see audio.c). */
void rt_audio_note_send(uint32_t mq, uint32_t msg);

extern uint64_t g_audio_us; /* time spent in audio tasks */

/* ---- English text (text_en.c, names_en.c) ------------------------------- */

bool rt_text_en_active(void);
int rt_text_en_char_width(uint8_t c);
int rt_names_put_calendar(uint32_t dst, uint32_t n, uint32_t k);

#endif
