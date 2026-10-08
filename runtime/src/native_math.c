/*
 * native_math.c -- native versions of the game's hottest small functions.
 *
 * sys_matrix.c (the matrix stack), m_skin_matrix.c, m_lib.c's sin_s/cos_s,
 * libultra's sins/coss, c_keyframe.c's interpolation and m_field_info.c's
 * lookups, written in C from the decomp's sources. The game calls them
 * thousands of times a frame (every actor, every bone); recompiled, each one
 * keeps its IDO prologue, spills and register traffic through ctx, and pulls
 * its own copy of that code through the instruction cache. Here they are
 * plain C on the RDRAM words: a MtxF is sixteen host-order floats there, so
 * the code reads them as such.
 *
 * Each function does the same single-precision operations in the same order
 * as the original (IDO neither reorders nor fuses them, nor does this
 * compiler on the Allegrex), so the results are bit-identical; and loads and
 * stores go through pointers in the same order, so a destination that is also
 * a source behaves the same too.
 *
 * They replace the recompiled functions outright ("ignored" in
 * recomp/af.jp.toml; the recompiler keeps their entries in the function table,
 * pointing here, for indirect calls). Before that they were hooks with a check
 * mode that ran both versions and compared what they wrote and returned: 8.2
 * million calls through the intro, the train and the village, none different.
 */
#include "rt.h"

/* ---- RDRAM views -------------------------------------------------------- */

typedef struct {
    float xx, yx, zx, wx, xy, yy, zy, wy, xz, yz, zz, wz, xw, yw, zw, ww;
} NMtxF;

typedef struct {
    float x, y, z;
} NVec;

#define MATRIX_NOW_ADDR 0x801462B4u /* MtxF* Matrix_now (sys_matrix.c) */
#define SINTABLE_ADDR   0x8003C5F0u /* libultra's s16 sintable[0x400] */
#define SHT_MINV_ADDR   0x80117234u /* m_lib.c's 1.0f / SHT_MAX, as sin_s/cos_s load it */

static inline void* nm_ptr(uint8_t* rdram, uint32_t addr) {
    return rdram + (addr & RDRAM_MASK);
}

#define MTX(addr) ((NMtxF*)nm_ptr(rdram, (addr)))
#define VEC(addr) ((NVec*)nm_ptr(rdram, (addr)))
/* The top of the matrix stack: its address, and the matrix there. */
#define TOP_ADDR() ((uint32_t)MEM_W(0, MATRIX_NOW_ADDR))
#define TOP() MTX(TOP_ADDR())

/* A halfword at N64 address addr (RDRAM words are host order). */
static inline int16_t nm_h(uint8_t* rdram, uint32_t addr) {
    return MEM_H(0, addr);
}

static inline void nm_set_h(uint8_t* rdram, uint32_t addr, uint16_t value) {
    MEM_HU(0, addr) = value;
}

/* s_xyz: three halfwords. */
#define SX(a) nm_h(rdram, (a) + 0)
#define SY(a) nm_h(rdram, (a) + 2)
#define SZ(a) nm_h(rdram, (a) + 4)

/* ---- trigonometry ------------------------------------------------------- */

static inline int16_t trig_sins(uint8_t* rdram, uint16_t x) {
    int16_t value;
    x >>= 4;
    if (x & 0x400) {
        value = nm_h(rdram, SINTABLE_ADDR + 2 * (0x3FF - (x & 0x3FF)));
    } else {
        value = nm_h(rdram, SINTABLE_ADDR + 2 * (x & 0x3FF));
    }
    return (x & 0x800) ? (int16_t)-value : value;
}

static inline int16_t trig_coss(uint8_t* rdram, uint16_t x) {
    return trig_sins(rdram, (uint16_t)(x + 0x4000));
}

static inline float trig_sin_s(uint8_t* rdram, int16_t angle) {
    return (float)trig_sins(rdram, (uint16_t)angle) * rt_f32(MEM_W(0, SHT_MINV_ADDR));
}

static inline float trig_cos_s(uint8_t* rdram, int16_t angle) {
    return (float)trig_coss(rdram, (uint16_t)angle) * rt_f32(MEM_W(0, SHT_MINV_ADDR));
}

/* Both of an angle, the way the matrix functions take them: angle 0 is exactly (0, 1). */
static inline void trig_sin_cos(uint8_t* rdram, int16_t angle, float* sin, float* cos) {
    if (angle != 0) {
        *sin = trig_sin_s(rdram, angle);
        *cos = trig_cos_s(rdram, angle);
    } else {
        *sin = 0.0f;
        *cos = 1.0f;
    }
}

/* ---- m_skin_matrix.c ---------------------------------------------------- */

