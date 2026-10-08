/*
 * me_audio.h -- audio tasks on the PSP's Media Engine (me_audio.c).
 *
 * Without a usable Media Engine (emulators, no custom firmware, no_me.txt)
 * rt_me_audio_ready() stays false and audio.c runs every task itself.
 */
#ifndef AFPSP_ME_AUDIO_H
#define AFPSP_ME_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "aspmain.h"

void rt_me_audio_init(void);
bool rt_me_audio_ready(void);
/* True while the ME still has a task; the caller can run the next one itself. */
bool rt_me_audio_busy(void);
/* Hands a task to the ME. The game is told the RSP is done right away. */
void rt_me_audio_submit(const AspTask* task);
/* Waits unless the samples at addr are known to be finished. */
void rt_me_audio_before_read(uint32_t addr);
void rt_me_audio_report(char* buf, int size);
/* For code that runs on the ME (resample.c): its data cache, by whole lines. */
void rt_me_cache_invalidate(uint32_t addr, uint32_t size);
void rt_me_cache_writeback(uint32_t addr, uint32_t size);
/* Standby and exit (main.c). */
void rt_me_audio_resume(void);
void rt_me_audio_shutdown(void);

#endif
