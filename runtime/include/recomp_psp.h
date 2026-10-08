/*
 * recomp_psp.h -- macros and types used by N64Recomp output on PSP.
 *
 * This replaces N64Recomp's recomp.h for the PSP fork. Differences:
 *   - Registers are 32 bits wide. Animal Forest's game code is MIPS II, and
 *     every function that uses 64-bit instructions is reimplemented by the
 *     runtime, so the upper halves would only ever hold sign extension.
 *   - RDRAM is a 4MB word-swapped buffer (Animal Forest limits itself to
 *     4MB, and its few direct hardware accesses are patched in
 *     recomp/af.jp.toml): each 32-bit word is stored in host (little-endian)
 *     order, so word loads need no byteswap and byte/half accesses use the
 *     address ^ 3 / ^ 2 trick. Addresses are masked to the buffer, which folds
 *     KSEG0/KSEG1/physical aliases together.
 *   - Division is emitted as DIV32 so it stays 32-bit on the target.
 */
#ifndef RECOMP_PSP_H
#define RECOMP_PSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t gpr;

#define RECOMP_FUNC
#define RECOMP_RESTRICT restrict
#define SET_FENV_ACCESS()

/*
 * Every recompiled function starts with this (scripts/recompile.sh puts it
 * there). Game code otherwise only reaches the runtime at OS calls and at
 * indirect calls, which leaves stretches -- loading a village is about 800 ms
 * -- where nothing else can run, and the game's own audio manager misses its
 * retrace. This costs a byte load and a branch not taken.
 */
extern volatile uint8_t g_preempt_hint;
void rt_preempt(void);
#define RECOMP_PREEMPT()      \
    do {                      \
        if (g_preempt_hint) { \
            rt_preempt();     \
        }                     \
    } while (0)

#define RDRAM_SIZE 0x00400000u
#define RDRAM_MASK (RDRAM_SIZE - 1)
/* Spare memory on each side of the RDRAM buffer (see MEM_PTR below). */
#define RDRAM_GUARD 0x10000u

#define SIGNED(val) ((int32_t)(val))
#define S32(val) ((int32_t)(val))
#define U32(val) ((uint32_t)(val))
#define S64(val) ((int64_t)(val))
#define U64(val) ((uint64_t)(val))

#define ADD32(a, b) ((gpr)((a) + (b)))
#define SUB32(a, b) ((gpr)((a) - (b)))

/* An address folded into RDRAM: KSEG0, KSEG1 and physical addresses all land on the same byte. */
#define MEM_OFFSET(offset, reg) (((uint32_t)((reg) + (offset))) & RDRAM_MASK)

/*
 * Is this operand of a generated access the displacement? A constant alone
 * doesn't say so: the compiler also knows the value of a base register that
 * was just loaded with an absolute address (lui).
 */
#define MEM_IS_DISP(x) (__builtin_constant_p(x) && (int32_t)(x) == (int16_t)(x))

/* misc.c: the address folded the plain way, for the checking build below. */
uint8_t* recomp_mem_check(uint8_t* rdram, uint32_t base, int32_t disp, const char* func);

/* What the generated code folds a base register with (see MEM_PTR below). */
#define RDRAM_BASE_MASK (2 * RDRAM_SIZE - 1)

#if defined(RECOMP_GENERATED_CODE) && defined(RECOMP_MEM_CHECK)
/* gmake MEMCHECK=1: every access is folded the plain way, and one that MEM_PTR
 * below would have put somewhere else is logged. */
#define MEM_PTR(a, b)                                                                    \
    (MEM_IS_DISP(a)   ? recomp_mem_check(rdram, (uint32_t)(b), (int32_t)(a), __func__) \
     : MEM_IS_DISP(b) ? recomp_mem_check(rdram, (uint32_t)(a), (int32_t)(b), __func__) \
                      : rdram + MEM_OFFSET(a, b))