static void skin_mul_matrix(NMtxF* mfA, NMtxF* mfB, NMtxF* dest) {
    float cx, cy, cz, cw;
    float rx = mfA->xx, ry = mfA->xy, rz = mfA->xz, rw = mfA->xw;

    cx = mfB->xx; cy = mfB->yx; cz = mfB->zx; cw = mfB->wx;
    dest->xx = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xy; cy = mfB->yy; cz = mfB->zy; cw = mfB->wy;
    dest->xy = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xz; cy = mfB->yz; cz = mfB->zz; cw = mfB->wz;
    dest->xz = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xw; cy = mfB->yw; cz = mfB->zw; cw = mfB->ww;
    dest->xw = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);

    rx = mfA->yx; ry = mfA->yy; rz = mfA->yz; rw = mfA->yw;
    cx = mfB->xx; cy = mfB->yx; cz = mfB->zx; cw = mfB->wx;
    dest->yx = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xy; cy = mfB->yy; cz = mfB->zy; cw = mfB->wy;
    dest->yy = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xz; cy = mfB->yz; cz = mfB->zz; cw = mfB->wz;
    dest->yz = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xw; cy = mfB->yw; cz = mfB->zw; cw = mfB->ww;
    dest->yw = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);

    rx = mfA->zx; ry = mfA->zy; rz = mfA->zz; rw = mfA->zw;
    cx = mfB->xx; cy = mfB->yx; cz = mfB->zx; cw = mfB->wx;
    dest->zx = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xy; cy = mfB->yy; cz = mfB->zy; cw = mfB->wy;
    dest->zy = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xz; cy = mfB->yz; cz = mfB->zz; cw = mfB->wz;
    dest->zz = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xw; cy = mfB->yw; cz = mfB->zw; cw = mfB->ww;
    dest->zw = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);

    rx = mfA->wx; ry = mfA->wy; rz = mfA->wz; rw = mfA->ww;
    cx = mfB->xx; cy = mfB->yx; cz = mfB->zx; cw = mfB->wx;
    dest->wx = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xy; cy = mfB->yy; cz = mfB->zy; cw = mfB->wy;
    dest->wy = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xz; cy = mfB->yz; cz = mfB->zz; cw = mfB->wz;
    dest->wz = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
    cx = mfB->xw; cy = mfB->yw; cz = mfB->zw; cw = mfB->ww;
    dest->ww = (rx * cx) + (ry * cy) + (rz * cz) + (rw * cw);
}

static void skin_set_unit(NMtxF* mf) {
    mf->xx = 1.0f; mf->yx = 0.0f; mf->zx = 0.0f; mf->wx = 0.0f;
    mf->xy = 0.0f; mf->yy = 1.0f; mf->zy = 0.0f; mf->wy = 0.0f;
    mf->xz = 0.0f; mf->yz = 0.0f; mf->zz = 1.0f; mf->wz = 0.0f;
    mf->xw = 0.0f; mf->yw = 0.0f; mf->zw = 0.0f; mf->ww = 1.0f;
}

static void skin_copy(NMtxF* src, NMtxF* dest) {
    dest->xx = src->xx; dest->yx = src->yx; dest->zx = src->zx; dest->wx = src->wx;
    dest->xy = src->xy; dest->yy = src->yy; dest->zy = src->zy; dest->wy = src->wy;
    dest->xz = src->xz; dest->yz = src->yz; dest->zz = src->zz; dest->wz = src->wz;
    dest->xw = src->xw; dest->yw = src->yw; dest->zw = src->zw; dest->ww = src->ww;
}

static void skin_set_scale(NMtxF* mf, float x, float y, float z) {
    mf->yx = 0.0f; mf->zx = 0.0f; mf->wx = 0.0f;
    mf->xy = 0.0f; mf->zy = 0.0f; mf->wy = 0.0f;
    mf->xz = 0.0f; mf->yz = 0.0f; mf->wz = 0.0f;
    mf->xw = 0.0f; mf->yw = 0.0f; mf->zw = 0.0f;
    mf->ww = 1.0f;
    mf->xx = x;
    mf->yy = y;
    mf->zz = z;
}

static void skin_set_translate(NMtxF* mf, float x, float y, float z) {
    mf->yx = 0.0f; mf->zx = 0.0f; mf->wx = 0.0f;
    mf->xy = 0.0f; mf->zy = 0.0f; mf->wy = 0.0f;
    mf->xz = 0.0f; mf->yz = 0.0f; mf->wz = 0.0f;
    mf->xx = 1.0f; mf->yy = 1.0f; mf->zz = 1.0f; mf->ww = 1.0f;
    mf->xw = x;
    mf->yw = y;
    mf->zw = z;
}

static void skin_set_rotate_xyz(uint8_t* rdram, NMtxF* mf, int16_t x, int16_t y, int16_t z) {
    float cos, sin, xy, xz, yy, yz;
    float sinZ = trig_sin_s(rdram, z);
    float cosZ = trig_cos_s(rdram, z);

    mf->yy = cosZ;
    mf->xy = -sinZ;
    mf->wx = mf->wy = mf->wz = 0;
    mf->xw = mf->yw = mf->zw = 0;
    mf->ww = 1;

    if (y != 0) {
        sin = trig_sin_s(rdram, y);
        cos = trig_cos_s(rdram, y);
        mf->xx = cosZ * cos;
        mf->xz = cosZ * sin;
        mf->yx = sinZ * cos;
        mf->yz = sinZ * sin;
        mf->zx = -sin;
        mf->zz = cos;
    } else {
        mf->xx = cosZ;
        mf->yx = sinZ;
        mf->zx = mf->xz = mf->yz = 0;
        mf->zz = 1;
    }

    if (x != 0) {
        sin = trig_sin_s(rdram, x);
        cos = trig_cos_s(rdram, x);
        xy = mf->xy;
        xz = mf->xz;
        mf->xy = (xy * cos) + (xz * sin);
        mf->xz = (xz * cos) - (xy * sin);
        yz = mf->yz;
        yy = mf->yy;
        mf->yy = (yy * cos) + (yz * sin);
        mf->yz = (yz * cos) - (yy * sin);
        mf->zy = mf->zz * sin;
        mf->zz = mf->zz * cos;
    } else {
        mf->zy = 0;
    }
}

