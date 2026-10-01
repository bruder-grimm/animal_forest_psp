/*
 * gfx_vertex.c -- matrices and the vertex stage.
 *
 * G_VTX transforms vertices by the modelview and the modelview-projection
 * matrices, works out their clip flags, lights them and computes texture
 * coordinates and fog, as the RSP does. The heavy lifting is VFPU code; each
 * piece of it has a plain C reference next to it, which the self-test
 * (bench_vtx.txt, at the end of this file) compares it against.
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

/* out = a * b (row-vector convention: v * a * b). The reference the VFPU
 * version below is checked against. */
static void mat_mul_ref(Mat4 out, const Mat4 a, const Mat4 b) {
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, r, sizeof(Mat4));
}

/*
 * The same on the VFPU. vtfm4 gives result[i] = sum(r) M[r][i] * v[r] when the
 * matrix' memory rows were loaded into the VFPU's rows (which is what the
 * selftest's 13951 pins down), so feeding it b that way and then a's rows one
 * at a time produces out's rows. Every load happens before every store, so out
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

/* Transform a world-space direction into model space (transpose of the
 * modelview's 3x3). The reference the VFPU version is checked against. */
static void dir_to_model_ref(const Mat4 mv, const float* in, float* out) {
    out[0] = mv[0][0] * in[0] + mv[0][1] * in[1] + mv[0][2] * in[2];
    out[1] = mv[1][0] * in[0] + mv[1][1] * in[1] + mv[1][2] * in[2];
    out[2] = mv[2][0] * in[0] + mv[2][1] * in[1] + mv[2][2] * in[2];
    normalize3(out);
}

/* Smallest squared length that still gets normalised; below it the direction
 * keeps whatever the transform gave it, as dividing by its length would. */
static const float kMinLenSq __attribute__((aligned(16))) = 1.0e-12f;

/*
 * The same on the VFPU. vtfm3 dots the matrix' memory rows against the vector
 * (see mat_mul), so loading the modelview's rows down the VFPU's columns --
 * which transposes it -- gives the model-space direction in one instruction.
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
    gRsp.lights_dirty = false;
}

/* The vertex stage computes these on the VFPU; this is the reference the
 * benchmark checks it against, and documents what the flags mean. */
static void compute_clip_flags(RspVertex* v) {
    uint16_t f = 0;
    if (v->cw < W_EPSILON) f |= CLIP_NEAR;
    if (v->cx > GUARD_BAND * v->cw) f |= 0x02;
    if (v->cx < -GUARD_BAND * v->cw) f |= 0x04;
    if (v->cy > GUARD_BAND * v->cw) f |= 0x08;
    if (v->cy < -GUARD_BAND * v->cw) f |= 0x10;
    if (v->cx > v->cw) f |= CLIP_X_POS;
    if (v->cx < -v->cw) f |= CLIP_X_NEG;
    if (v->cy > v->cw) f |= CLIP_Y_POS;
    if (v->cy < -v->cw) f |= CLIP_Y_NEG;
    if (v->cz < -DEPTH_PIN * v->cw) f |= CLIP_Z_NEAR;
    if (v->cz > DEPTH_PIN * v->cw) f |= CLIP_Z_FAR;
    v->clip = f;
}

/*
 * Transforms a vertex and works out its clipping flags while the clip
 * coordinates are still in registers. The flags are eleven comparisons of the
 * form "value > cw" (see compute_clip_flags, which is the same thing in C and
 * what the benchmark checks this against): three vectors of comparisons give
 * 1.0 or 0.0 per lane, and a dot product with the flag values adds them up.
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
 * Everything about a vertex except the two transforms. The batch settings come
 * in by value: the transforms' asm clobbers memory, so anything read through a
 * pointer would be loaded again for every vertex.
 */