#elif defined(RECOMP_GENERATED_CODE)
/*
 * Generated code reaches memory through a base register and a constant 16-bit
 * displacement (the recompiler emits them in either order). Folding their sum
 * into RDRAM takes an add, a mask and an add for every access. Folding the
 * base alone and adding the displacement to the host pointer lets the compiler
 * fold each register value once and reach everything off it -- the registers
 * saved in a stack frame, the fields of a struct -- with one load or store
 * each, as the N64's own code does. The generated code is 16% smaller for it.
 *
 * The result is the same byte wherever base and base + displacement lie in
 * the same copy of RDRAM, which is every access a 4 MB N64 can make. Two
 * things keep the rest harmless:
 *   - The fold keeps one bit more than RDRAM_MASK, so a pointer to the end of
 *     memory (0x80400000) stays at the end of the buffer, and the negative
 *     displacements code uses with such end pointers reach the last bytes of
 *     RDRAM.
 *   - Displacements can step up to 32 KB outside RDRAM (a read past the last
 *     row of a framebuffer at the top of memory). The buffer has RDRAM_GUARD
 *     bytes of zeros on each side for that: such reads see nothing and such
 *     writes are lost, as on a console without the Expansion Pak.
 * A base register that points beyond the end of memory -- 0x80410000 and up,
 * which no access on a 4 MB console can use -- is not folded back into RDRAM.
 * A displacement that isn't a constant (RELOC_LO16 of an overlay symbol) is
 * folded with its base as before.
 */
#define MEM_PTR(a, b)                                                              \
    (MEM_IS_DISP(a)   ? (rdram + ((uint32_t)(b) & RDRAM_BASE_MASK)) + (int32_t)(a) \
     : MEM_IS_DISP(b) ? (rdram + ((uint32_t)(a) & RDRAM_BASE_MASK)) + (int32_t)(b) \
                      : rdram + MEM_OFFSET(a, b))
#else
/* The runtime's own accesses: the address is folded as a whole. */
#define MEM_PTR(a, b) (rdram + MEM_OFFSET(a, b))
#endif

/*
 * RDRAM holds 32-bit words in host byte order, so a halfword or a byte is at
 * its big-endian address with the low bits flipped. The buffer is word-aligned,
 * so flipping them in the host pointer is the same thing.
 */
#define MEM_W(offset, reg)  (*(int32_t*)MEM_PTR(offset, reg))
#define MEM_WU(offset, reg) (*(uint32_t*)MEM_PTR(offset, reg))
#define MEM_H(offset, reg)  (*(int16_t*)((uintptr_t)MEM_PTR(offset, reg) ^ 2))
#define MEM_HU(offset, reg) (*(uint16_t*)((uintptr_t)MEM_PTR(offset, reg) ^ 2))
#define MEM_B(offset, reg)  (*(int8_t*)((uintptr_t)MEM_PTR(offset, reg) ^ 3))
#define MEM_BU(offset, reg) (*(uint8_t*)((uintptr_t)MEM_PTR(offset, reg) ^ 3))

/* 64-bit stores/loads are only emitted for sdc1/ldc1 (doubles). */
#define SD(val, offset, reg) {                                           \
    uint64_t sd_value__ = (uint64_t)(val);                               \
    MEM_W((offset), (reg)) = (int32_t)(uint32_t)(sd_value__ >> 32);      \
    MEM_W((offset) + 4, (reg)) = (int32_t)(uint32_t)sd_value__;          \
}

static inline uint64_t load_doubleword(uint8_t* rdram, gpr offset, gpr reg) {
    uint64_t hi = (uint32_t)MEM_W(offset, reg);
    uint64_t lo = (uint32_t)MEM_W(offset + 4, reg);
    return (hi << 32) | lo;
}

#define LD(offset, reg) load_doubleword(rdram, offset, reg)

static inline gpr do_lwl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    uint32_t loaded_value = (uint32_t)MEM_W(0, address & ~3u);
    uint32_t misalignment = address & 3;
    uint32_t masked_value = initial_value & ~(0xFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return masked_value | loaded_value;
}

static inline gpr do_lwr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = offset + reg;
    uint32_t loaded_value = (uint32_t)MEM_W(0, address & ~3u);
    uint32_t misalignment = address & 3;
    uint32_t masked_value = initial_value & ~(0xFFFFFFFFu >> (24 - misalignment * 8));
    loaded_value >>= (24 - misalignment * 8);
    return masked_value | loaded_value;
}