static void skin_set_rotate_zxy(uint8_t* rdram, NMtxF* mf, int16_t x, int16_t y, int16_t z) {
    float cos, sin, zx, zy, xx, xy;
    float sinY = trig_sin_s(rdram, y);
    float cosY = trig_cos_s(rdram, y);

    mf->xx = cosY;
    mf->zx = -sinY;
    mf->wz = 0;
    mf->wy = 0;
    mf->wx = 0;
    mf->zw = 0;
    mf->yw = 0;
    mf->xw = 0;
    mf->ww = 1;

    if (x != 0) {
        sin = trig_sin_s(rdram, x);
        cos = trig_cos_s(rdram, x);
        mf->zz = cosY * cos;
        mf->zy = cosY * sin;
        mf->xz = sinY * cos;
        mf->xy = sinY * sin;
        mf->yz = -sin;
        mf->yy = cos;
    } else {
        mf->zz = cosY;
        mf->xz = sinY;
        mf->xy = mf->zy = mf->yz = 0;
        mf->yy = 1;
    }

    if (z != 0) {
        sin = trig_sin_s(rdram, z);
        cos = trig_cos_s(rdram, z);
        xx = mf->xx;
        xy = mf->xy;
        mf->xx = (xx * cos) + (xy * sin);
        mf->xy = xy * cos - (xx * sin);
        zy = mf->zy;
        zx = mf->zx;
        mf->zx = (zx * cos) + (zy * sin);
        mf->zy = (zy * cos) - (zx * sin);
        mf->yx = mf->yy * sin;
        mf->yy = mf->yy * cos;
    } else {
        mf->yx = 0;
    }
}

/*
 * MtxF -> the RSP's fixed-point Mtx: the integer halves of all sixteen
 * elements, then the fractions. _MtxF_to_Mtx and Skin_Matrix_to_Mtx write the
 * same halfwords in the same order.
 */
static void mtxf_to_mtx(uint8_t* rdram, NMtxF* src, uint32_t dest) {
    const float* f = &src->xx;
    for (int i = 0; i < 16; i++) {
        int32_t fp = (int32_t)(f[i] * 65536.0f);
        nm_set_h(rdram, dest + 2 * i, (uint16_t)(fp >> 16));
        nm_set_h(rdram, dest + 0x20 + 2 * i, (uint16_t)fp);
    }
}

/* ---- the game's functions ------------------------------------------------ */

/*
 * Each replaces the recompiled function of its name (the recompiler leaves them
 * out: "ignored" in recomp/af.jp.toml) and reads its arguments from ctx as the
 * o32 ABI passes them: the first two float arguments in $f12/$f14 when they
 * lead the list, otherwise everything in $a0-$a3 and then the stack.
 */

/* libultra */

void sins(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)trig_sins(rdram, (uint16_t)ctx->r4);
}

void coss(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)trig_coss(rdram, (uint16_t)ctx->r4);
}

/* m_lib.c */

void sin_s(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = trig_sin_s(rdram, (int16_t)ctx->r4);
}

void cos_s(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = trig_cos_s(rdram, (int16_t)ctx->r4);
}

/* sys_matrix.c */

void Matrix_push(uint8_t* rdram, recomp_context* ctx) {
    uint32_t top = TOP_ADDR();
    NMtxF* src = MTX(top);
    NMtxF* dest = MTX(top + sizeof(NMtxF));
    *dest = *src;
    MEM_W(0, MATRIX_NOW_ADDR) = (int32_t)(top + sizeof(NMtxF));
}

void Matrix_pull(uint8_t* rdram, recomp_context* ctx) {
    MEM_W(0, MATRIX_NOW_ADDR) = (int32_t)(TOP_ADDR() - sizeof(NMtxF));
}

void Matrix_get(uint8_t* rdram, recomp_context* ctx) {
    *MTX(ctx->r4) = *TOP();
}

void Matrix_put(uint8_t* rdram, recomp_context* ctx) {
    *TOP() = *MTX(ctx->r4);
}

void Matrix_copy_MtxF(uint8_t* rdram, recomp_context* ctx) {
    *MTX(ctx->r4) = *MTX(ctx->r5);
}

void Matrix_mult(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    if ((uint8_t)ctx->r5 == 1) {
        skin_mul_matrix(top, MTX(ctx->r4), top);
    } else {
        *top = *MTX(ctx->r4);
    }
}

void Matrix_translate(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    float x = ctx->f12.fl, y = ctx->f14.fl, z = rt_f32(ctx->r6);
    float tempX, tempY;

    if ((uint8_t)ctx->r7 == 1) {
        tempX = top->xx;
        tempY = top->xy;
        top->xw += tempX * x + tempY * y + top->xz * z;
        tempX = top->yx;
        tempY = top->yy;
        top->yw += tempX * x + tempY * y + top->yz * z;
        tempX = top->zx;
        tempY = top->zy;
        top->zw += tempX * x + tempY * y + top->zz * z;
        tempX = top->wx;
        tempY = top->wy;
        top->ww += tempX * x + tempY * y + top->wz * z;
    } else {
        skin_set_translate(top, x, y, z);
    }
}

