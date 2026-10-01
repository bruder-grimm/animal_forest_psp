/*
 * audio.c -- audio tasks and the audio interface (AI).
 *
 * Audio RSP tasks run through the microcode HLE in audio/aspmain.c, on the
 * Media Engine where there is one (audio/me_audio.c). The game hands each
 * finished buffer of samples to func_800EFD40_jp (its inlined
 * osAiSetNextBuffer); it is copied into a ring buffer, which a PSP thread
 * plays through sceAudio's SRC channel.
 *
 * The game sizes each new buffer from osAiGetLength (samples still queued),
 * aiming to keep a small backlog. Reporting the ring fill minus a bias makes
 * it keep a deeper one than it would on an N64, which is what lets the sound
 * survive a frame the PSP takes too long over (LENGTH_BIAS).
 */
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <string.h>

#include "audio/me_audio.h"
#include "rt.h"

#define VI_NTSC_CLOCK 48681812

#define OUT_CHUNK 512             /* frames per sceAudio output call */
#define OUT_RATE_44K 44100
#define RING_FRAMES 16384         /* power of two */
#define RING_MASK (RING_FRAMES - 1)
/*
 * How much sound we keep ahead of the speaker. The game sizes each buffer from
 * what osAiGetLength reports, so under-reporting the ring by this much is what
 * makes it keep that much queued; in steady state the ring holds its own target
 * plus this. At 32 kHz, 3072 frames is 96 ms, which comes out around 110 ms in
 * practice -- in line with the 104 ms the OoT PSP port aims for, and enough to
 * ride out a scene that stretches a frame well past 16.6 ms.
 */
#define LENGTH_BIAS 3072
#define CONCEAL_FRAMES 256        /* repeated to cover an underrun */
/*
 * Nothing is played until the ring has this much in it for the first time.
 * Starting an empty ring against a 16 ms output chunk means the first frames
 * that run long empty it again, and the game is at its slowest while it is
 * still loading. Priming to the steady-state target costs a delay of that
 * length once, and the game is already generating at full tilt to fill it
 * (osAiGetLength reports nothing queued until the ring passes LENGTH_BIAS).
 */
#define PRIME_FRAMES LENGTH_BIAS
#define PRIME_TIMEOUT_US 3000000  /* give up waiting if the game makes nothing */

uint64_t g_audio_us = 0;

static uint32_t sTaskCount = 0;
static bool sLastOnMe = false;    /* which core ran the last task */
static uint32_t sCpuTasks = 0;    /* tasks the main CPU ran, since the last stats line */

/* ---- ring buffer -------------------------------------------------------- */

static int16_t sRing[RING_FRAMES * 2];
static volatile uint32_t sRingWrite = 0; /* frames, advanced by the game thread */
static volatile uint32_t sRingRead = 0;  /* frames, advanced by the output thread */

static volatile uint32_t sFrequency = 0; /* the game's output rate, once it sets one */
static SceUID sFreqSignal = -1;

static uint32_t sSubmitted = 0;  /* buffers the game handed over */
static uint32_t sUnderruns = 0;
static uint32_t sOverflows = 0;
static uint32_t sProduced = 0;   /* frames the game handed over, since the last rate line */
static uint32_t sConsumed = 0;   /* frames sent to the hardware, likewise (in the game's rate) */
static uint32_t sRateMark = 0;

static uint32_t ring_fill(void) {
    return sRingWrite - sRingRead;
}

/* ---- debug recordings --------------------------------------------------- */

static void write_wav_header(SceUID fd, uint32_t rate, uint32_t frames) {
    uint8_t h[44];
    uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t w = 1;
    memcpy(h + 20, &w, 2);
    w = 2;
    memcpy(h + 22, &w, 2);
    memcpy(h + 24, &rate, 4);
    v = rate * 4;
    memcpy(h + 28, &v, 4);
    w = 4;
    memcpy(h + 32, &w, 2);
    w = 16;
    memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    sceIoWrite(fd, h, sizeof(h));
}

/*
 * audio_wav.txt holds "start seconds, length seconds": the output thread
 * records what it plays in that window to afpsp_audio.wav.
 */