static inline void do_swl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    uint32_t word_address = address & ~3u;
    uint32_t initial_value = (uint32_t)MEM_W(0, word_address);
    uint32_t misalignment = address & 3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu >> (misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) >> (misalignment * 8);
    MEM_W(0, word_address) = (int32_t)(masked_initial_value | shifted_input_value);
}

static inline void do_swr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = offset + reg;
    uint32_t word_address = address & ~3u;
    uint32_t initial_value = (uint32_t)MEM_W(0, word_address);
    uint32_t misalignment = address & 3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu << (24 - misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) << (24 - misalignment * 8);
    MEM_W(0, word_address) = (int32_t)(masked_initial_value | shifted_input_value);
}

/* Signed 32-bit division with the VR4300's results for the undefined cases. */
#define DIV32(a, b, lo_out, hi_out) do {                                 \
    int32_t div_a__ = (a);                                               \
    int32_t div_b__ = (b);                                               \
    if (div_b__ == 0) {                                                  \
        (lo_out) = (gpr)(div_a__ < 0 ? 1 : -1);                          \
        (hi_out) = (gpr)div_a__;                                         \
    } else if (div_a__ == INT32_MIN && div_b__ == -1) {                  \
        (lo_out) = (gpr)div_a__;                                         \
        (hi_out) = 0;                                                    \
    } else {                                                             \
        (lo_out) = (gpr)(div_a__ / div_b__);                             \
        (hi_out) = (gpr)(div_a__ % div_b__);                             \
    }                                                                    \
} while (0)

/* Only used by 64-bit instructions, which the recompiled set doesn't contain. */
#define DMULT(a, b, lo, hi) recomp_unsupported_64bit(__func__)
#define DMULTU(a, b, lo, hi) recomp_unsupported_64bit(__func__)
#define DDIV(a, b, lo, hi) recomp_unsupported_64bit(__func__)
#define DDIVU(a, b, lo, hi) recomp_unsupported_64bit(__func__)

/* FPU rounding mode (cop1 control register bits 0-1), tracked in software. */
extern uint32_t recomp_fpu_round_mode;

static inline uint32_t get_cop1_cs(void) {
    return recomp_fpu_round_mode;
}

static inline void set_cop1_cs(uint32_t val) {
    recomp_fpu_round_mode = val & 3;
}

#define MUL_S(val1, val2) ((val1) * (val2))
#define MUL_D(val1, val2) ((val1) * (val2))
#define DIV_S(val1, val2) ((val1) / (val2))
#define DIV_D(val1, val2) ((val1) / (val2))

#define CVT_S_W(val) ((float)((int32_t)(val)))
#define CVT_D_W(val) ((double)((int32_t)(val)))
#define CVT_D_L(val) ((double)((int64_t)(val)))
#define CVT_S_L(val) ((float)((int64_t)(val)))
#define CVT_D_S(val) ((double)(val))
#define CVT_S_D(val) ((float)(val))

#define TRUNC_W_S(val) ((int32_t)(val))
#define TRUNC_W_D(val) ((int32_t)(val))
#define TRUNC_L_S(val) ((int64_t)(val))
#define TRUNC_L_D(val) ((int64_t)(val))

#define DEFAULT_ROUNDING_MODE 0

static inline int32_t do_cvt_w_s(float val) {
    switch (recomp_fpu_round_mode) {
        default:
        case 0: {
            /* Round half to even, like the FPU's default mode. */
            float rounded = floorf(val + 0.5f);
            if (rounded - val == 0.5f && ((int32_t)rounded & 1)) {
                rounded -= 1.0f;
            }
            return (int32_t)rounded;
        }
        case 1:
            return (int32_t)val;
        case 2:
            return (int32_t)ceilf(val);
        case 3:
            return (int32_t)floorf(val);
    }
}

static inline int32_t do_cvt_w_d(double val) {
    switch (recomp_fpu_round_mode) {
        default:
        case 0: {
            double rounded = floor(val + 0.5);
            if (rounded - val == 0.5 && ((int32_t)rounded & 1)) {
                rounded -= 1.0;
            }
            return (int32_t)rounded;
        }
        case 1:
            return (int32_t)val;
        case 2:
            return (int32_t)ceil(val);
        case 3:
            return (int32_t)floor(val);
    }
}