void Matrix_scale(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    float x = ctx->f12.fl, y = ctx->f14.fl, z = rt_f32(ctx->r6);

    if ((uint8_t)ctx->r7 == 1) {
        top->xx *= x;
        top->yx *= x;
        top->zx *= x;
        top->xy *= y;
        top->yy *= y;
        top->zy *= y;
        top->xz *= z;
        top->yz *= z;
        top->zz *= z;
        top->wx *= x;
        top->wy *= y;
        top->wz *= z;
    } else {
        skin_set_scale(top, x, y, z);
    }
}

/* Rotations about one axis, applied: the two columns (a, b) become a*c + b*s, b*c - a*s. */
#define ROT_AB(m, a, b, c, s)          \
    do {                               \
        float ta__ = (m)->a;           \
        float tb__ = (m)->b;           \
        (m)->a = ta__ * (c) + tb__ * (s); \
        (m)->b = tb__ * (c) - ta__ * (s); \
    } while (0)

/* ... and the Y rotation's form: a*c - b*s, a*s + b*c. */
#define ROT_Y(m, a, b, c, s)           \
    do {                               \
        float ta__ = (m)->a;           \
        float tb__ = (m)->b;           \
        (m)->a = ta__ * (c) - tb__ * (s); \
        (m)->b = ta__ * (s) + tb__ * (c); \
    } while (0)

static inline void apply_rot_x(NMtxF* top, float cos, float sin) {
    ROT_AB(top, xy, xz, cos, sin);
    ROT_AB(top, yy, yz, cos, sin);
    ROT_AB(top, zy, zz, cos, sin);
    ROT_AB(top, wy, wz, cos, sin);
}

static inline void apply_rot_y(NMtxF* top, float cos, float sin) {
    ROT_Y(top, xx, xz, cos, sin);
    ROT_Y(top, yx, yz, cos, sin);
    ROT_Y(top, zx, zz, cos, sin);
    ROT_Y(top, wx, wz, cos, sin);
}

static inline void apply_rot_z(NMtxF* top, float cos, float sin) {
    ROT_AB(top, xx, xy, cos, sin);
    ROT_AB(top, yx, yy, cos, sin);
    ROT_AB(top, zx, zy, cos, sin);
    ROT_AB(top, wx, wy, cos, sin);
}

void Matrix_RotateX(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    int16_t x = (int16_t)ctx->r4;
    float sin, cos;

    trig_sin_cos(rdram, x, &sin, &cos);
    if (ctx->r5 == 1) {
        if (x != 0) {
            apply_rot_x(top, cos, sin);
        }
    } else {
        top->yx = 0.0f; top->zx = 0.0f; top->wx = 0.0f;
        top->xy = 0.0f; top->wy = 0.0f;
        top->xz = 0.0f; top->wz = 0.0f;
        top->xw = 0.0f; top->yw = 0.0f; top->zw = 0.0f;
        top->xx = 1.0f;
        top->ww = 1.0f;
        top->yy = cos;
        top->zz = cos;
        top->zy = sin;
        top->yz = -sin;
    }
}

void Matrix_RotateY(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    int16_t y = (int16_t)ctx->r4;
    float sin, cos;

    trig_sin_cos(rdram, y, &sin, &cos);
    if (ctx->r5 == 1) {
        if (y != 0) {
            apply_rot_y(top, cos, sin);
        }
    } else {
        top->yx = 0.0f; top->wx = 0.0f;
        top->xy = 0.0f; top->zy = 0.0f; top->wy = 0.0f;
        top->yz = 0.0f; top->wz = 0.0f;
        top->xw = 0.0f; top->yw = 0.0f; top->zw = 0.0f;
        top->yy = 1.0f;
        top->ww = 1.0f;
        top->xx = cos;
        top->zz = cos;
        top->zx = -sin;
        top->xz = sin;
    }
}

void Matrix_RotateZ(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    int16_t z = (int16_t)ctx->r4;
    float sin, cos;

    trig_sin_cos(rdram, z, &sin, &cos);
    if (ctx->r5 == 1) {
        if (z != 0) {
            apply_rot_z(top, cos, sin);
        }
    } else {
        top->zx = 0.0f; top->wx = 0.0f;
        top->zy = 0.0f; top->wy = 0.0f;
        top->xz = 0.0f; top->yz = 0.0f; top->wz = 0.0f;
        top->xw = 0.0f; top->yw = 0.0f; top->zw = 0.0f;
        top->zz = 1.0f;
        top->ww = 1.0f;
        top->xx = cos;
        top->yy = cos;
        top->yx = sin;
        top->xy = -sin;
    }
}

void Matrix_rotateXYZ(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    int16_t x = (int16_t)ctx->r4, y = (int16_t)ctx->r5, z = (int16_t)ctx->r6;

    if (ctx->r7 == 1) {
        apply_rot_z(top, trig_cos_s(rdram, z), trig_sin_s(rdram, z));
        if (y != 0) {
            apply_rot_y(top, trig_cos_s(rdram, y), trig_sin_s(rdram, y));
        }
        if (x != 0) {
            apply_rot_x(top, trig_cos_s(rdram, x), trig_sin_s(rdram, x));
        }
    } else {
        skin_set_rotate_xyz(rdram, top, x, y, z);
    }
}