static inline void finish_vertex(RspVertex* v, const uint32_t* w, bool lighting, bool texgen, bool fog,
                                 float scale_s, float scale_t, float fog_mul, float fog_ofs, bool light_later) {
    int16_t s = (int16_t)(w[2] >> 16);
    int16_t t = (int16_t)w[2];
    uint32_t cn = w[3];

    float inv_w = v->cw != 0 ? 1.0f / v->cw : 0;
    v->nx = v->cx * inv_w;
    v->ny = v->cy * inv_w;

    if (lighting && light_later) {
        /* colour from light_vertices_vfpu, after the batch is transformed */
        v->u = s * scale_s;
        v->v = t * scale_t;
    } else if (lighting) {
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
    } else {
        v->r = sByteUnit[(cn >> 24) & 0xFF];
        v->g = sByteUnit[(cn >> 16) & 0xFF];
        v->b = sByteUnit[(cn >> 8) & 0xFF];
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
 * no texgen: the common case; finish_vertex does the rest in C). Runs after
 * the batch's transforms, so it may use every VFPU register:
 *   M200  the light directions, one per column, pre-scaled by 1/127 because
 *         normals are signed bytes with 127 = 1
 *   M300  the light colours, one per row
 *   C500  ambient, C510 zeros, C520 ones
 * Per vertex: the normal's bytes are widened to floats (vc2i/vi2f), one vtfm4
 * gives every light's dot product, negatives are clamped to zero, a second
 * vtfm4 sums the colours, then ambient is added and the result clamped to 1.
 * Same maths as finish_vertex's C loop, summed in a different order.
 */
static bool sVfpuLighting = true; /* false: finish_vertex's C loop (the self-test's reference) */

static void light_vertices_vfpu(RspVertex* verts, const uint32_t* src, int count) {
    static float dirs[4][4] __attribute__((aligned(16)));
    static float cols[4][4] __attribute__((aligned(16)));
    static float amb[4] __attribute__((aligned(16)));
    for (int l = 0; l < 4; l++) {
        const Light* light = &gRsp.lights[l];
        bool on = l < gRsp.num_lights;
        for (int k = 0; k < 3; k++) {
            dirs[l][k] = on ? light->model_dir[k] * (1.0f / 127.0f) : 0.0f;
            cols[l][k] = on ? light->col[k] : 0.0f;
        }
        dirs[l][3] = cols[l][3] = 0.0f;
    }
    for (int k = 0; k < 3; k++) {
        amb[k] = gRsp.lights[gRsp.num_lights].col[k];
    }
    amb[3] = 0.0f;
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

void gfx_process_vertices(uint32_t addr, int start, int count) {
    if (start < 0 || start + count > MAX_VERTICES) {
        RT_LOG_ONCE("G_VTX out of range: start %d count %d", start, count);
        return;
    }
    if (gfx_ablated(3)) {
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

    /* The matrices stay in VFPU registers for the whole batch (M000: MVP,
     * M100: modelview), so a vertex costs two transforms instead of seven
     * dot products on the FPU. Our rows go into VFPU rows: vtfm4 multiplies
     * by the matrix's columns. */
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

    bool light_later = lighting && !texgen && gRsp.num_lights <= 4 && sVfpuLighting;
    const uint32_t* first = src;
    gfx_forget_packed(start, count); /* these slots hold different vertices now */
    for (int i = 0; i < count; i++, src += 4) {
        RspVertex* v = &gRsp.verts[start + i];
        transform_one(v, src);
        finish_vertex(v, src, lighting, texgen, fog, scale_s, scale_t, fog_mul, fog_ofs, light_later);
    }
    if (light_later) {
        light_vertices_vfpu(&gRsp.verts[start], first, count);
    }
}

/* ---- benchmark and self-test (bench_vtx.txt) ---------------------------- */

/*
 * Times the vertex stage on whatever machine this is, so optimisations are
 * measured rather than guessed. Runs at startup when bench_vtx.txt is next to
 * the EBOOT (the file's first number is the number of vertices per round, the
 * second the rounds). Results go to the log as ns per vertex.
 */
static void bench_setup(uint32_t addr, int count) {
    uint32_t* p = (uint32_t*)(void*)(g_rdram + (addr & RDRAM_MASK & ~3u));
    uint32_t seed = 12345;
    for (int i = 0; i < count; i++) {
        /* Model coordinates, texture coordinates, normal/colour, like the game's. */
        seed = seed * 1103515245u + 12345u;
        int16_t x = (int16_t)((seed >> 16) % 2000 - 1000);
        seed = seed * 1103515245u + 12345u;
        int16_t y = (int16_t)((seed >> 16) % 2000 - 1000);
        seed = seed * 1103515245u + 12345u;
        int16_t z = (int16_t)((seed >> 16) % 2000 - 1000);
        seed = seed * 1103515245u + 12345u;
        p[i * 4 + 0] = (uint32_t)((uint16_t)x << 16 | (uint16_t)y);
        p[i * 4 + 1] = (uint32_t)((uint16_t)z << 16);
        p[i * 4 + 2] = seed & 0x0FFF0FFF;
        p[i * 4 + 3] = seed ^ 0x40302010;
    }
}

static void bench_state(bool lighting, bool fog) {
    memset(&gRsp, 0, sizeof(gRsp));
    /* A perspective projection over a modelview with a translation and a spin. */
    static const Mat4 proj = { { 1.6f, 0, 0, 0 }, { 0, 1.2f, 0, 0 }, { 0, 0, -1.002f, -1.0f }, { 0, 0, -20.02f, 0 } };
    static const Mat4 mv = { { 0.87f, 0.1f, -0.48f, 0 }, { 0, 0.98f, 0.2f, 0 }, { 0.48f, -0.17f, 0.86f, 0 },
                             { 12.0f, -30.0f, -180.0f, 1.0f } };
    memcpy(gRsp.proj, proj, sizeof(Mat4));
    memcpy(gRsp.mv_stack[0], mv, sizeof(Mat4));
    gRsp.mv_depth = 0;
    gRsp.mvp_dirty = true;
    gRsp.tex_scale_s = gRsp.tex_scale_t = 0x8000;
    gRsp.vscale[0] = 160; gRsp.vscale[1] = 120; gRsp.vscale[2] = 511;
    gRsp.vtrans[0] = 160; gRsp.vtrans[1] = 120; gRsp.vtrans[2] = 511;
    gRsp.fog_mul = 200; gRsp.fog_ofs = -100;
    gRsp.geometry_mode = (lighting ? G_LIGHTING : 0) | (fog ? G_FOG : 0);
    gRsp.num_lights = 2;
    gRsp.lights_dirty = true;
    for (int i = 0; i < 3; i++) {
        gRsp.lights[0].col[i] = 0.7f;
        gRsp.lights[1].col[i] = 0.3f;
        gRsp.lights[2].col[i] = 0.2f;   /* ambient (index num_lights) */
        gRsp.lights[0].dir[i] = i == 1 ? 0.9f : 0.3f;
        gRsp.lights[1].dir[i] = i == 2 ? 0.8f : -0.4f;
        gRsp.lookat[0][i] = i == 0 ? 1.0f : 0.0f;
        gRsp.lookat[1][i] = i == 1 ? 1.0f : 0.0f;
    }
}

/* Isolates the VFPU int/float conversion so its semantics can be seen. */
static void bench_vfpu_selftest(void) {
    static uint32_t in[2] __attribute__((aligned(16))) = { 0x00020003u, 0x00040005u }; /* shorts 2,3 and 4,5 */
    static uint32_t raw[4] __attribute__((aligned(16)));
    static float conv[4] __attribute__((aligned(16)));
    static float shuf[4] __attribute__((aligned(16)));
    __asm__ volatile(
        "lv.s S200, 0(%0)\n"
        "lv.s S201, 4(%0)\n"
        "vs2i.p C300, C200\n"
        "sv.q C300, 0(%1)\n"
        "vi2f.q C400, C300, 16\n"
        "sv.q C400, 0(%2)\n"
        "vmov.q C500, C400[y,x,w,w]\n"
        "sv.q C500, 0(%3)\n"
        :
        : "r"(in), "r"(raw), "r"(conv), "r"(shuf)
        : "memory");
    static float mat[16] __attribute__((aligned(16))) = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    static float vec[4] __attribute__((aligned(16))) = { 1, 10, 100, 1000 };
    static float res[4] __attribute__((aligned(16)));
    __asm__ volatile(
        "lv.q R000,  0(%0)\n"
        "lv.q R001, 16(%0)\n"
        "lv.q R002, 32(%0)\n"
        "lv.q R003, 48(%0)\n"
        "lv.q C400, 0(%1)\n"
        "vtfm4.q C500, M000, C400\n"
        "sv.q C500, 0(%2)\n"
        :
        : "r"(mat), "r"(vec), "r"(res)
        : "memory");
    rt_log("vfpu selftest: tfm %d %d %d %d (13951,... = memory rows are inputs; 4321,... = transposed)",
           (int)res[0], (int)res[1], (int)res[2], (int)res[3]);
    uint32_t cb[4], sb[4];
    memcpy(cb, conv, sizeof(cb));
    memcpy(sb, shuf, sizeof(sb));
    rt_log("vfpu selftest: ints %08X %08X %08X %08X", (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2], (unsigned)raw[3]);
    rt_log("vfpu selftest: conv %08X %08X %08X %08X", (unsigned)cb[0], (unsigned)cb[1], (unsigned)cb[2], (unsigned)cb[3]);
    rt_log("vfpu selftest: shuf %08X %08X %08X %08X", (unsigned)sb[0], (unsigned)sb[1], (unsigned)sb[2], (unsigned)sb[3]);

    /* The matrix multiply and the light directions, against the same maths in C. */
    Mat4 ma, mb, got, want;
    uint32_t seed = 12345;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            seed = seed * 1103515245u + 12345u;
            ma[i][j] = (float)(int32_t)(seed >> 8) / 8.0e6f;
            seed = seed * 1103515245u + 12345u;
            mb[i][j] = (float)(int32_t)(seed >> 8) / 8.0e6f;
        }
    }
    mat_mul_ref(want, ma, mb);
    mat_mul(got, ma, mb);
    int worst = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t g, w;
            memcpy(&g, &got[i][j], 4);
            memcpy(&w, &want[i][j], 4);
            int ulp = g > w ? g - w : w - g;
            if (ulp > worst) worst = ulp;
        }
    }
    rt_log("vfpu selftest: mat_mul worst %d ulp", worst);

    /* out == a and out == b as well: G_MTX multiplies a matrix into itself. */
    Mat4 alias;
    memcpy(alias, ma, sizeof(Mat4));
    mat_mul_ref(want, ma, mb);
    mat_mul(alias, alias, mb);
    worst = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t g, w;
            memcpy(&g, &alias[i][j], 4);
            memcpy(&w, &want[i][j], 4);
            int ulp = g > w ? g - w : w - g;
            if (ulp > worst) worst = ulp;
        }
    }
    rt_log("vfpu selftest: mat_mul into itself worst %d ulp", worst);

    worst = 0;
    float biggest = 0;
    bool zero_ok = true;
    for (int t = 0; t < 8; t++) {
        float in[3], vg[3], vw[3];
        for (int k = 0; k < 3; k++) {
            seed = seed * 1103515245u + 12345u;
            in[k] = t == 7 ? 0.0f : (float)(int32_t)(seed >> 8) / 8.0e6f;
        }
        dir_to_model_ref((const float (*)[4])ma, in, vw);
        dir_to_model((const float (*)[4])ma, in, vg);
        for (int k = 0; k < 3; k++) {
            int32_t g, w;
            memcpy(&g, &vg[k], 4);
            memcpy(&w, &vw[k], 4);
            int ulp = g > w ? g - w : w - g;
            if (ulp > worst) worst = ulp;
            float d = vg[k] - vw[k];
            if (d < 0) d = -d;
            if (d > biggest) biggest = d;
            if (t == 7 && vg[k] != 0.0f) zero_ok = false; /* also false for a NaN */
        }
    }
    rt_log("vfpu selftest: dir_to_model worst %d ulp (%.9f), zero direction -> %s", worst, biggest,
           zero_ok ? "zero" : "NOT ZERO");
}