static SceUID sWavFd = -1;
static uint32_t sWavStart = 0;
static uint32_t sWavFrames = 0;
static uint32_t sWavWritten = 0;
static uint32_t sPlayedFrames = 0;

static void output_wav_open(uint32_t rate) {
    uint32_t v[2];
    if (rt_load_number_list("audio_wav.txt", v, 2) < 2) {
        return;
    }
    char path[256];
    sWavFd = sceIoOpen(rt_data_path("afpsp_audio.wav", path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                       0777);
    if (sWavFd >= 0) {
        sWavStart = v[0] * rate;
        sWavFrames = v[1] * rate;
        write_wav_header(sWavFd, rate, 0);
        rt_log("audio: recording %u s from %u s to %s", (unsigned)v[1], (unsigned)v[0], path);
    }
}

static void output_wav_write(const int16_t* chunk, uint32_t frames, uint32_t rate) {
    if (sWavFd < 0) {
        return;
    }
    uint32_t start = sPlayedFrames;
    sPlayedFrames += frames;
    if (sPlayedFrames <= sWavStart) {
        return;
    }
    uint32_t skip = start < sWavStart ? sWavStart - start : 0;
    uint32_t n = frames - skip;
    if (n > sWavFrames - sWavWritten) {
        n = sWavFrames - sWavWritten;
    }
    sceIoWrite(sWavFd, chunk + skip * 2, n * 4);
    sWavWritten += n;
    if (sWavWritten >= sWavFrames) {
        write_wav_header(sWavFd, rate, sWavWritten);
        sceIoClose(sWavFd);
        sWavFd = -1;
        rt_log("audio: recording done");
    }
}

/*
 * audio_raw.txt holds "first count": the sample buffers the game hands over,
 * from buffer number `first` on, go to afpsp_raw.wav. Unlike the output this
 * does not depend on timing, so recordings of the same scene can be compared
 * bit for bit (e.g. Media Engine against main CPU).
 */
static SceUID sRawFd = -1;
static bool sRawLoaded = false;
static uint32_t sRawFirst = 0;
static uint32_t sRawCount = 0;
static uint32_t sRawWritten = 0;

/* count: frames to write; total: frames in this buffer (0 for the rest of one that wrapped). */
static void raw_record(const int16_t* samples, uint32_t count, uint32_t total, uint32_t rate) {
    if (!sRawLoaded) {
        uint32_t v[2];
        sRawLoaded = true;
        if (rt_load_number_list("audio_raw.txt", v, 2) < 2) {
            return;
        }
        char path[256];
        sRawFd = sceIoOpen(rt_data_path("afpsp_raw.wav", path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                           0777);
        sRawFirst = v[0];
        sRawCount = v[1];
        if (sRawFd >= 0) {
            write_wav_header(sRawFd, rate, 0);
            rt_log("audio: recording %u buffers from buffer %u to %s", (unsigned)sRawCount, (unsigned)sRawFirst, path);
        }
    }
    if (sRawFd < 0 || sSubmitted < sRawFirst) {
        return;
    }
    if (sSubmitted >= sRawFirst + sRawCount && total != 0) {
        write_wav_header(sRawFd, rate, sRawWritten);
        sceIoClose(sRawFd);
        sRawFd = -1;
        rt_log("audio: buffer recording done (%u frames)", (unsigned)sRawWritten);
        return;
    }
    sceIoWrite(sRawFd, samples, count * 4);
    sRawWritten += count;
}

/* dump_audio.txt lists task numbers: those tasks' RDRAM goes to aspt_<n>.bin, for testing the microcode off the PSP. */
static void maybe_dump_task(uint32_t task_number, uint32_t task) {
    static uint32_t sDumpTasks[32];
    static int sNumDumpTasks = -1;
    if (sNumDumpTasks < 0) {
        sNumDumpTasks = rt_load_number_list("dump_audio.txt", sDumpTasks, 32);
    }
    for (int i = 0; i < sNumDumpTasks; i++) {
        if (sDumpTasks[i] != task_number) {
            continue;
        }
        char name[64];
        char path[256];
        snprintf(name, sizeof(name), "aspt_%05u.bin", (unsigned)task_number);
        SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            uint32_t hdr[3];
            memcpy(&hdr[0], "ASPT", 4);
            hdr[1] = task;
            hdr[2] = RT_OSMEMSIZE;
            sceIoWrite(fd, hdr, sizeof(hdr));
            sceIoWrite(fd, g_rdram, RT_OSMEMSIZE);
            sceIoClose(fd);
            rt_log("audio: dumped task %u to %s", (unsigned)task_number, path);
        }
    }
}

/* ---- output ------------------------------------------------------------- */

/*
 * The buffers handed to the hardware. The audio driver does not copy a buffer
 * when sceAudioSRCOutputBlocking accepts it: it reads it while that chunk
 * plays, i.e. *after* the call has returned, and stops needing it only when
 * the next call returns. So two buffers are used in turn, as PSPSDK's
 * pspaudiolib and the OoT PSP port do; with one, the next chunk was written
 * over the one still playing. (PPSSPP copies the samples at the call, so it
 * never shows this.)
 *
 * The driver also reads them from memory, not through the CPU's data cache,
 * so each one is written back before it is handed over. They own whole cache
 * lines so the write-back touches nothing else.
 */
#define OUT_BUFFERS 2
static int16_t sOutBuf[OUT_BUFFERS][OUT_CHUNK * 2] __attribute__((aligned(64)));
static uint32_t sOutIndex = 0;

/*
 * How well the output thread keeps the hardware fed: whether it hands each
 * chunk over before the hardware has played out what it already holds. A late
 * hand-over is a gap in the sound that no recording of the samples can show.
 */
static uint32_t sHwLate = 0;        /* chunks where the hardware had already run dry */
static uint32_t sHwWorstGapUs = 0;  /* worst turnaround between two output calls */
static uint32_t sHwChunks = 0;
static uint32_t sPrevOutEnd = 0;    /* when the last output call returned */
static uint32_t sChunkUs = 0;       /* how long one chunk plays for */

/* The end of the last chunk played, looped to cover an underrun (resampled output). */
static int16_t sHist[CONCEAL_FRAMES * 2];
static uint32_t sHistFill = 0;

/*
 * Hands one chunk to the hardware, timing the handover. In steady state the
 * blocking call waits about one chunk for a buffer to free; if it returns at
 * once the hardware had nothing left to play and the sound has just gapped.
 * Afterwards the thread fills the other buffer: the hardware now owns this one.
 */
static void output_chunk(const int16_t* buf) {
    sceKernelDcacheWritebackRange(buf, OUT_CHUNK * 4);
    uint32_t t0 = sceKernelGetSystemTimeLow();
    if (sPrevOutEnd != 0) {
        uint32_t gap = t0 - sPrevOutEnd;
        if (gap > sHwWorstGapUs) {
            sHwWorstGapUs = gap;
        }
    }
    sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, (void*)buf);
    uint32_t t1 = sceKernelGetSystemTimeLow();
    if (sPrevOutEnd != 0 && sChunkUs != 0 && t1 - t0 < sChunkUs / 8) {
        sHwLate++;
    }
    sHwChunks++;
    sPrevOutEnd = t1;
    sOutIndex = (sOutIndex + 1) % OUT_BUFFERS;
}