void Matrix_softcv3_mult(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* t = VEC(ctx->r4);
    uint32_t rot = ctx->r5;
    int16_t rz = SZ(rot);
    float sin = trig_sin_s(rdram, rz);
    float cos = trig_cos_s(rdram, rz);
    float temp1, temp2;

    /* The Z rotation interleaved with the translation, as the original does it. */
    temp1 = top->xx;
    temp2 = top->xy;
    top->xw += temp1 * t->x + temp2 * t->y + top->xz * t->z;
    top->xx = temp1 * cos + temp2 * sin;
    top->xy = temp2 * cos - temp1 * sin;

    temp1 = top->yx;
    temp2 = top->yy;
    top->yw += temp1 * t->x + temp2 * t->y + top->yz * t->z;
    top->yx = temp1 * cos + temp2 * sin;
    top->yy = temp2 * cos - temp1 * sin;

    temp1 = top->zx;
    temp2 = top->zy;
    top->zw += temp1 * t->x + temp2 * t->y + top->zz * t->z;
    top->zx = temp1 * cos + temp2 * sin;
    top->zy = temp2 * cos - temp1 * sin;

    temp1 = top->wx;
    temp2 = top->wy;
    top->ww += temp1 * t->x + temp2 * t->y + top->wz * t->z;
    top->wx = temp1 * cos + temp2 * sin;
    top->wy = temp2 * cos - temp1 * sin;

    int16_t ry = SY(rot);
    if (ry != 0) {
        apply_rot_y(top, trig_cos_s(rdram, ry), trig_sin_s(rdram, ry));
    }
    int16_t rx = SX(rot);
    if (rx != 0) {
        apply_rot_x(top, trig_cos_s(rdram, rx), trig_sin_s(rdram, rx));
    }
}

void Matrix_softcv3_load(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    float x = ctx->f12.fl, y = ctx->f14.fl, z = rt_f32(ctx->r6);
    uint32_t rot = ctx->r7;
    int16_t ry = SY(rot);
    float sinY = trig_sin_s(rdram, ry);
    float cosY = trig_cos_s(rdram, ry);
    float cosTemp, sinTemp;

    top->xx = cosY;
    top->zx = -sinY;
    top->xw = x;
    top->yw = y;
    top->zw = z;
    top->wx = 0.0f;
    top->wy = 0.0f;
    top->wz = 0.0f;
    top->ww = 1.0f;

    int16_t rx = SX(rot);
    if (rx != 0) {
        sinTemp = trig_sin_s(rdram, rx);
        cosTemp = trig_cos_s(rdram, rx);
        top->zz = cosY * cosTemp;
        top->zy = cosY * sinTemp;
        top->xz = sinY * cosTemp;
        top->xy = sinY * sinTemp;
        top->yz = -sinTemp;
        top->yy = cosTemp;
    } else {
        top->zz = cosY;
        top->xz = sinY;
        top->yz = 0.0f;
        top->zy = 0.0f;
        top->xy = 0.0f;
        top->yy = 1.0f;
    }

    int16_t rz = SZ(rot);
    if (rz != 0) {
        sinTemp = trig_sin_s(rdram, rz);
        cosTemp = trig_cos_s(rdram, rz);
        sinY = top->xx;
        cosY = top->xy;
        top->xx = sinY * cosTemp + cosY * sinTemp;
        top->xy = cosY * cosTemp - sinY * sinTemp;
        sinY = top->zx;
        cosY = top->zy;
        top->zx = sinY * cosTemp + cosY * sinTemp;
        top->zy = cosY * cosTemp - sinY * sinTemp;
        cosY = top->yy;
        top->yx = cosY * sinTemp;
        top->yy = cosY * cosTemp;
    } else {
        top->yx = 0.0f;
    }
}

void _MtxF_to_Mtx(uint8_t* rdram, recomp_context* ctx) {
    mtxf_to_mtx(rdram, MTX(ctx->r4), ctx->r5);
    ctx->r2 = ctx->r5;
}

void _Matrix_to_Mtx(uint8_t* rdram, recomp_context* ctx) {
    mtxf_to_mtx(rdram, TOP(), ctx->r4);
    ctx->r2 = ctx->r4;
}

void Matrix_Position(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* src = VEC(ctx->r4);
    NVec* dest = VEC(ctx->r5);
    dest->x = top->xw + (top->xx * src->x + top->xy * src->y + top->xz * src->z);
    dest->y = top->yw + (top->yx * src->x + top->yy * src->y + top->yz * src->z);
    dest->z = top->zw + (top->zx * src->x + top->zy * src->y + top->zz * src->z);
}

void Matrix_Position_Zero(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* dest = VEC(ctx->r4);
    dest->x = top->xw;
    dest->y = top->yw;
    dest->z = top->zw;
}

void Matrix_Position_VecX(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* dest = VEC(ctx->r5);
    float x = ctx->f12.fl;
    dest->x = top->xw + top->xx * x;
    dest->y = top->yw + top->yx * x;
    dest->z = top->zw + top->zx * x;
}

void Matrix_Position_VecY(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* dest = VEC(ctx->r5);
    float y = ctx->f12.fl;
    dest->x = top->xw + top->xy * y;
    dest->y = top->yw + top->yy * y;
    dest->z = top->zw + top->zy * y;
}

void Matrix_Position_VecZ(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* top = TOP();
    NVec* dest = VEC(ctx->r5);
    float z = ctx->f12.fl;
    dest->x = top->xw + top->xz * z;
    dest->y = top->yw + top->yz * z;
    dest->z = top->zw + top->zz * z;
}