/* Checks the VFPU vertex code against the same maths in plain C (in ULPs,
 * without float arithmetic: subtracting near-equal values traps on denormals). */
static void bench_verify(uint32_t addr, int count) {
    bench_state(false, false);
    gfx_process_vertices(addr, 0, count);
    const float (*mv)[4] = (const float (*)[4])gRsp.mv_stack[gRsp.mv_depth];
    const float (*mvp)[4] = (const float (*)[4])gRsp.mvp;
    const uint32_t* src = (const uint32_t*)(void*)(g_rdram + (addr & RDRAM_MASK & ~3u));
    int worst = 0;
    for (int i = 0; i < count; i++, src += 4) {
        float x = (int16_t)(src[0] >> 16), y = (int16_t)src[0], z = (int16_t)(src[1] >> 16);
        float want[7] = {
            x * mvp[0][0] + y * mvp[1][0] + z * mvp[2][0] + mvp[3][0],
            x * mvp[0][1] + y * mvp[1][1] + z * mvp[2][1] + mvp[3][1],
            x * mvp[0][2] + y * mvp[1][2] + z * mvp[2][2] + mvp[3][2],
            x * mvp[0][3] + y * mvp[1][3] + z * mvp[2][3] + mvp[3][3],
            x * mv[0][0] + y * mv[1][0] + z * mv[2][0] + mv[3][0],
            x * mv[0][1] + y * mv[1][1] + z * mv[2][1] + mv[3][1],
            x * mv[0][2] + y * mv[1][2] + z * mv[2][2] + mv[3][2],
        };
        const RspVertex* v = &gRsp.verts[i];
        const float got[7] = { v->cx, v->cy, v->cz, v->cw, v->wx, v->wy, v->wz };
        for (int k = 0; k < 7; k++) {
            int32_t a, b;
            memcpy(&a, &want[k], 4);
            memcpy(&b, &got[k], 4);
            int d = a > b ? a - b : b - a;
            if ((a ^ b) < 0) {
                d = 1 << 30; /* different signs: not a near miss */
            }
            if (d > worst) {
                worst = d;
            }
        }
    }
    int clip_bad = 0;
    for (int i = 0; i < count; i++) {
        RspVertex copy = gRsp.verts[i];
        uint16_t got = copy.clip;
        compute_clip_flags(&copy);
        if (copy.clip != got) {
            if (clip_bad == 0) {
                rt_log("bench vtx verify: clip flags differ at %d: %03X vs %03X (cx %d cy %d cz %d cw %d)", i,
                       (unsigned)got, (unsigned)copy.clip, (int)copy.cx, (int)copy.cy, (int)copy.cz, (int)copy.cw);
            }
            clip_bad++;
        }
    }
    rt_log("bench vtx verify: worst %d ulp, %d/%d clip flags differ", worst, clip_bad, count);

    /* Lighting: the VFPU pass against finish_vertex's C loop, same vertices
     * (the bench's normals are random bytes, so some point away from both
     * lights and exercise the clamp at zero). */
    static float ref[MAX_VERTICES][3];
    int n = count > MAX_VERTICES ? MAX_VERTICES : count;
    bench_state(true, false);
    sVfpuLighting = false;
    gfx_process_vertices(addr, 0, n);
    for (int i = 0; i < n; i++) {
        ref[i][0] = gRsp.verts[i].r;
        ref[i][1] = gRsp.verts[i].g;
        ref[i][2] = gRsp.verts[i].b;
    }
    bench_state(true, false);
    sVfpuLighting = true;
    gfx_process_vertices(addr, 0, n);
    int lworst = 0;
    int lbad = 0;
    for (int i = 0; i < n; i++) {
        const float got[3] = { gRsp.verts[i].r, gRsp.verts[i].g, gRsp.verts[i].b };
        for (int k = 0; k < 3; k++) {
            int32_t a, b;
            memcpy(&a, &ref[i][k], 4);
            memcpy(&b, &got[k], 4);
            int d = a > b ? a - b : b - a;
            if ((a ^ b) < 0 && ref[i][k] != got[k]) {
                d = 1 << 30;
            }
            if (d > lworst) {
                lworst = d;
            }
            /* what actually reaches the screen: the 8-bit colour */
            if ((int)(ref[i][k] * 255.0f + 0.5f) != (int)(got[k] * 255.0f + 0.5f)) {
                lbad++;
            }
        }
    }
    rt_log("bench vtx verify: lighting worst %d ulp, %d/%d channels differ in 8 bits", lworst, lbad, n * 3);
}

