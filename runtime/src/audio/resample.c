/*
 * resample.c -- the audio output's rate conversion (Catmull-Rom), on the Media Engine when there is one.
 *
 * The game's buffers are copied into a ring at the game's rate (g_rs_ring). This turns it into chunks at the
 * DAC's 44.1 kHz and keeps up to RS_QUEUE of them ahead of the output thread (audio.c), so that the thread
 * only has to hand a finished chunk to the hardware. One chunk is ~230 us on the main CPU, which is 2% of it
 * and ahead of the game threads, since the output thread outranks them.
 *
 * Whoever converts, it is one function (produce) on shared state, so the sound is the same bit for bit. The
 * Media Engine takes the work from its idle loop (me_audio.c calls rt_rs_me_poll between audio tasks, at most
 * one chunk at a time); the output thread does it itself when there is no ME, the ME is gone (standby, a
 * timeout) or no_me_resample.txt is next to the EBOOT.
 *
 * The state is a block in uncached memory (RsShared) that both cores use. A chunk is committed by the last
 * thing produce does, raising `produced`: the position after the chunk (rd_end, ph_end) is written before, and
 * is the converter's only state, so a core that stops half-way leaves nothing behind and the other one
 * starts that chunk again.
 *
 * The Media Engine reads the ring and writes the chunks through its own cache (uncached access is too slow):
 * it drops the ring lines it is about to read, which the game thread has written back (rt_rs_publish), and
 * writes the chunk back before committing it. Neither buffer shares a cache line with anything else.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "audio/me_audio.h"
#include "audio/resample.h"
#include "rt.h"

int16_t g_rs_ring[RS_RING_FRAMES * 2] __attribute__((aligned(64)));
static int16_t sQueue[RS_QUEUE][RS_CHUNK * 2] __attribute__((aligned(64)));

typedef struct {
    /* the game thread and the output thread write these */
    volatile uint32_t src_write; /* frames published */
    volatile uint32_t released;  /* chunks below this number are free again */
    volatile uint32_t step;      /* source frames per output frame, 16.16; 0 until rt_rs_start */
    volatile uint32_t me_on;     /* 1: the ME converts, 0: the output thread does */
    volatile uint32_t kick_src;  /* bumped when src_write moved */
    volatile uint32_t kick_out;  /* bumped when released, step or me_on moved */
    uint32_t pad0[10];
    /* whoever converts writes these */
    volatile uint32_t produced;  /* chunks finished */
    volatile uint32_t busy;      /* the ME is inside produce */
    volatile uint32_t n_me;      /* chunks the ME made, the main CPU made (statistics) */
    volatile uint32_t n_main;
    volatile uint32_t rd_end[RS_QUEUE];  /* source position after chunk i (frames), phase after it (16 bits) */
    volatile uint32_t ph_end[RS_QUEUE];
} __attribute__((aligned(64))) RsShared;

static RsShared sRs;
#ifndef RS_UNCACHED_MASK
#define RS_UNCACHED_MASK 0x40000000u /* the uncached alias; tools/asptest/rs_test.c builds this on a host with 0 */
#endif
#define RS ((volatile RsShared*)(RS_UNCACHED_MASK | (uintptr_t)&sRs))
#ifdef RS_HOST_TEST
#define SYNC() asm volatile("" ::: "memory")
#else
#define SYNC() asm volatile("sync" ::: "memory")
#endif

/* The Media Engine's own bookkeeping, on cache lines of its own. */
static struct {
    uint32_t seen_src, seen_out;
    uint32_t more;
} __attribute__((aligned(64))) sMe;

static bool sUseMe = true;      /* no_me_resample.txt clears it */
static bool sMeMaybe = true;    /* the ME might read the ring: the game thread has to write it back */
static bool sStarted = false;
static uint32_t sWaits = 0;     /* get_chunk had to wait for the ME */

void rt_rs_init(void) {
    if (rt_data_file_exists("no_me_resample.txt")) {
        sUseMe = false;
        sMeMaybe = false;
        rt_log("audio: resampling stays on the main CPU (no_me_resample.txt)");
    }
}

void rt_rs_start(uint32_t src_hz, uint32_t out_hz) {
    volatile RsShared* r = RS;
    if (!rt_me_audio_ready()) {
        sMeMaybe = false;
    }
    r->step = (uint32_t)(((uint64_t)src_hz << 16) / out_hz);
    r->kick_out++;
    sStarted = true;
}

/* ---- the converter ------------------------------------------------------- */