void Matrix_MtxF_Position2(uint8_t* rdram, recomp_context* ctx) {
    NVec* src = VEC(ctx->r4);
    NVec* dest = VEC(ctx->r5);
    NMtxF* mf = MTX(ctx->r6);
    dest->x = mf->xw + (mf->xx * src->x + mf->xy * src->y + mf->xz * src->z);
    dest->y = mf->yw + (mf->yx * src->x + mf->yy * src->y + mf->yz * src->z);
    dest->z = mf->zw + (mf->zx * src->x + mf->zy * src->y + mf->zz * src->z);
}

void Matrix_MtxtoMtxF(uint8_t* rdram, recomp_context* ctx) {
    uint32_t src = ctx->r4;
    float* dest = &MTX(ctx->r5)->xx;
    for (int i = 0; i < 16; i++) {
        uint16_t ip = (uint16_t)nm_h(rdram, src + 2 * i);
        uint16_t fp = (uint16_t)nm_h(rdram, src + 0x20 + 2 * i);
        dest[i] = (float)(int32_t)((ip << 16) | fp) * (1 / (float)0x10000);
    }
}

/* m_skin_matrix.c */

void Skin_Matrix_PrjMulVector(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* mf = MTX(ctx->r4);
    NVec* src = VEC(ctx->r5);
    NVec* xyzDest = VEC(ctx->r6);
    float* wDest = (float*)nm_ptr(rdram, ctx->r7);
    xyzDest->x = mf->xw + ((src->x * mf->xx) + (src->y * mf->xy) + (src->z * mf->xz));
    xyzDest->y = mf->yw + ((src->x * mf->yx) + (src->y * mf->yy) + (src->z * mf->yz));
    xyzDest->z = mf->zw + ((src->x * mf->zx) + (src->y * mf->zy) + (src->z * mf->zz));
    *wDest = mf->ww + ((src->x * mf->wx) + (src->y * mf->wy) + (src->z * mf->wz));
}

void Skin_Matrix_MulVector(uint8_t* rdram, recomp_context* ctx) {
    NMtxF* mf = MTX(ctx->r4);
    NVec* src = VEC(ctx->r5);
    NVec* dest = VEC(ctx->r6);
    float mx = mf->xx, my = mf->xy, mz = mf->xz, mw = mf->xw;
    dest->x = mw + ((src->x * mx) + (src->y * my) + (src->z * mz));
    mx = mf->yx; my = mf->yy; mz = mf->yz; mw = mf->yw;
    dest->y = mw + ((src->x * mx) + (src->y * my) + (src->z * mz));
    mx = mf->zx; my = mf->zy; mz = mf->zz; mw = mf->zw;
    dest->z = mw + ((src->x * mx) + (src->y * my) + (src->z * mz));
}

void Skin_Matrix_MulMatrix(uint8_t* rdram, recomp_context* ctx) {
    skin_mul_matrix(MTX(ctx->r4), MTX(ctx->r5), MTX(ctx->r6));
}

void Skin_Matrix_SetUnitMatrix(uint8_t* rdram, recomp_context* ctx) {
    skin_set_unit(MTX(ctx->r4));
}

void Skin_Matrix_Copy(uint8_t* rdram, recomp_context* ctx) {
    skin_copy(MTX(ctx->r4), MTX(ctx->r5));
}

void Skin_Matrix_SetScale(uint8_t* rdram, recomp_context* ctx) {
    skin_set_scale(MTX(ctx->r4), rt_f32(ctx->r5), rt_f32(ctx->r6), rt_f32(ctx->r7));
}

void Skin_Matrix_SetTranslate(uint8_t* rdram, recomp_context* ctx) {
    skin_set_translate(MTX(ctx->r4), rt_f32(ctx->r5), rt_f32(ctx->r6), rt_f32(ctx->r7));
}

void Skin_Matrix_SetRotateXyz_s(uint8_t* rdram, recomp_context* ctx) {
    skin_set_rotate_xyz(rdram, MTX(ctx->r4), (int16_t)ctx->r5, (int16_t)ctx->r6, (int16_t)ctx->r7);
}

void Skin_Matrix_SetRotateZxy_s(uint8_t* rdram, recomp_context* ctx) {
    skin_set_rotate_zxy(rdram, MTX(ctx->r4), (int16_t)ctx->r5, (int16_t)ctx->r6, (int16_t)ctx->r7);
}

/* Scale, rotation (rotate(&r, ...)), translation: T * R * S into dest, with two temporaries. */
static void skin_set_srt(uint8_t* rdram, recomp_context* ctx,
                         void (*rotate)(uint8_t*, NMtxF*, int16_t, int16_t, int16_t)) {
    NMtxF* dest = MTX(ctx->r4);
    NMtxF mft1, mft2;
    float sx = rt_f32(ctx->r5), sy = rt_f32(ctx->r6), sz = rt_f32(ctx->r7);
    int16_t rx = (int16_t)rt_stack_arg(ctx, 4);
    int16_t ry = (int16_t)rt_stack_arg(ctx, 5);
    int16_t rz = (int16_t)rt_stack_arg(ctx, 6);
    float tx = rt_f32(rt_stack_arg(ctx, 7));
    float ty = rt_f32(rt_stack_arg(ctx, 8));
    float tz = rt_f32(rt_stack_arg(ctx, 9));

    skin_set_translate(dest, tx, ty, tz);
    rotate(rdram, &mft1, rx, ry, rz);
    skin_mul_matrix(dest, &mft1, &mft2);
    skin_set_scale(&mft1, sx, sy, sz);
    skin_mul_matrix(&mft2, &mft1, dest);
}

