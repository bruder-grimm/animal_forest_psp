/*
 * resample.h -- the audio output's rate conversion, on the Media Engine when there is one (resample.c).
 *
 * The game's samples arrive in a ring (g_rs_ring) at its own rate; the DAC runs at 44.1 kHz. The converter
 * cuts the ring into chunks of RS_CHUNK output frames and keeps up to RS_QUEUE of them ready. The output
 * thread (audio.c) only hands the finished chunks to the hardware.
 */
#ifndef AFPSP_RESAMPLE_H
#define AFPSP_RESAMPLE_H

#include <stdint.h>

#define RS_CHUNK 512 /* output frames per chunk */
#define RS_RING_FRAMES 16384 /* source frames; power of two */
#define RS_RING_MASK (RS_RING_FRAMES - 1)
#define RS_QUEUE 8 /* chunks that can be ready at once; power of two */

/* The source ring, stereo s16. The game thread writes it, then calls rt_rs_publish. */
extern int16_t g_rs_ring[RS_RING_FRAMES * 2];

void rt_rs_init(void);
/* The game's rate and the output's; the converter does nothing before this. */
void rt_rs_start(uint32_t src_hz, uint32_t out_hz);

/* Source frames published so far (a running count; the ring slot is count & RS_RING_MASK). */
uint32_t rt_rs_written(void);
/* Source frames in the ring the converter has not yet used up. */
uint32_t rt_rs_unread(void);
/* The game thread has written frames [first, first + n) of the ring. */
void rt_rs_publish(uint32_t first, uint32_t n);

/* Output thread: puts the converter on the right core. Call once per chunk. */
void rt_rs_sync(void);
/*
 * Chunk number n (counting from 0) of the output, or NULL if there is none ready (an underrun). *src_end is the
 * source position (frames) after the chunk. The buffer stays valid until rt_rs_release(n + 1) or later.
 */
const int16_t* rt_rs_get_chunk(uint32_t n, uint32_t* src_end);
/* The hardware is done with chunks 0 .. n - 1. */
void rt_rs_release(uint32_t n);
/* Chunk n's buffer, for reading it back (the converter's cores write it, not this one's cache). */
const int16_t* rt_rs_chunk_buf(uint32_t n);

/* Media Engine: does one chunk if there is work; true if it did. */
int rt_rs_me_poll(void);

void rt_rs_report(char* buf, int size);

#endif
