/*
 * gfx_vertex.c -- matrices and the vertex stage.
 *
 * G_VTX transforms vertices by the modelview and the modelview-projection
 * matrices, works out their clip flags, lights them and computes texture
 * coordinates and fog, as the RSP does. The heavy lifting is VFPU code.
 */
#include <pspkernel.h>
#include <math.h>
#include <string.h>

#include "gfx_internal.h"

/* ---- matrices ----------------------------------------------------------- */

void mat_identity(Mat4 m) {
    memset(m, 0, sizeof(Mat4));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

/*
 * out = a * b (row-vector convention: v * a * b), on the VFPU. vtfm4 gives
 * result[i] = sum(r) M[r][i] * v[r] when the matrix' memory rows were loaded
 * into the VFPU's rows, so feeding it b that way and then a's rows one at a
 * time produces out's rows. Every load happens before every store, so out
 * may be a or b -- G_MTX multiplies a matrix into itself.
 *
 * The game dirties the modelview on nearly every vertex load, so this runs
 * about as often as G_VTX; on the FPU it was most of that command's cost.
 */
void mat_mul(Mat4 out, const Mat4 a, const Mat4 b) {
    __asm__ volatile(
        "lv.q R400,  0(%2)\n"
        "lv.q R401, 16(%2)\n"
        "lv.q R402, 32(%2)\n"
        "lv.q R403, 48(%2)\n"
        "lv.q C500,  0(%1)\n"
        "lv.q C510, 16(%1)\n"
        "lv.q C520, 32(%1)\n"
        "lv.q C530, 48(%1)\n"
        "vtfm4.q C600, M400, C500\n"
        "vtfm4.q C610, M400, C510\n"
        "vtfm4.q C620, M400, C520\n"
        "vtfm4.q C630, M400, C530\n"
        "sv.q C600,  0(%0)\n"
        "sv.q C610, 16(%0)\n"
        "sv.q C620, 32(%0)\n"
        "sv.q C630, 48(%0)\n"
        :
        : "r"(out), "r"(a), "r"(b)
        : "memory");
}

static void update_mvp(void) {
    if (gRsp.mvp_dirty) {
        mat_mul(gRsp.mvp, gRsp.mv_stack[gRsp.mv_depth], gRsp.proj);
        gRsp.mvp_dirty = false;
    }
}

/* ---- vertices ----------------------------------------------------------- */

/*
 * Colours and normals arrive as bytes and are wanted as floats; the FPU's
 * convert-and-scale costs more than a cached table lookup.
 */
static float sByteUnit[256];    /* b / 255 */
static float sByteNormal[256];  /* (int8)b / 127 */

static void init_byte_tables(void) {
    for (int i = 0; i < 256; i++) {
        sByteUnit[i] = i / 255.0f;
        sByteNormal[i] = (float)(int8_t)i / 127.0f;
    }
}

void gfx_vertex_init(void) {
    init_byte_tables();
}

void normalize3(float* v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.000001f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

/* Smallest squared length that still gets normalised; below it the direction
 * keeps whatever the transform gave it, as dividing by its length would. */
static const float kMinLenSq __attribute__((aligned(16))) = 1.0e-12f;

/*
 * Transforms a world-space direction into model space (the transpose of the
 * modelview's 3x3) and normalises it. vtfm3 dots the matrix' memory rows
 * against the vector (see mat_mul), so loading the modelview's rows down the
 * VFPU's columns -- which transposes it -- gives the model-space direction in
 * one instruction.
 * The direction itself is three floats in the middle of a Light, so it is
 * loaded and stored a word at a time; only the matrix is quad-aligned.
 */
static void dir_to_model(const Mat4 mv, const float* in, float* out) {
    __asm__ volatile(
        "lv.q C400,  0(%1)\n"
        "lv.q C410, 16(%1)\n"
        "lv.q C420, 32(%1)\n"
        "lv.s S500, 0(%2)\n"
        "lv.s S501, 4(%2)\n"
        "lv.s S502, 8(%2)\n"
        "vtfm3.t C510, M400, C500\n"
        "vdot.t S520, C510, C510\n"
        "lv.s S521, 0(%3)\n"
        "vmax.s S520, S520, S521\n"   /* a zero direction stays zero, not NaN */
        "vrsq.s S520, S520\n"
        "vscl.t C530, C510, S520\n"
        "sv.s S530, 0(%0)\n"
        "sv.s S531, 4(%0)\n"
        "sv.s S532, 8(%0)\n"
        :
        : "r"(out), "r"(mv), "r"(in), "r"(&kMinLenSq)
        : "memory");
}

/* What light_vertices_vfpu loads: made along with the model-space directions, as they are what it depends on. */
static float sLightDirs[4][4] __attribute__((aligned(16)));
static float sLightCols[4][4] __attribute__((aligned(16)));
static float sLightAmb[4] __attribute__((aligned(16)));

static void update_light_dirs(void) {
    if (!gRsp.lights_dirty) {
        return;
    }
    const float (*mv)[4] = (const float (*)[4])gRsp.mv_stack[gRsp.mv_depth];
    for (int i = 0; i < gRsp.num_lights; i++) {
        dir_to_model(mv, gRsp.lights[i].dir, gRsp.lights[i].model_dir);
    }
    dir_to_model(mv, gRsp.lookat[0], gRsp.lookat_model[0]);
    dir_to_model(mv, gRsp.lookat[1], gRsp.lookat_model[1]);
    for (int l = 0; l < 4; l++) {
        const Light* light = &gRsp.lights[l];
        bool on = l < gRsp.num_lights;
        for (int k = 0; k < 3; k++) {
            sLightDirs[l][k] = on ? light->model_dir[k] * (1.0f / 127.0f) : 0.0f;
            sLightCols[l][k] = on ? light->col[k] : 0.0f;
        }
        sLightDirs[l][3] = sLightCols[l][3] = 0.0f;
    }
    for (int k = 0; k < 3; k++) {
        sLightAmb[k] = gRsp.lights[gRsp.num_lights].col[k];
    }
    sLightAmb[3] = 0.0f;
    gRsp.lights_dirty = false;
}

/*
 * Transforms a vertex and works out its clipping flags while the clip
 * coordinates are still in registers. The flags (CLIP_* in gfx_internal.h)
 * are eleven comparisons of the form "value > cw": three vectors of
 * comparisons give 1.0 or 0.0 per lane, and a dot product with the flag
 * values adds them up.
 *
 * The position words hold x,y,z as big-endian shorts, so a pair expands to
 * (y, x, flag, z); [y,x,w,w] puts x,y,z back in order for the homogeneous
 * transforms (w = 1).
 */
static inline void transform_one(RspVertex* v, const uint32_t* src) {
    __asm__ volatile(
        "lv.s S200, 0(%1)\n"
        "lv.s S201, 4(%1)\n"
        "vs2i.p C210, C200\n"
        "vi2f.q C210, C210, 16\n"
        "vmov.q C220, C210[y,x,w,w]\n"
        "vhtfm4.q C230, M000, C220\n"
        "vhtfm4.q C300, M100, C220\n"
        "sv.q C230,  0(%0)\n"
        "sv.s S300, 24(%0)\n"
        "sv.s S301, 28(%0)\n"
        "sv.s S302, 32(%0)\n"
        /* clipping flags */
        "vmov.q C400, C230[x,-x,y,-y]\n"
        "vmul.q C410, C230[w,w,w,w], C600\n"
        "vmul.q C420, C230[z,-z,1,0], C620\n"
        "vmul.q C430, C230[w,w,w,1], C610\n"
        "vslt.q C500, C230[w,w,w,w], C400\n"
        "vslt.q C510, C410, C400\n"
        "vslt.q C520, C430, C420\n"
        "vdot.q S530, C500, C630\n"
        "vdot.q S531, C510, C700\n"
        "vdot.q S532, C520, C710\n"
        "vadd.s S530, S530, S531\n"
        "vadd.s S530, S530, S532\n"
        "vf2iu.s S530, S530, 0\n"
        "sv.s S530, 60(%0)\n"
        :
        : "r"(v), "r"(src)
        : "memory");
}

/*
 * Constants the clipping flags are built from: the guard band, the depth
 * pinning limit, the near-plane epsilon, and the flag values themselves.
 */
static const float kClipConstants[6][4] __attribute__((aligned(16))) = {
    { GUARD_BAND, GUARD_BAND, GUARD_BAND, GUARD_BAND },
    { DEPTH_PIN, DEPTH_PIN, 1.0f, 1.0f },
    { 1.0f, 1.0f, W_EPSILON, 0.0f },
    { CLIP_X_POS, CLIP_X_NEG, CLIP_Y_POS, CLIP_Y_NEG },
    { 0x02, 0x04, 0x08, 0x10 },
    { CLIP_Z_FAR, CLIP_Z_NEAR, CLIP_NEAR, 0.0f },
};

/*
 * Everything about a lit vertex except the two transforms, for what the VFPU
 * passes don't do: more than four lights, texture generation. The batch
 * settings come in by value: the transforms' asm clobbers memory, so anything
 * read through a pointer would be loaded again for every vertex.
 */
static inline void finish_lit_vertex(RspVertex* v, const uint32_t* w, bool texgen, bool fog, float scale_s,
                                     float scale_t, float fog_mul, float fog_ofs) {
    int16_t s = (int16_t)(w[2] >> 16);
    int16_t t = (int16_t)w[2];
    uint32_t cn = w[3];

    float inv_w = v->cw != 0 ? 1.0f / v->cw : 0;
    v->nx = v->cx * inv_w;
    v->ny = v->cy * inv_w;

    /* Like the RSP, normals are taken as given (unit length at 127). */
    float n[3] = { sByteNormal[(cn >> 24) & 0xFF], sByteNormal[(cn >> 16) & 0xFF],
                   sByteNormal[(cn >> 8) & 0xFF] };
    float r = gRsp.lights[gRsp.num_lights].col[0];
    float g = gRsp.lights[gRsp.num_lights].col[1];
    float b = gRsp.lights[gRsp.num_lights].col[2];
    for (int l = 0; l < gRsp.num_lights; l++) {
        const Light* light = &gRsp.lights[l];
        float d = n[0] * light->model_dir[0] + n[1] * light->model_dir[1] + n[2] * light->model_dir[2];
        if (d > 0) {
            r += light->col[0] * d;
            g += light->col[1] * d;
            b += light->col[2] * d;
        }
    }
    v->r = r > 1.0f ? 1.0f : r;
    v->g = g > 1.0f ? 1.0f : g;
    v->b = b > 1.0f ? 1.0f : b;
    if (texgen) {
        float dx = n[0] * gRsp.lookat_model[0][0] + n[1] * gRsp.lookat_model[0][1] + n[2] * gRsp.lookat_model[0][2];
        float dy = n[0] * gRsp.lookat_model[1][0] + n[1] * gRsp.lookat_model[1][1] + n[2] * gRsp.lookat_model[1][2];
        if (gRsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
            dx = acosf(dx) / (float)M_PI * 2.0f - 1.0f;
            dy = acosf(dy) / (float)M_PI * 2.0f - 1.0f;
        }
        v->u = (dx + 1.0f) / 4.0f * gRsp.tex_scale_s;
        v->v = (dy + 1.0f) / 4.0f * gRsp.tex_scale_t;
    } else {
        v->u = s * scale_s;
        v->v = t * scale_t;
    }
    if (fog) {
        float f = v->cz * inv_w * fog_mul + fog_ofs;
        v->a = f < 0 ? 0 : f > 1.0f ? 1.0f : f;
    } else {
        v->a = sByteUnit[cn & 0xFF];
    }
}

/*
 * Directional lighting for a batch of vertices on the VFPU (up to four lights,
 * no texgen: the common case; finish_lit_vertex does the rest in C). Runs after
 * the batch's transforms, so it may use every VFPU register:
 *   M200  the light directions, one per column, pre-scaled by 1/127 because
 *         normals are signed bytes with 127 = 1
 *   M300  the light colours, one per row
 *   C500  ambient, C510 zeros, C520 ones
 * Per vertex: the normal's bytes are widened to floats (vc2i/vi2f), one vtfm4
 * gives every light's dot product, negatives are clamped to zero, a second
 * vtfm4 sums the colours, then ambient is added and the result clamped to 1.
 * Same maths as finish_lit_vertex's C loop, summed in a different order.
 */
static void light_vertices_vfpu(RspVertex* verts, const uint32_t* src, int count) {
    const float(*dirs)[4] = sLightDirs;
    const float(*cols)[4] = sLightCols;
    const float* amb = sLightAmb;
    __asm__ volatile(
        "lv.q C200,  0(%0)\n"
        "lv.q C210, 16(%0)\n"
        "lv.q C220, 32(%0)\n"
        "lv.q C230, 48(%0)\n"
        "lv.q R300,  0(%1)\n"
        "lv.q R301, 16(%1)\n"
        "lv.q R302, 32(%1)\n"
        "lv.q R303, 48(%1)\n"
        "lv.q C500,  0(%2)\n"
        "vzero.q C510\n"
        "vone.q C520\n"
        :
        : "r"(dirs), "r"(cols), "r"(amb)
        : "memory");
    for (int i = 0; i < count; i++, src += 4) {
        __asm__ volatile(
            "lv.s S400, 12(%1)\n"
            "vc2i.s C410, S400\n"            /* bytes (a, nz, ny, nx) << 24 */
            "vi2f.q C410, C410, 24\n"
            "vmov.q C420, C410[w,z,y,0]\n"   /* (nx, ny, nz, 0) */
            "vtfm4.q C430, M200, C420\n"     /* n . dir, per light */
            "vmax.q C430, C430, C510\n"
            "vtfm4.q C600, M300, C430\n"     /* sum of colour * dot */
            "vadd.t C600, C600, C500\n"
            "vmin.t C600, C600, C520\n"
            "sv.s S600, 44(%0)\n"
            "sv.s S601, 48(%0)\n"
            "sv.s S602, 52(%0)\n"
            :
            : "r"(&verts[i]), "r"(src)
            : "memory");
    }
}
_Static_assert(offsetof(RspVertex, r) == 44 && offsetof(RspVertex, b) == 52, "light_vertices_vfpu stores r,g,b here");

/*
 * A vertex in the common modes (no texture generation, and either no lighting
 * or lighting that light_vertices_vfpu does afterwards) as one VFPU sequence:
 * transform_one's transforms and clip flags, and everything after them. The
 * clip coordinates stay in registers for 1/w, the screen position, the
 * texture coordinates, the colour and the fog, so nothing goes through memory
 * and back into the FPU. In the lit case the colour written here is replaced
 * by the lighting pass.
 * C720 holds the batch's (scale_s, scale_t, fog_mul, fog_ofs).
 * The output quads are the RspVertex rows at 16, 32 and 48:
 *   (nx, ny, wx, wy)  (wz, u, v, r)  (g, b, a, clip flags)
 */
#define FUSED_HEAD                                                           \
        "lv.s S200, 0(%1)\n"                                                 \
        "lv.s S201, 4(%1)\n"                                                 \
        "lv.s S202, 8(%1)\n"                                                 \
        "mtv %2, S203\n"                                                     \
        "vs2i.p C210, C200\n"                                                \
        "vi2f.q C210, C210, 16\n"                                            \
        "vmov.q C220, C210[y,x,w,w]\n"                                       \
        "vhtfm4.q C230, M000, C220\n"                                        \
        "vhtfm4.q C300, M100, C220\n"                                        \
        "sv.q C230, 0(%0)\n"                                                 \
        "vmov.q C400, C230[x,-x,y,-y]\n"                                     \
        "vmul.q C410, C230[w,w,w,w], C600\n"                                 \
        "vmul.q C420, C230[z,-z,1,0], C620\n"                                \
        "vmul.q C430, C230[w,w,w,1], C610\n"                                 \
        "vslt.q C500, C230[w,w,w,w], C400\n"                                 \
        "vslt.q C510, C410, C400\n"                                          \
        "vslt.q C520, C430, C420\n"                                          \
        "vdot.q S530, C500, C630\n"                                          \
        "vdot.q S531, C510, C700\n"                                          \
        "vdot.q S532, C520, C710\n"                                          \
        "vrcp.s S730, S233\n"                                                \
        "vadd.s S530, S530, S531\n"                                          \
        "vadd.s S530, S530, S532\n"                                          \
        "vf2iu.s S333, S530, 0\n"                                            \
        "vscl.p C310, C230, S730\n"                                          \
        "vmov.p C312, C300\n"                                                \
        "vmov.s S320, S302\n"                                                \
        "vs2i.s C500, S202\n"                                                \
        "vi2f.p C500, C500, 16\n"                                            \
        "vmul.p C510, C500[y,x], C720\n"                                     \
        "vmov.s S321, S510\n"                                                \
        "vmov.s S322, S511\n"                                                \
        "vc2i.s C500, S203\n"                                                \
        "vi2f.q C500, C500, 24\n"                                            \
        "vadd.q C500, C500, C730[y,y,y,y]\n"                                 \
        "vscl.q C500, C500, S732\n"                                          \
        "vmov.s S323, S503\n"                                                \
        "vmov.t C330, C500[z,y,x]\n"
#define FUSED_TAIL                                                           \
        "sv.q C310, 16(%0)\n"                                                \
        "sv.q C320, 32(%0)\n"                                                \
        "sv.q C330, 48(%0)\n"
#define FUSED_FOG                                                            \
        "vmul.s S510, S232, S730\n"                                          \
        "vmul.s S510, S510, S722\n"                                          \
        "vadd.s S510, S510, S723\n"                                          \
        "vsat0.s S332, S510\n"

/* (the colour bytes arrive with their top bits flipped: vc2i takes them as signed, 128 is added back) */
static inline void fused_one(RspVertex* v, const uint32_t* src) {
    __asm__ volatile(FUSED_HEAD FUSED_TAIL : : "r"(v), "r"(src), "r"(src[3] ^ 0x80808080u) : "memory");
}

static inline void fused_one_fog(RspVertex* v, const uint32_t* src) {
    __asm__ volatile(FUSED_HEAD FUSED_FOG FUSED_TAIL : : "r"(v), "r"(src), "r"(src[3] ^ 0x80808080u) : "memory");
}
_Static_assert(offsetof(RspVertex, nx) == 16 && offsetof(RspVertex, wz) == 32 && offsetof(RspVertex, u) == 36 &&
               offsetof(RspVertex, v) == 40 && offsetof(RspVertex, r) == 44 && offsetof(RspVertex, g) == 48 &&
               offsetof(RspVertex, a) == 56 && offsetof(RspVertex, clip) == 60, "fused_one stores whole rows");

/* The matrices stay in VFPU registers for the whole batch (M000: MVP,
 * M100: modelview), so a vertex costs two transforms instead of seven
 * dot products on the FPU. Our rows go into VFPU rows: vtfm4 multiplies
 * by the matrix's columns. */
static inline void load_vfpu_state(const float (*mvp)[4], const float (*mv)[4]) {
    __asm__ volatile(
        "lv.q R000,  0(%0)\n"
        "lv.q R001, 16(%0)\n"
        "lv.q R002, 32(%0)\n"
        "lv.q R003, 48(%0)\n"
        "lv.q R100,  0(%1)\n"
        "lv.q R101, 16(%1)\n"
        "lv.q R102, 32(%1)\n"
        "lv.q R103, 48(%1)\n"
        "lv.q C600,  0(%2)\n"
        "lv.q C610, 16(%2)\n"
        "lv.q C620, 32(%2)\n"
        "lv.q C630, 48(%2)\n"
        "lv.q C700, 64(%2)\n"
        "lv.q C710, 80(%2)\n"
        :
        : "r"(mvp), "r"(mv), "r"(kClipConstants));
}

void gfx_process_vertices(uint32_t addr, int start, int count) {
    if (start < 0 || start + count > MAX_VERTICES) {
        RT_LOG_ONCE("G_VTX out of range: start %d count %d", start, count);
        return;
    }
    update_mvp();
    const float (*mv)[4] = (const float (*)[4])gRsp.mv_stack[gRsp.mv_depth];
    const float (*mvp)[4] = (const float (*)[4])gRsp.mvp;
    bool lighting = (gRsp.geometry_mode & G_LIGHTING) != 0;
    bool texgen = (gRsp.geometry_mode & G_TEXTURE_GEN) != 0;
    bool fog = (gRsp.geometry_mode & G_FOG) != 0;
    float scale_s = gRsp.tex_scale_s / 65536.0f;
    float scale_t = gRsp.tex_scale_t / 65536.0f;
    float fog_mul = gRsp.fog_mul / 255.0f;
    float fog_ofs = gRsp.fog_ofs / 255.0f;
    if (lighting) {
        update_light_dirs();
    }
    /* Vtx is 4 big-endian words; RDRAM keeps words in host order. */
    const uint32_t* src = (const uint32_t*)(void*)(g_rdram + (addr & RDRAM_MASK & ~3u));
    uint32_t words[4 * MAX_VERTICES];
    if (addr & 3) {
        uint8_t raw[16 * MAX_VERTICES];
        rt_copy_from_rdram(addr, raw, 16 * count);
        for (int i = 0; i < 4 * count; i++) {
            words[i] = (uint32_t)raw[i * 4] << 24 | raw[i * 4 + 1] << 16 | raw[i * 4 + 2] << 8 | raw[i * 4 + 3];
        }
        src = words;
    }

    load_vfpu_state(mvp, mv);

    /* Unlit vertices, and lit ones without texture generation by up to four lights, are all VFPU work. */
    bool light_later = lighting && !texgen && gRsp.num_lights <= 4;
    const uint32_t* first = src;
    gfx_forget_packed(start, count); /* these slots hold different vertices now */
    if (!lighting || light_later) {
        static float consts[8] __attribute__((aligned(16)));
        consts[0] = scale_s;
        consts[1] = scale_t;
        consts[2] = fog_mul;
        consts[3] = fog_ofs;
        consts[4] = 0.0f;
        consts[5] = 128.0f;
        consts[6] = 1.0f / 255.0f;
        consts[7] = 0.0f;
        __asm__ volatile("lv.q C720, 0(%0)\nlv.q C730, 16(%0)\n" : : "r"(consts) : "memory");
        if (fog) {
            for (int i = 0; i < count; i++, src += 4) {
                fused_one_fog(&gRsp.verts[start + i], src);
            }
        } else {
            for (int i = 0; i < count; i++, src += 4) {
                fused_one(&gRsp.verts[start + i], src);
            }
        }
        if (light_later) {
            light_vertices_vfpu(&gRsp.verts[start], first, count);
        }
        gfx_snap_precompute(gRsp.verts, start, count);
        return;
    }
    for (int i = 0; i < count; i++, src += 4) {
        RspVertex* v = &gRsp.verts[start + i];
        transform_one(v, src);
        finish_lit_vertex(v, src, texgen, fog, scale_s, scale_t, fog_mul, fog_ofs);
    }
    gfx_snap_precompute(gRsp.verts, start, count);
}
