/*
 * synth.h -- the audio engine's synthesis on the Media Engine (synth.c).
 */
#ifndef AFPSP_SYNTH_H
#define AFPSP_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Before an audio task runs: finishes the synthesis that made its command
 * list (it may still be on the ME, or not started) and does the sample DMA it
 * asked for. If the task's command list is the one it made, *data_size is set
 * to its size (the game put 0 there, not knowing it yet).
 */
void rt_synth_before_task(uint32_t data_ptr, uint32_t* data_size);
/* Runs on the ME (me_audio.c): the job rt_synth_start handed over. */
void rt_synth_me_run(uint8_t* rdram);
void rt_synth_report(char* buf, int size);

#endif