void Skin_Matrix_SetSrtMatrix(uint8_t* rdram, recomp_context* ctx) {
    skin_set_srt(rdram, ctx, skin_set_rotate_xyz);
}

void Skin_Matrix_SetSRzxyTMatrix(uint8_t* rdram, recomp_context* ctx) {
    skin_set_srt(rdram, ctx, skin_set_rotate_zxy);
}

void Skin_Matrix_SetRtMatrix(uint8_t* rdram, recomp_context* ctx) {
    NMtxF rotation, translation;
    int16_t rx = (int16_t)ctx->r5, ry = (int16_t)ctx->r6, rz = (int16_t)ctx->r7;
    float tx = rt_f32(rt_stack_arg(ctx, 4));
    float ty = rt_f32(rt_stack_arg(ctx, 5));
    float tz = rt_f32(rt_stack_arg(ctx, 6));

    skin_set_translate(&translation, tx, ty, tz);
    skin_set_rotate_xyz(rdram, &rotation, rx, ry, rz);
    skin_mul_matrix(&translation, &rotation, MTX(ctx->r4));
}

void Skin_Matrix_to_Mtx(uint8_t* rdram, recomp_context* ctx) {
    mtxf_to_mtx(rdram, MTX(ctx->r4), ctx->r5);
}

/* c_keyframe.c: the leaves of skeleton animation, once per joint axis and frame */

#define KF_ZERO_ADDR    0x80116110u /* IS_ZERO's 0.008f, as cKF_KeyCalc loads it */
#define KF_THIRTIETH_ADDR 0x80116114u /* 1.0f / 30 */

/* IDO turns 2 * x into x + x and -a + b into b - a: the same values. */
static inline float hermit_calc(float t, float duration, float p0, float p1, float v0, float v1) {
    float sq = t * t;
    float cb = sq * t;
    float h3 = sq * 3.0f - (cb + cb);
    float h2 = cb - sq;
    float h1 = (cb - (sq + sq)) + t;
    float h0 = 1.0f - h3;
    return (h0 * p0 + h3 * p1) + (h1 * v0 + h2 * v1) * duration;
}

/*
 * cKF_KeyCalc rounds the curve's value with nearbyint, in double precision --
 * which the Allegrex only has in software -- and as the recompiler does it
 * (lround: halves away from zero). The same rounding of the float itself:
 * every float is a double, and the difference to its truncation is exact.
 */
static inline int32_t round_half_away(float v) {
    int32_t i = (int32_t)v;
    float frac = v - (float)i;
    if (frac >= 0.5f) {
        i++;
    } else if (frac <= -0.5f) {
        i--;
    }
    return i;
}

void cKF_HermitCalc(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = hermit_calc(ctx->f12.fl, ctx->f14.fl, rt_f32(ctx->r6), rt_f32(ctx->r7),
                             rt_f32(rt_stack_arg(ctx, 4)), rt_f32(rt_stack_arg(ctx, 5)));
}

/* Keyframe: s16 frame, value, velocity. */
#define KF_FRAME(a) nm_h(rdram, (a) + 0)
#define KF_VALUE(a) nm_h(rdram, (a) + 2)
#define KF_VEL(a)   nm_h(rdram, (a) + 4)

static int16_t key_calc(uint8_t* rdram, int16_t start, int16_t sequence_length, uint32_t data_source, float frame) {
    uint32_t ds = data_source + 6 * start;
    int32_t length = sequence_length;
    int16_t result;

    if (frame <= (float)KF_FRAME(ds)) {
        result = KF_VALUE(ds);
    } else if ((float)KF_FRAME(ds + 6 * (length - 1)) <= frame) {
        result = KF_VALUE(ds + 6 * (length - 1));
    } else {
        uint32_t kf1 = ds;
        uint32_t kf2 = ds + 6;
        while (!(frame < (float)KF_FRAME(kf2))) {
            kf1 += 6;
            kf2 += 6;
        }
        int16_t f1 = KF_FRAME(kf1);
        float delta = (float)(KF_FRAME(kf2) - f1);
        if (fabsf(delta) < rt_f32(MEM_W(0, KF_ZERO_ADDR))) {
            result = KF_VALUE(kf1);
        } else {
            float h = hermit_calc((frame - (float)f1) / delta, delta * rt_f32(MEM_W(0, KF_THIRTIETH_ADDR)),
                                  (float)KF_VALUE(kf1), (float)KF_VALUE(kf2), (float)KF_VEL(kf1), (float)KF_VEL(kf2));
            result = (int16_t)round_half_away(h);
        }
    }
    return result;
}

void cKF_KeyCalc(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)key_calc(rdram, (int16_t)ctx->r4, (int16_t)ctx->r5, ctx->r6, rt_f32(ctx->r7));
}

/* ---- m_field_info.c: the field's blocks and units, asked thousands of times a frame */

#define G_FDINFO_ADDR   0x8013A248u /* FieldMakeInfo* g_fdinfo */
#define L_EDGE_UT_ADDR  0x80106820u /* l_edge_ut: what mFI_UtNum2UtCol gives outside the field */
#define FD_BLOCK_INFO   0x148       /* FieldMakeBlockInfo* blockInfo */
#define FD_BLOCK_XMAX   0x166       /* u8 */
#define FD_BLOCK_ZMAX   0x167       /* u8 */
#define BLOCK_INFO_SIZE 1556        /* sizeof(FieldMakeBlockInfo) */
#define BLOCK_UT_COLS   0x20        /* its unit collision table, 16 x 16 words */
#define UT_NUM          16          /* units a block, each way */