#define CVT_W_S(val) do_cvt_w_s(val)
#define CVT_W_D(val) do_cvt_w_d(val)
#define CVT_L_S(val) ((int64_t)do_cvt_w_s(val))
#define CVT_L_D(val) ((int64_t)do_cvt_w_d(val))

#define NAN_CHECK(val)
#define CHECK_FR(ctx, idx)

typedef union {
    double d;
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;

typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
    fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
        f8,  f9,  f10, f11, f12, f13, f14, f15,
        f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31;
    gpr hi, lo;
    uint32_t* f_odd;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
} recomp_context;

/*
 * Generated with register_locals (recomp/af.jp.toml): each function keeps the
 * GPRs in locals and relies on the MIPS calling convention -- a callee reads
 * only a0-a3 and sp, returns only v0/v1, and restores s0-s8 and sp itself --
 * so the context sees just those at calls and returns. Hooks, syscalls and
 * events get the whole register file. Nothing changes for code that follows
 * the convention; the compiler keeps the rest in registers.
 */
#define RL_DECLARE() gpr r1 = ctx->r1, r2 = ctx->r2, r3 = ctx->r3, r4 = ctx->r4, r5 = ctx->r5, r6 = ctx->r6, r7 = ctx->r7, r8 = ctx->r8, r9 = ctx->r9, r10 = ctx->r10, r11 = ctx->r11, r12 = ctx->r12, r13 = ctx->r13, r14 = ctx->r14, r15 = ctx->r15, r16 = ctx->r16, r17 = ctx->r17, r18 = ctx->r18, r19 = ctx->r19, r20 = ctx->r20, r21 = ctx->r21, r22 = ctx->r22, r23 = ctx->r23, r24 = ctx->r24, r25 = ctx->r25, r26 = ctx->r26, r27 = ctx->r27, r28 = ctx->r28, r29 = ctx->r29, r30 = ctx->r30, r31 = ctx->r31
#define RL_FLUSH_ARGS() do { ctx->r4 = r4; ctx->r5 = r5; ctx->r6 = r6; ctx->r7 = r7; ctx->r29 = r29; } while (0)
#define RL_RELOAD_RET() do { r2 = ctx->r2; r3 = ctx->r3; } while (0)
#define RL_WRITE_RET() do { ctx->r2 = r2; ctx->r3 = r3; } while (0)
#define RL_FLUSH_ALL() do { ctx->r1 = r1; ctx->r2 = r2; ctx->r3 = r3; ctx->r4 = r4; ctx->r5 = r5; ctx->r6 = r6; ctx->r7 = r7; ctx->r8 = r8; ctx->r9 = r9; ctx->r10 = r10; ctx->r11 = r11; ctx->r12 = r12; ctx->r13 = r13; ctx->r14 = r14; ctx->r15 = r15; ctx->r16 = r16; ctx->r17 = r17; ctx->r18 = r18; ctx->r19 = r19; ctx->r20 = r20; ctx->r21 = r21; ctx->r22 = r22; ctx->r23 = r23; ctx->r24 = r24; ctx->r25 = r25; ctx->r26 = r26; ctx->r27 = r27; ctx->r28 = r28; ctx->r29 = r29; ctx->r30 = r30; ctx->r31 = r31; } while (0)
#define RL_RELOAD_ALL() do { r1 = ctx->r1; r2 = ctx->r2; r3 = ctx->r3; r4 = ctx->r4; r5 = ctx->r5; r6 = ctx->r6; r7 = ctx->r7; r8 = ctx->r8; r9 = ctx->r9; r10 = ctx->r10; r11 = ctx->r11; r12 = ctx->r12; r13 = ctx->r13; r14 = ctx->r14; r15 = ctx->r15; r16 = ctx->r16; r17 = ctx->r17; r18 = ctx->r18; r19 = ctx->r19; r20 = ctx->r20; r21 = ctx->r21; r22 = ctx->r22; r23 = ctx->r23; r24 = ctx->r24; r25 = ctx->r25; r26 = ctx->r26; r27 = ctx->r27; r28 = ctx->r28; r29 = ctx->r29; r30 = ctx->r30; r31 = ctx->r31; } while (0)