static void bench_run(const char* name, bool lighting, bool fog, uint32_t addr, int count, int rounds) {
    bench_state(lighting, fog);
    gfx_process_vertices(addr, 0, count); /* warm the caches */
    uint64_t t0 = sceKernelGetSystemTimeWide();
    for (int i = 0; i < rounds; i++) {
        gRsp.mvp_dirty = true;
        gRsp.lights_dirty = true;
        gfx_process_vertices(addr, 0, count);
    }
    uint64_t us = sceKernelGetSystemTimeWide() - t0;
    uint32_t verts = (uint32_t)count * (uint32_t)rounds;
    rt_log("bench vtx %-10s %5u ns/vertex (%u verts in %u us)", name, (unsigned)(us * 1000 / verts), (unsigned)verts,
           (unsigned)us);
}

void rt_gfx_bench(void) {
    uint32_t v[2];
    int n = rt_load_number_list("bench_vtx.txt", v, 2);
    if (n < 1) {
        return;
    }
    int count = (int)v[0];
    int rounds = n > 1 ? (int)v[1] : 2000;
    if (count < 1 || count > MAX_VERTICES) {
        count = 32;
    }
    uint32_t addr = 0x80300000u;
    bench_setup(addr, count);
    rt_log("bench vtx: %d vertices x %d rounds", count, rounds);
    bench_vfpu_selftest();
    bench_verify(addr, count);
    bench_run("plain", false, false, addr, count, rounds);
    bench_run("fog", false, true, addr, count, rounds);
    bench_run("light", true, false, addr, count, rounds);
    sVfpuLighting = false;
    bench_run("light (C)", true, false, addr, count, rounds);
    sVfpuLighting = true;
    bench_run("light+fog", true, true, addr, count, rounds);
    memset(&gRsp, 0, sizeof(gRsp));
}