/* The position the converter has reached: after the last finished chunk. */
static inline void committed(volatile RsShared* r, uint32_t* rd, uint32_t* ph) {
    uint32_t p = r->produced;
    if (p == 0) {
        *rd = 0;
        *ph = 0;
    } else {
        *rd = r->rd_end[(p - 1) & (RS_QUEUE - 1)];
        *ph = r->ph_end[(p - 1) & (RS_QUEUE - 1)];
    }
}

/*
 * A chunk at the output rate from the ring at the game's rate (Catmull-Rom). step is source frames per output
 * frame and pos the position, 16.16: its integer part counts frames from rd. Even at the AI's highest rate
 * (step < 9.0) a chunk's positions fit 32 bits. Returns the position after the chunk.
 *
 * In fixed point (within 1 LSB rms of the float formula, at most 5): measured on the PSP a chunk takes 227 us
 * instead of 345 in floats -- the FPU's conversions and latencies stalled.
 */
static uint32_t convert_chunk(const int16_t* ring, uint32_t rd, uint32_t pos, uint32_t step, int16_t* chunk) {
    for (uint32_t i = 0; i < RS_CHUNK; i++) {
        uint32_t idx = pos >> 16;
        /* The weights of the four source frames around the position, 1.0 = 32768,
         * for both channels. t, t2 and t3 are 0.16 fixed point (t^2 < 2^32). */
        int32_t t = (int32_t)(pos & 0xFFFF);
        int32_t t2 = (int32_t)(((uint32_t)t * (uint32_t)t) >> 16);
        int32_t t3 = (int32_t)(((uint32_t)t2 * (uint32_t)t) >> 16);
        int32_t w0 = (2 * t2 - t3 - t) >> 2;
        int32_t w1 = ((3 * t3 - 5 * t2) >> 2) + 32768;
        int32_t w2 = (t + 4 * t2 - 3 * t3) >> 2;
        int32_t w3 = (t3 - t2) >> 2;
        const int16_t* p0 = &ring[((rd + idx - 1) & RS_RING_MASK) * 2];
        const int16_t* p1 = &ring[((rd + idx) & RS_RING_MASK) * 2];
        const int16_t* p2 = &ring[((rd + idx + 1) & RS_RING_MASK) * 2];
        const int16_t* p3 = &ring[((rd + idx + 2) & RS_RING_MASK) * 2];
        for (int ch = 0; ch < 2; ch++) {
            /* Summed in 64 bits only so that the compiler chains multiply-adds
             * (madd): the weights' magnitudes add up to at most 1.25, so the sum
             * fits 32 bits and its low word is the result. */
            int64_t acc = (int64_t)w0 * p0[ch];
            acc += (int64_t)w1 * p1[ch];
            acc += (int64_t)w2 * p2[ch];
            acc += (int64_t)w3 * p3[ch];
            int32_t v = (int32_t)acc >> 15;
            chunk[i * 2 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        pos += step;
    }
    return pos;
}

/* The ME's cache: drops the ring frames [first, first + n) (a running count) before they are read. */
static void me_drop_ring(uint32_t first, uint32_t n) {
    first &= RS_RING_MASK;
    uint32_t end = first + n;
    if (end > RS_RING_FRAMES) {
        rt_me_cache_invalidate((uint32_t)&g_rs_ring[0], (end - RS_RING_FRAMES) * 4);
        end = RS_RING_FRAMES;
    }
    rt_me_cache_invalidate((uint32_t)&g_rs_ring[first * 2], (end - first) * 4);
}

/* One chunk, if the ring holds enough and the queue has room. On the ME `me` is set: it manages its cache. */
static bool produce_inner(volatile RsShared* r, bool me) {
    uint32_t p = r->produced;
    uint32_t step = r->step;
    if (step == 0 || p - r->released >= RS_QUEUE) {
        return false;
    }
    uint32_t rd, ph;
    committed(r, &rd, &ph);
    /* Frames from rd - 1 to the last position's idx + 2 are read. */
    uint32_t need = ((RS_CHUNK * step + ph) >> 16) + 4;
    if (r->src_write - rd < need) {
        return false;
    }
    int16_t* chunk = sQueue[p & (RS_QUEUE - 1)];
    if (me) {
        me_drop_ring(rd - 1, need + 1);
    }
    uint32_t pos = convert_chunk(g_rs_ring, rd, ph, step, chunk);
    if (me) {
        rt_me_cache_writeback((uint32_t)chunk, RS_CHUNK * 4);
    }
    r->rd_end[p & (RS_QUEUE - 1)] = rd + (pos >> 16);
    r->ph_end[p & (RS_QUEUE - 1)] = pos & 0xFFFF;
    SYNC();
    r->produced = p + 1;
    SYNC();
    if (me) {
        r->n_me = r->n_me + 1;
    } else {
        r->n_main = r->n_main + 1;
    }
    return true;
}

static bool produce(bool me) {
    volatile RsShared* r = RS;
    if (!me) {
        return produce_inner(r, false);
    }
    /* Announce, then check that the ME is still wanted: rt_rs_sync takes the job back by clearing me_on
     * and waiting for busy to drop, so one of the two never runs while the other does. */
    r->busy = 1;
    SYNC();
    bool did = false;
    if (r->me_on) {
        did = produce_inner(r, true);
    }
    SYNC();
    r->busy = 0;
    return did;
}

int rt_rs_me_poll(void) {
    volatile RsShared* r = RS;
    uint32_t a = r->kick_src;
    uint32_t b = r->kick_out;
    if (a == sMe.seen_src && b == sMe.seen_out && !sMe.more) {
        return 0;
    }
    sMe.seen_src = a;
    sMe.seen_out = b;
    sMe.more = produce(true);
    return (int)sMe.more;
}

/* ---- the game thread ----------------------------------------------------- */

uint32_t rt_rs_written(void) {
    return RS->src_write;
}

uint32_t rt_rs_unread(void) {
    volatile RsShared* r = RS;
    uint32_t rd, ph;
    committed(r, &rd, &ph);
    return r->src_write - rd;
}

void rt_rs_publish(uint32_t first, uint32_t n) {
    volatile RsShared* r = RS;
    if (sMeMaybe) {
        /* The ME reads the ring from memory. */
        uint32_t a = first & RS_RING_MASK;
        uint32_t end = a + n;
        if (end > RS_RING_FRAMES) {
            sceKernelDcacheWritebackRange(&g_rs_ring[0], (end - RS_RING_FRAMES) * 4);
            end = RS_RING_FRAMES;
        }
        sceKernelDcacheWritebackRange(&g_rs_ring[a * 2], (end - a) * 4);
    }
    SYNC();
    r->src_write = first + n;
    SYNC();
    r->kick_src++;
}

/* ---- the output thread --------------------------------------------------- */

void rt_rs_sync(void) {
    volatile RsShared* r = RS;
    bool want = sUseMe && sStarted && rt_me_audio_ready();
    if (want == (r->me_on != 0)) {
        return;
    }
    if (want) {
        /* Frames the game thread wrote while the ME was not in use are still in this core's cache. */
        sceKernelDcacheWritebackRange(g_rs_ring, sizeof(g_rs_ring));
        SYNC();
        r->me_on = 1;
        SYNC();
        r->kick_out++;
        rt_log("audio: resampling on the Media Engine");
    } else {
        r->me_on = 0;
        SYNC();
        /* The ME is at most one chunk (~0.3 ms) from leaving produce; if it was stopped inside it, it never
         * will, and the chunk it was making is made again from the committed position. */
        for (int i = 0; i < 50 && r->busy; i++) {
            sceKernelDelayThread(100);
        }
        r->busy = 0;
        rt_log("audio: resampling on the main CPU");
    }
}

static bool me_could_make_one(volatile RsShared* r) {
    uint32_t rd, ph;
    committed(r, &rd, &ph);
    return r->src_write - rd >= ((RS_CHUNK * r->step + ph) >> 16) + 4;
}

const int16_t* rt_rs_get_chunk(uint32_t n, uint32_t* src_end) {
    volatile RsShared* r = RS;
    if (r->produced == n) {
        if (!r->me_on) {
            produce(false);
        } else if (me_could_make_one(r)) {
            /* The ME is late (it has the audio task, or woke up just now): the ring has what it needs. */
            sWaits++;
            for (int i = 0; i < 20 && r->produced == n; i++) {
                sceKernelDelayThread(100);
            }
        }
        if (r->produced == n) {
            return NULL;
        }
    }
    *src_end = r->rd_end[n & (RS_QUEUE - 1)];
    return sQueue[n & (RS_QUEUE - 1)];
}

void rt_rs_release(uint32_t n) {
    volatile RsShared* r = RS;
    r->released = n;
    SYNC();
    r->kick_out++;
}

const int16_t* rt_rs_chunk_buf(uint32_t n) {
    return sQueue[n & (RS_QUEUE - 1)];
}

void rt_rs_report(char* buf, int size) {
    volatile RsShared* r = RS;
    snprintf(buf, size, "resample me %u main %u waits %u", (unsigned)r->n_me, (unsigned)r->n_main, (unsigned)sWaits);
    r->n_me = 0;
    r->n_main = 0;
    sWaits = 0;
}

#ifdef RS_HOST_TEST
void rt_rs_test_reset(void) {
    memset(&sRs, 0, sizeof(sRs));
    memset(&sMe, 0, sizeof(sMe));
    sStarted = false;
    sUseMe = true;
    sMeMaybe = true;
}
#endif