static void note_underrun(void) {
    if (sSubmitted != 0) {
        sUnderruns++;
    }
}

/*
 * A chunk at 44.1 kHz from the ring at the game's rate (Catmull-Rom). step is
 * source frames per output frame and *phase the position between two source
 * frames, both 16.16. Even at the AI's highest rate (step < 9.0) a chunk's
 * positions fit 32 bits.
 *
 * In fixed point (within 1 LSB rms of the float formula, at most 5): measured
 * on the PSP a chunk takes 227 us instead of 345 in floats -- the FPU's
 * conversions and latencies stalled -- which is 1% of the CPU, taken ahead
 * of the game, since this thread outranks it.
 */
static void fill_resampled(int16_t* chunk, uint32_t step, uint32_t* phase) {
    uint32_t need = ((OUT_CHUNK * step + *phase) >> 16) + 4;
    if (ring_fill() < need) {
        /*
         * Underrun. Silence would click going in and coming out, so the end of
         * the last chunk is looped instead, fading out if the game stays behind.
         */
        note_underrun();
        for (uint32_t i = 0; i < OUT_CHUNK; i++) {
            if (sHistFill == 0) {
                chunk[i * 2] = chunk[i * 2 + 1] = 0;
                continue;
            }
            uint32_t k = i % sHistFill;
            int32_t fade = 256 - (int32_t)(i >> 1);
            if (fade < 0) {
                fade = 0;
            }
            chunk[i * 2] = (int16_t)(sHist[k * 2] * fade >> 8);
            chunk[i * 2 + 1] = (int16_t)(sHist[k * 2 + 1] * fade >> 8);
        }
        return;
    }
    uint32_t rd = sRingRead;
    uint32_t pos = *phase;
    for (uint32_t i = 0; i < OUT_CHUNK; i++) {
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
        const int16_t* p0 = &sRing[((rd + idx - 1) & RING_MASK) * 2];
        const int16_t* p1 = &sRing[((rd + idx) & RING_MASK) * 2];
        const int16_t* p2 = &sRing[((rd + idx + 1) & RING_MASK) * 2];
        const int16_t* p3 = &sRing[((rd + idx + 2) & RING_MASK) * 2];
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
    uint32_t used = pos >> 16;
    sRingRead = rd + used;
    *phase = pos & 0xFFFF;
    sConsumed += used;
    memcpy(sHist, chunk + (OUT_CHUNK - CONCEAL_FRAMES) * 2, sizeof(sHist));
    sHistFill = CONCEAL_FRAMES;
}

/* A chunk straight from the ring, for the firmware's rate converter (src32.txt). */
static void fill_direct(int16_t* chunk) {
    uint32_t avail = ring_fill();
    uint32_t take = avail < OUT_CHUNK ? avail : OUT_CHUNK;
    uint32_t rd = sRingRead;
    for (uint32_t i = 0; i < take; i++) {
        uint32_t k = (rd + i) & RING_MASK;
        chunk[i * 2] = sRing[k * 2];
        chunk[i * 2 + 1] = sRing[k * 2 + 1];
    }
    sRingRead = rd + take;
    if (take < OUT_CHUNK) {
        /* Underrun: loop what was just played (the ring still holds it), fading out. */
        note_underrun();
        uint32_t played = rd + take;
        uint32_t loop = played < CONCEAL_FRAMES ? played : CONCEAL_FRAMES;
        for (uint32_t i = take; i < OUT_CHUNK; i++) {
            if (loop == 0) {
                chunk[i * 2] = chunk[i * 2 + 1] = 0;
                continue;
            }
            uint32_t k = (played - loop + ((i - take) % loop)) & RING_MASK;
            int32_t fade = 256 - (int32_t)((i - take) >> 1);
            if (fade < 0) {
                fade = 0;
            }
            chunk[i * 2] = (int16_t)(sRing[k * 2] * fade >> 8);
            chunk[i * 2 + 1] = (int16_t)(sRing[k * 2 + 1] * fade >> 8);
        }
    }
    sConsumed += OUT_CHUNK;
}

/* The firmware's SRC channel takes these rates only; the closest to the game's. */
static uint32_t nearest_src_rate(uint32_t freq) {
    static const uint32_t kRates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    uint32_t rate = kRates[0];
    for (size_t i = 0; i < RT_COUNT(kRates); i++) {
        uint32_t d_best = rate > freq ? rate - freq : freq - rate;
        uint32_t d = kRates[i] > freq ? kRates[i] - freq : freq - kRates[i];
        if (d < d_best) {
            rate = kRates[i];
        }
    }
    return rate;
}

/*
 * Plays the ring. By default the output runs at the DAC's own 44.1 kHz,
 * resampled here, which keeps the firmware's rate converter out of the path;
 * src32.txt hands the firmware the game's rate instead, for comparisons.
 */
static int output_thread(SceSize args, void* argp) {
    sceKernelWaitSema(sFreqSignal, 1, NULL);
    uint32_t freq = sFrequency;
    bool resample = !rt_data_file_exists("src32.txt");
    uint32_t rate = resample ? OUT_RATE_44K : nearest_src_rate(freq);
    int ret = sceAudioSRCChReserve(OUT_CHUNK, (int)rate, 2);
    rt_log("audio: output %u Hz (game %u Hz), reserve %08X%s", rate, freq, (unsigned)ret,
           resample ? " [resampled here]" : " [firmware SRC, src32.txt]");
    if (ret < 0) {
        return 0;
    }
    uint32_t step = (uint32_t)(((uint64_t)freq << 16) / OUT_RATE_44K);
    uint32_t phase = 0;
    sChunkUs = (uint32_t)((uint64_t)OUT_CHUNK * 1000000 / rate);
    output_wav_open(rate);

    bool priming = true;
    uint32_t prime_start = sceKernelGetSystemTimeLow();
    for (;;) {
        int16_t* chunk = sOutBuf[sOutIndex];
        if (priming) {
            uint32_t avail = ring_fill();
            if (avail >= PRIME_FRAMES) {
                priming = false;
                rt_log("audio: primed with %u frames (%u ms)", (unsigned)avail, (unsigned)(avail * 1000 / rate));
            } else if (sceKernelGetSystemTimeLow() - prime_start > PRIME_TIMEOUT_US) {
                priming = false;
                rt_log("audio: priming gave up at %u frames", (unsigned)avail);
            } else {
                /* Silence while the ring fills; not an underrun. */
                memset(chunk, 0, OUT_CHUNK * 4);
                output_chunk(chunk);
                continue;
            }
        }
        if (resample) {
            fill_resampled(chunk, step, &phase);
        } else {
            fill_direct(chunk);
        }
        output_wav_write(chunk, OUT_CHUNK, rate);
        output_chunk(chunk);
    }
    return 0;
}

void rt_audio_init(void) {
    sFreqSignal = sceKernelCreateSema("rt_audio_freq", 0, 0, 1, NULL);
    SceUID thid = sceKernelCreateThread("rt_audio", output_thread, 0x12, 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    sceKernelStartThread(thid, 0, NULL);
}

/* ---- tasks -------------------------------------------------------------- */

/*
 * Spacing of audio task dispatches: one per retrace is 16.7 ms. The game's
 * audio manager discards retraces that pile up while it is busy, so every
 * gap past ~25 ms is a buffer of sound it never makes.
 */
static uint32_t sPrevTaskAt = 0;
static uint32_t sGapHist[4];   /* <25 ms, <42 ms, <59 ms, longer */
static uint32_t sGapMax = 0;

/*
 * Where an audio cycle's time goes. The game's audio manager (audioMgr.c) gets
 * a retrace from irqmgr, hands last cycle's task to the scheduler, builds the
 * next one and waits for the first to finish. "react" is retrace-to-handover,
 * "dispatch" is handover-to-RSP.
 */
#define AUDIOMGR_RETRACE_MQ 0x80144D8Cu /* audiomgr_class.interruptQueue */
#define SCHED_CMD_MQ 0x80145DB8u        /* the scheduler's task queue */
#define AUDIOMGR_TASK 0x80144D30u       /* audiomgr_class.audioTask */
static uint32_t sRetraceAt = 0, sHandoverAt = 0;
static uint32_t sReactMax = 0, sReactSum = 0, sReactN = 0;
static uint32_t sDispatchMax = 0, sDispatchSum = 0, sDispatchN = 0;

void rt_audio_note_send(uint32_t mq, uint32_t msg) {
    if (mq == AUDIOMGR_RETRACE_MQ) {
        if (sRetraceAt == 0) {
            sRetraceAt = sceKernelGetSystemTimeLow(); /* the first one of a backlog is the one that counts */
        }
    } else if (mq == SCHED_CMD_MQ && msg == AUDIOMGR_TASK) {
        uint32_t now = sceKernelGetSystemTimeLow();
        if (sRetraceAt != 0) {
            uint32_t d = now - sRetraceAt;
            sReactSum += d;
            sReactN++;
            if (d > sReactMax) {
                sReactMax = d;
            }
            sRetraceAt = 0;
        }
        sHandoverAt = now;
    }
}

static void note_task_timing(uint32_t now) {
    if (sHandoverAt != 0) {
        uint32_t d = now - sHandoverAt;
        sDispatchSum += d;
        sDispatchN++;
        if (d > sDispatchMax) {
            sDispatchMax = d;
        }
        sHandoverAt = 0;
    }
    if (sPrevTaskAt != 0) {
        uint32_t gap = now - sPrevTaskAt;
        sGapHist[gap < 25000 ? 0 : gap < 42000 ? 1 : gap < 59000 ? 2 : 3]++;
        if (gap > sGapMax) {
            sGapMax = gap;
        }
    }
    sPrevTaskAt = now;
}

/* Every 600 tasks (10 s): the rates, the ring, the hardware, and the audio cycle's pacing. */
static void log_stats(void) {
    uint32_t now = (uint32_t)(sceKernelGetSystemTimeWide() / 1000);
    if (sRateMark != 0 && now > sRateMark) {
        uint32_t span = now - sRateMark;
        rt_log("audio rate: game %u Hz, hardware %u Hz over %u ms", (unsigned)((uint64_t)sProduced * 1000 / span),
               (unsigned)((uint64_t)sConsumed * 1000 / span), (unsigned)span);
    }
    sRateMark = now;
    sProduced = sConsumed = 0;

    char me[32] = "";
    char dma[96] = "";
    rt_me_audio_report(me, sizeof(me));
    rt_dma_report(dma, sizeof(dma));
    rt_log("audio: %u tasks, fill %u, underruns %u, overflows %u, bad opcodes %u, rom blocks read %u (%u ms)%s cpu %u, retraces lost %u | hw %u late/%u chunks, worst gap %u us (chunk %u us) | %s",
           (unsigned)sTaskCount, (unsigned)ring_fill(), (unsigned)sUnderruns, (unsigned)sOverflows,
           (unsigned)asp_opcode_counts[31], (unsigned)rt_rom_block_loads(), (unsigned)(rt_rom_block_us() / 1000), me,
           (unsigned)sCpuTasks, (unsigned)rt_sched_vi_dropped(), (unsigned)sHwLate, (unsigned)sHwChunks,
           (unsigned)sHwWorstGapUs, (unsigned)sChunkUs, dma);
    sCpuTasks = 0;
    sHwLate = 0;
    sHwChunks = 0;
    sHwWorstGapUs = 0;

    char vi[80];
    rt_sched_vi_report(vi, sizeof(vi));
    rt_log("audio pacing: task gaps <25ms %u, <42 %u, <59 %u, longer %u, max %u us | %s | react avg %u max %u, dispatch avg %u max %u us",
           (unsigned)sGapHist[0], (unsigned)sGapHist[1], (unsigned)sGapHist[2], (unsigned)sGapHist[3],
           (unsigned)sGapMax, vi, (unsigned)(sReactN ? sReactSum / sReactN : 0), (unsigned)sReactMax,
           (unsigned)(sDispatchN ? sDispatchSum / sDispatchN : 0), (unsigned)sDispatchMax);
    sReactMax = sReactSum = sReactN = sDispatchMax = sDispatchSum = sDispatchN = 0;
    memset(sGapHist, 0, sizeof(sGapHist));
    sGapMax = 0;
}

void rt_audio_run_task(uint32_t task) {
    uint64_t t0 = sceKernelGetSystemTimeWide();
    note_task_timing((uint32_t)t0);
    sTaskCount++;
    maybe_dump_task(sTaskCount, task);

    AspTask t = {
        .ucode_data = rd_w32(task + 0x18),
        .ucode_data_size = rd_w32(task + 0x1C),
        .dram_stack = rd_w32(task + 0x20),
        .data_ptr = rd_w32(task + 0x30),
        .data_size = rd_w32(task + 0x34),
    };
    /*
     * Both cores run these. The Media Engine takes the task unless it is still
     * working on the previous one -- in a busy village a task can outlast the
     * 16.6 ms between them -- in which case the main CPU runs this one rather
     * than letting the game wait, which is what audio underruns are made of.
     * The cores share the microcode's state (g_asp), so whoever runs a task
     * must see what the other one left behind.
     */
    bool on_me = rt_me_audio_ready() && !rt_me_audio_busy();
    if (on_me) {
        rt_me_audio_submit(&t);
        on_me = rt_me_audio_ready();
        sLastOnMe = true;
    }
    if (!on_me) {
        if (sLastOnMe) {
            sceKernelDcacheInvalidateRange(&g_asp, sizeof(g_asp));
            sLastOnMe = false;
        }
        asp_run_task(g_rdram, &t);
        /* The samples are read uncached, so they have to be in memory. */
        sceKernelDcacheWritebackAll();
        g_audio_us += sceKernelGetSystemTimeWide() - t0;
        sCpuTasks++;
    }

    if ((sTaskCount % 600) == 0) {
        log_stats();
    }
}

/* ---- AI ----------------------------------------------------------------- */

static void set_frequency(uint32_t freq) {
    if (sFrequency == 0 && freq != 0) {
        sFrequency = freq;
        sceKernelSignalSema(sFreqSignal, 1);
    }
}

/* s32 func_800EFD40_jp(void* buffer, u32 size): osAiSetNextBuffer, inlined by AF */
void func_800EFD40_jp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t addr = ctx->r4 & ~3u;
    uint32_t frames = ctx->r5 / 4;
    /* Waits if the Media Engine could still be filling this buffer. */
    rt_me_audio_before_read(addr);
    uint32_t room = RING_FRAMES - 1 - ring_fill();
    if (frames > room) {
        sOverflows++;
        frames = room;
    }
    /* The ME writes straight to memory, so read the samples uncached. */
    const volatile uint32_t* me_src = rt_me_audio_ready()
            ? (const volatile uint32_t*)(0x40000000u | (uintptr_t)(g_rdram + (addr & RDRAM_MASK)))
            : NULL;
    uint32_t wr = sRingWrite;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t w = me_src != NULL ? me_src[i] : rd_w32(addr + 4 * i);
        uint32_t k = (wr + i) & RING_MASK;
        sRing[k * 2] = (int16_t)(w >> 16);
        sRing[k * 2 + 1] = (int16_t)w;
    }
    sRingWrite = wr + frames;
    sProduced += frames;
    /* The samples as produced, for comparing runs (see raw_record). */
    uint32_t head = RING_FRAMES - (wr & RING_MASK);
    if (head > frames) {
        head = frames;
    }
    raw_record(&sRing[(wr & RING_MASK) * 2], head, frames, sFrequency);
    if (head < frames) {
        raw_record(&sRing[0], frames - head, 0, sFrequency);
    }
    sSubmitted++;
    ctx->r2 = 0;
}

/* s32 osAiSetFrequency(u32 frequency) */
void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t freq = ctx->r4;
    if (freq == 0) {
        ctx->r2 = (uint32_t)-1;
        return;
    }
    uint32_t dac_rate = (uint32_t)((float)VI_NTSC_CLOCK / (float)freq + 0.5f);
    if (dac_rate < 132) {
        ctx->r2 = (uint32_t)-1;
        return;
    }
    ctx->r2 = VI_NTSC_CLOCK / dac_rate;
    rt_log("osAiSetFrequency(%u) -> %u", (unsigned)freq, (unsigned)ctx->r2);
    set_frequency(ctx->r2);
}

/* u32 osAiGetLength(void): bytes left to play */
void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t fill = ring_fill();
    ctx->r2 = fill > LENGTH_BIAS ? (fill - LENGTH_BIAS) * 4 : 0;
}

RT_STUB_RETURN(osAiGetStatus_recomp, 0)