static inline uint32_t fd_info(uint8_t* rdram) {
    return (uint32_t)MEM_W(0, G_FDINFO_ADDR);
}

static inline int32_t fd_xmax(uint8_t* rdram) {
    uint32_t fd = fd_info(rdram);
    return fd != 0 ? MEM_BU(0, fd + FD_BLOCK_XMAX) : 0;
}

static inline int32_t fd_zmax(uint8_t* rdram) {
    uint32_t fd = fd_info(rdram);
    return fd != 0 ? MEM_BU(0, fd + FD_BLOCK_ZMAX) : 0;
}

static inline int32_t block_num(uint8_t* rdram, int32_t bx, int32_t bz) {
    return bx + bz * fd_xmax(rdram);
}

/* mFI_BlockCheck: inside the field and not an empty (0xE9) block. */
static inline int32_t block_check(uint8_t* rdram, int32_t bx, int32_t bz) {
    int32_t n = block_num(rdram, bx, bz);
    if (bx < 0 || bx >= fd_xmax(rdram) || bz < 0 || bz >= fd_zmax(rdram)) {
        return 0;
    }
    uint32_t info = (uint32_t)MEM_W(0, fd_info(rdram) + FD_BLOCK_INFO);
    return ((uint32_t)MEM_W(0, info + n * BLOCK_INFO_SIZE) >> 18) != 0xE9;
}

static inline int32_t ut_num_check(int32_t utX, int32_t utZ, int32_t xmax, int32_t zmax) {
    return !(utX < 0 || utX >= xmax * UT_NUM || utZ < 0 || utZ >= zmax * UT_NUM);
}

/* C division: towards zero, as the game's code does it. */
static inline int32_t ut_to_block(int32_t ut) {
    if (ut < 0) {
        ut -= UT_NUM - 1;
    }
    return ut / UT_NUM;
}

void mFI_CheckFieldData(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = fd_info(rdram) != 0;
}

void mFI_GetBlockXMax(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)fd_xmax(rdram);
}

void mFI_GetBlockZMax(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)fd_zmax(rdram);
}

void mFI_GetBlockNum(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)block_num(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5);
}

void mFI_BlockCheck(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)block_check(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5);
}

void mFI_UtNumCheck(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)ut_num_check((int32_t)ctx->r4, (int32_t)ctx->r5, (int32_t)ctx->r6, (int32_t)ctx->r7);
}

void mFI_GetUtNumInBK(uint8_t* rdram, recomp_context* ctx) {
    int32_t utX = (int32_t)ctx->r6, utZ = (int32_t)ctx->r7;
    int32_t ok = ut_num_check(utX, utZ, fd_xmax(rdram), fd_zmax(rdram));
    MEM_W(0, ctx->r4) = ok ? (utX & 0xF) : 0;
    MEM_W(0, ctx->r5) = ok ? (utZ & 0xF) : 0;
    ctx->r2 = (gpr)ok;
}

void mFI_UtNum2BlockNum(uint8_t* rdram, recomp_context* ctx) {
    int32_t bx = ut_to_block((int32_t)ctx->r6);
    int32_t bz = ut_to_block((int32_t)ctx->r7);
    MEM_W(0, ctx->r4) = bx;
    MEM_W(0, ctx->r5) = bz;
    ctx->r2 = (gpr)block_check(rdram, MEM_W(0, ctx->r4), bz);
}

/* The unit's collision word, or l_edge_ut outside the field. */
void mFI_UtNum2UtCol(uint8_t* rdram, recomp_context* ctx) {
    int32_t utX = (int32_t)ctx->r4, utZ = (int32_t)ctx->r5;
    if (!ut_num_check(utX, utZ, fd_xmax(rdram), fd_zmax(rdram))) {
        ctx->r2 = L_EDGE_UT_ADDR;
        return;
    }
    int32_t bx = ut_to_block(utX), bz = ut_to_block(utZ);
    if (!block_check(rdram, bx, bz)) {
        ctx->r2 = L_EDGE_UT_ADDR;
        return;
    }
    /* mFI_GetUtNumInBK, whose check has just passed */
    int32_t ux = utX & 0xF, uz = utZ & 0xF;
    uint32_t info = (uint32_t)MEM_W(0, fd_info(rdram) + FD_BLOCK_INFO);
    ctx->r2 = info + block_num(rdram, bx, bz) * BLOCK_INFO_SIZE + BLOCK_UT_COLS + (uz * UT_NUM + ux) * 4;
}

/* (utX, utZ, xyz_t wpos): wpos.x in $a2, .y in $a3, .z on the stack. */
void mFI_Wpos2UtNum(uint8_t* rdram, recomp_context* ctx) {
    float x = rt_f32(ctx->r6), z = rt_f32(rt_stack_arg(ctx, 4));
    MEM_W(0, ctx->r4) = (int32_t)(x / 40.0f);
    MEM_W(0, ctx->r5) = (int32_t)(z / 40.0f);
    ctx->r2 = !(x < 0.0f || (float)fd_xmax(rdram) * 640.0f < x || z < 0.0f || (float)fd_zmax(rdram) * 640.0f < z);
}