typedef void (recomp_func_t)(uint8_t* rdram, recomp_context* ctx);

/* Called by the generated code; implemented in runtime/src (misc.c, sched.c, sections.c). */
void cop0_status_write(recomp_context* ctx, gpr value);
gpr cop0_status_read(recomp_context* ctx);
void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);
void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram);
void pause_self(uint8_t* rdram);
void recomp_unsupported_64bit(const char* func);
void recomp_unrecompiled(uint8_t* rdram, recomp_context* ctx, const char* name, uint32_t vram);
void recomp_overlay_load_hook(uint8_t* rdram, recomp_context* ctx);

/*
 * [[patches.hook]]s in recomp/af.jp.toml: runtime functions spliced into
 * recompiled game functions. A hook returning bool that returns true has done
 * the function's work, and the game's own code is skipped.
 */
/* runtime/src/english/: the game in English (see english.h there) */
bool rt_en_message(uint8_t* rdram, recomp_context* ctx);
bool rt_en_char_width(uint8_t* rdram, recomp_context* ctx);
bool rt_en_glyph(uint8_t* rdram, recomp_context* ctx);
bool rt_en_year(uint8_t* rdram, recomp_context* ctx);
bool rt_en_month(uint8_t* rdram, recomp_context* ctx);
bool rt_en_day(uint8_t* rdram, recomp_context* ctx);
bool rt_en_weekday(uint8_t* rdram, recomp_context* ctx);
bool rt_en_hour(uint8_t* rdram, recomp_context* ctx);
bool rt_en_minute(uint8_t* rdram, recomp_context* ctx);
bool rt_en_second(uint8_t* rdram, recomp_context* ctx);
bool rt_en_villager_name(uint8_t* rdram, recomp_context* ctx);
bool rt_en_choice(uint8_t* rdram, recomp_context* ctx);
bool rt_en_string(uint8_t* rdram, recomp_context* ctx);
bool rt_en_item_name(uint8_t* rdram, recomp_context* ctx);
bool rt_en_draw_line(uint8_t* rdram, recomp_context* ctx);
bool rt_en_string_width(uint8_t* rdram, recomp_context* ctx);
void rt_en_msg_length(uint8_t* rdram, recomp_context* ctx, uint32_t src);
void rt_en_msg_catchphrase(uint8_t* rdram, recomp_context* ctx, uint32_t src);
void rt_en_msg_determination(uint8_t* rdram, recomp_context* ctx, uint32_t src);
bool rt_en_msg_copy(uint8_t* rdram, recomp_context* ctx);
bool rt_en_letter(uint8_t* rdram, recomp_context* ctx);
bool rt_en_letter2(uint8_t* rdram, recomp_context* ctx);
bool rt_en_villager_letter(uint8_t* rdram, recomp_context* ctx);
void rt_en_letter_footer(uint8_t* rdram, recomp_context* ctx);
bool rt_en_letter_tag(uint8_t* rdram, recomp_context* ctx);
bool rt_en_free_string(uint8_t* rdram, recomp_context* ctx);
void rt_en_keyboard_page(uint8_t* rdram, recomp_context* ctx);
void rt_en_typed_space(uint8_t* rdram, recomp_context* ctx);
void rt_en_typed_cursor(uint8_t* rdram, recomp_context* ctx);
void rt_en_board_cursor(uint8_t* rdram, recomp_context* ctx);
void rt_en_board_end(uint8_t* rdram, recomp_context* ctx);
bool rt_en_resetti_match(uint8_t* rdram, recomp_context* ctx);
bool rt_en_resetti_rude(uint8_t* rdram, recomp_context* ctx);

recomp_func_t* get_function(int32_t vram);

#define LOOKUP_FUNC(val) get_function((int32_t)(val))

extern int32_t* section_addresses;

#define LO16(x) ((x) & 0xFFFF)
#define HI16(x) (((x) >> 16) + (((x) >> 15) & 1))

#define RELOC_HI16(section_index, offset) HI16((uint32_t)(section_addresses[section_index] + (offset)))
#define RELOC_LO16(section_index, offset) LO16((uint32_t)(section_addresses[section_index] + (offset)))

#ifdef __cplusplus
}
#endif

#endif
