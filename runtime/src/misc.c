/*
 * misc.c -- the rest of libultra: init, interrupts, caches, TLB, FPU control,
 * the compiler's 64-bit arithmetic helpers, and the recompiler's error hooks.
 *
 * Throughout the runtime, a function named X_recomp is libultra's X, which
 * N64Recomp leaves to the runtime (its built-in lists of reimplemented and
 * ignored functions); libultra functions without the suffix are the ones
 * recomp/af.jp.toml adds to `ignored`.
 */
#include "rt.h"


/*
 * gmake MEMCHECK=1: generated code calls this for every access with a constant
 * displacement. It folds the address the plain way and reports accesses that
 * the fast form (MEM_PTR in recomp_psp.h) would have put elsewhere: outside
 * RDRAM -- in the guard zones or beyond them -- or wrapped to its other end.
 * Each function is reported once.
 */
uint8_t* recomp_mem_check(uint8_t* rdram, uint32_t base, int32_t disp, const char* func) {
    uint32_t plain = (base + (uint32_t)disp) & RDRAM_MASK;
    int32_t fast = (int32_t)(base & (2 * RDRAM_SIZE - 1)) + disp;
    if (fast != (int32_t)plain) {
        static const char* seen[64];
        static int nseen = 0;
        static uint32_t count = 0;
        count++;
        bool known = false;
        for (int i = 0; i < nseen && !known; i++) {
            known = seen[i] == func;
        }
        if (!known && nseen < (int)RT_COUNT(seen)) {
            seen[nseen++] = func;
            bool guarded = fast >= -(int32_t)RDRAM_GUARD && fast < (int32_t)(RDRAM_SIZE + RDRAM_GUARD) - 8;
            rt_log("memcheck: %s reaches %08X%+d: RDRAM %06X, fast form %s (%u so far)", func, (unsigned)base,
                   (int)disp, (unsigned)plain, guarded ? "in a guard zone" : "OUTSIDE THE BUFFER", (unsigned)count);
        }
    }
    return rdram + plain;
}

/* ---- init / memory ------------------------------------------------------ */

RT_STUB(__osInitialize_common_recomp)
RT_STUB_RETURN(osGetMemSize_recomp, RT_OSMEMSIZE)
RT_STUB_RETURN(osAfterPreNMI, 0)

/* ---- interrupts --------------------------------------------------------- */

static uint32_t sIntMask = 0x3FFF01;

void osSetIntMask_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t prev = sIntMask;
    sIntMask = ctx->r4;
    ctx->r2 = prev;
}

/* Game threads only change hands inside OS calls (sched.c), so there is nothing to mask. */
RT_STUB_RETURN(__osDisableInt_recomp, 1)
RT_STUB(__osRestoreInt_recomp)

/* ---- CPU: caches, TLB, cop0 --------------------------------------------- */

void osVirtualToPhysical_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ctx->r4 & 0x1FFFFFFF;
}

RT_STUB(osInvalDCache_recomp)
RT_STUB(osInvalICache_recomp)
RT_STUB(osWritebackDCache_recomp)
RT_STUB(osWritebackDCacheAll_recomp)
RT_STUB(osUnmapTLBAll_recomp)
RT_STUB_RETURN(__osGetActiveQueue, 0)
RT_STUB_RETURN(__osGetCurrFaultedThread, 0)

void cop0_status_write(recomp_context* ctx, gpr value) {
    ctx->status_reg = value;
}

gpr cop0_status_read(recomp_context* ctx) {
    return ctx->status_reg;
}

/* ---- FPU control -------------------------------------------------------- */

/* The rounding mode (FCSR bits 0-1), which CVT.W honours (recomp_psp.h). */
uint32_t recomp_fpu_round_mode = 0;

void __osSetFpcCsr_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t prev = 0x01000800 | recomp_fpu_round_mode;
    recomp_fpu_round_mode = ctx->r4 & 3;
    ctx->r2 = prev;
}

void __osGetFpcCsr(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0x01000800 | recomp_fpu_round_mode;
}

/* ---- the compiler's 64-bit helpers --------------------------------------- */

/* o32: long long arguments come in register pairs, high word first. */
static inline int64_t arg64_0(recomp_context* ctx) {
    return (int64_t)(((uint64_t)ctx->r4 << 32) | ctx->r5);
}

static inline int64_t arg64_1(recomp_context* ctx) {
    return (int64_t)(((uint64_t)ctx->r6 << 32) | ctx->r7);
}

static inline void ret64(recomp_context* ctx, uint64_t v) {
    ctx->r2 = (gpr)(v >> 32);
    ctx->r3 = (gpr)v;
}

void __ll_div_recomp(uint8_t* rdram, recomp_context* ctx) {
    int64_t a = arg64_0(ctx), b = arg64_1(ctx);
    ret64(ctx, b == 0 ? 0 : (uint64_t)(a / b));
}

void __ll_mul_recomp(uint8_t* rdram, recomp_context* ctx) {
    ret64(ctx, (uint64_t)arg64_0(ctx) * (uint64_t)arg64_1(ctx));
}

void __ll_lshift_recomp(uint8_t* rdram, recomp_context* ctx) {
    ret64(ctx, (uint64_t)arg64_0(ctx) << (arg64_1(ctx) & 63));
}

void __ull_rshift_recomp(uint8_t* rdram, recomp_context* ctx) {
    ret64(ctx, (uint64_t)arg64_0(ctx) >> (arg64_1(ctx) & 63));
}

void __ull_div_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t a = (uint64_t)arg64_0(ctx), b = (uint64_t)arg64_1(ctx);
    ret64(ctx, b == 0 ? 0 : a / b);
}

void __ull_rem_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint64_t a = (uint64_t)arg64_0(ctx), b = (uint64_t)arg64_1(ctx);
    ret64(ctx, b == 0 ? a : a % b);
}

void __ull_to_d_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.d = (double)(uint64_t)arg64_0(ctx);
}

void __ull_to_f_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = (float)(uint64_t)arg64_0(ctx);
}

/* ---- recompiler hooks --------------------------------------------------- */

void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    rt_fatal("switch_error in %s at %08X (jump table %08X)", func, vram, jtbl);
}

void do_break(uint32_t vram) {
    rt_log("break instruction at %08X", vram);
}

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    rt_log("syscall at %08X ignored", (uint32_t)instruction_vram);
}

void recomp_unsupported_64bit(const char* func) {
    rt_fatal("64-bit multiply/divide reached in %s", func);
}

void recomp_unrecompiled(uint8_t* rdram, recomp_context* ctx, const char* name, uint32_t vram) {
    rt_log("unrecompiled function %s (%08X) called from %08X", name, vram, ctx->r31);
    ctx->r2 = 0;
}
