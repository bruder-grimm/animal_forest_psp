/*
 * aspmain.h -- HLE of Animal Forest's audio microcode.
 *
 * Plain C with no PSP dependencies so a test harness on the host
 * can check it against the recompiled microcode.
 */
#ifndef AFPSP_ASPMAIN_H
#define AFPSP_ASPMAIN_H

#include <stdint.h>

typedef struct {
    uint32_t ucode_data;      /* OSTask fields, as the game wrote them */
    uint32_t ucode_data_size;
    uint32_t dram_stack;
    uint32_t data_ptr;
    uint32_t data_size;
} AspTask;

/* rdram: the word-swapped RDRAM image (see recomp_psp.h). */
void asp_run_task(uint8_t* rdram, const AspTask* task);

/* Clears state the RSP keeps between tasks (vector registers, DMEM). */
void asp_reset(void);

/*
 * State a task writes, kept together so it can own whole cache lines (see
 * aspmain.c). dm is the RSP's data memory, the rest is register state that
 * carries between commands; out_buffer is the sample buffer the task wrote.
 */
#define ASP_DM_WORDS 0x800
typedef struct {
    int16_t dm[ASP_DM_WORDS];
    uint8_t* rdram;
    int16_t env_vol[8];   /* v1 during ENVSETUP1/ENVSETUP2/ENVMIXER */
    uint32_t env_ramp_l;  /* r21 */
    uint32_t env_ramp_r;  /* r22 */
    uint32_t env_ramp_wet; /* r11 */
    int32_t filter_count; /* r15 */
    int16_t v31[8];       /* v31: ADDMIXER's VADDC reads it */
    uint32_t out_buffer;
    uint32_t opcode_counts[32];
    uint8_t pad[64];      /* nothing else may share the last line */
} AspState;

extern AspState g_asp;

#define asp_out_buffer (g_asp.out_buffer)
#define asp_opcode_counts (g_asp.opcode_counts)

/* Short names for the implementation only: they are ordinary words. */
#ifdef ASP_IMPLEMENTATION
#define dm (g_asp.dm)
#define sRdram (g_asp.rdram)
#define sEnvVol (g_asp.env_vol)
#define sEnvRampL (g_asp.env_ramp_l)
#define sEnvRampR (g_asp.env_ramp_r)
#define sEnvRampWet (g_asp.env_ramp_wet)
#define sFilterCount (g_asp.filter_count)
#define sV31 (g_asp.v31)
#endif

#endif
