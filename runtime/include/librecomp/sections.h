/*
 * Types used by N64Recomp's generated recomp_overlays.inl (normally provided
 * by N64ModernRuntime's librecomp).
 */
#ifndef LIBRECOMP_SECTIONS_H
#define LIBRECOMP_SECTIONS_H

#include <stddef.h>
#include <stdint.h>
#include "recomp_psp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

typedef enum {
    R_MIPS_NONE = 0,
    R_MIPS_16,
    R_MIPS_32,
    R_MIPS_REL32,
    R_MIPS_26,
    R_MIPS_HI16,
    R_MIPS_LO16,
    R_MIPS_GPREL16,
} RelocType;

typedef struct {
    recomp_func_t* func;
    uint32_t offset;
    uint32_t rom_size;
} FuncEntry;

typedef struct {
    uint32_t offset;
    uint32_t target_section_offset;
    uint16_t target_section;
    RelocType type;
} RelocEntry;

typedef struct {
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint32_t size;
    FuncEntry* funcs;
    size_t num_funcs;
    RelocEntry* relocs;
    size_t num_relocs;
    size_t index;
} SectionTableEntry;

#endif
