/*
 * gfx_draw.c -- GE render state and triangles.
 *
 * A draw state (combiner, render mode, textures) becomes GE state when the
 * first triangle is drawn with it (prepare_3d_state): the combiner's fit
 * (gfx_combiner.c) picks the texture function and the texture to bind
 * (gfx_bind.c), the render mode the depth test, blending, alpha test and fog.
 * Triangles are collected in batches until the state changes, then drawn with
 * one sceGuDrawArray; their vertices are snapped to the PSP's pixel grid
 * first, so that meshes that meet leave no gaps.
 *
 * The GE projects the vertices itself (the projection matrix is uploaded by
 * gfx_frame.c) but drops any triangle that leaves its coordinate range or,
 * with depth clipping off, its depth range. So triangles are clipped here
 * against a guard band and the eye plane, and pieces in front of the near
 * plane or beyond the far plane -- which the N64's NoN microcode clamps per
 * pixel -- are drawn separately with their depth pinned to that plane.
 */
#include <pspkernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx_internal.h"

#define BATCH_MAX_VERTS 3 * 512
#define BATCH_MAX_INDICES 3 * 1024

/* ---- state -------------------------------------------------------------- */

GuCache gGu;

/* Fog as the RDP blends it (fog colour over the pixel by shade alpha) for
 * opaque geometry: a second pass of the same triangles in the fog colour,
 * with the RSP's per-vertex fog factor as alpha, at equal depth. */
static bool sFogPass = false;
static uint32_t sFogColor = 0;
CombinerFit gFit;
const GuTexture* gBatchTex = NULL;
static const GuTexture* sBatchTex2 = NULL; /* a split combiner's second texture (BAKE_Y), for its second pass */
static float sTraceZ, sTraceW; /* first vertex of the batch, for traces */

/* The draw state of the current batch, for traces. */
static struct {
    uint32_t cc0, cc1, prim, env, gm, oml, omh;
} sBatchState;

void gfx_gu_reset_cache(void) {
    /* -1 everywhere means "not known, send it" ... */
    memset(&gGu, 0xFF, sizeof(gGu));
    gGu.tex_pixels = NULL;
    /*
     * ... except for the blend colour, where all ones is opaque white, a colour
     * draws do ask for, and which would then never be sent. Fits always ask for
     * an opaque colour, so transparent black stands for unknown.
     */
    gGu.env = 0;
    gProjVariant = -1;
}

/* Points the GE at a texture's pixels. */
RT_SPRAM void gfx_gu_texture_image(const GuTexture* tex) {
    if (gGu.tex_pixels != tex->pixels || tex->buf_width != 0) {
        if (tex->swizzled != gGu.tex_swizzled || tex->psm != gGu.tex_psm) {
            sceGuTexMode(tex->psm, 0, 0, tex->swizzled);
            gGu.tex_swizzled = tex->swizzled;
            gGu.tex_psm = tex->psm;
        }
        sceGuTexImage(0, tex->gu_width, tex->gu_height, tex->buf_width ? tex->buf_width : tex->gu_width, tex->pixels);
        sceGuTexFlush();
        gGu.tex_pixels = tex->pixels;
    }
}

void gfx_gu_texturing(bool on) {
    int texture = on ? 1 : 0;
    if (texture != gGu.texture) {
        if (texture) sceGuEnable(GU_TEXTURE_2D);
        else sceGuDisable(GU_TEXTURE_2D);
        gGu.texture = texture;
    }
}

/* The GE texture function (and blend colour) for a fit. */
RT_SPRAM void gfx_gu_tex_func(const CombinerFit* fit) {
    int func;
    switch (fit->mode) {
        case TEX_BLEND: func = GU_TFX_BLEND; break;
        case TEX_ADD: func = GU_TFX_ADD; break;
        case TEX_DECAL: func = GU_TFX_DECAL; break;
        default: func = GU_TFX_MODULATE; break;
    }
    int tcc = (fit->tex_alpha || fit->mode == TEX_DECAL) ? GU_TCC_RGBA : GU_TCC_RGB;
    int key = func | (tcc << 8);
    if (key != gGu.tex_func) {
        sceGuTexFunc(func, tcc);
        gGu.tex_func = key;
    }
    if (fit->mode == TEX_BLEND && fit->env != gGu.env) {
        sceGuTexEnvColor(fit->env);
        gGu.env = fit->env;
    }
}

/* ---- batches ------------------------------------------------------------ */

/*
 * Triangles of the current draw state; pieces in front of the near plane or
 * beyond the far plane go to their own batches, drawn with pinned depth.
 *
 * A batch is a vertex array plus a list of indices into it, drawn with
 * GU_INDEX_16BIT: the game's triangles share each vertex about twice over, so
 * sending each one once costs the GE less to fetch and costs us two bytes per
 * corner instead of a whole vertex.
 */
typedef struct {
    GuVertex* verts;   /* BATCH_MAX_VERTS of room: in the arena, or sStaticVerts */
    GuVertex* fogv;    /* the same vertices in the fog colour (fog pass), same indices */
    bool in_arena;     /* verts/fogv are GE-visible memory: drawn from where they are */
    uint16_t idx[BATCH_MAX_INDICES];
    int nverts;
    int nidx;
    int nidx_up;       /* indices of upright triangles, kept at the end of idx (see sClassify) */
    uint8_t fog_max;
} Batch;

static Batch sBatches[3]; /* by DEPTH_* */
#define BATCH (&sBatches[DEPTH_NORMAL])

/*
 * Vertices are written straight into memory the GE reads them from. The
 * normal batch -- nearly every triangle -- takes BATCH_MAX_VERTS of room at
 * the top of a per-frame arena, fills it through the uncached mirror, and is
 * drawn from there; drawing commits only what it used. The fog pass' vertices
 * (same positions, the fog colour, shade alpha from the fog) are written
 * alongside while packing. This replaced building each batch in cached memory
 * and copying it into the display list at the draw, and then copying and
 * recolouring it all again for the fog pass.
 *
 * The arena starts over with each frame (gfx_open_frame): presenting waits for the
 * GE, so nothing still reads the last frame's vertices. The near/far depth
 * batches are rare and keep static arrays that are copied at the draw, as does
 * the normal batch if the arena ever runs out.
 */
#define ARENA_VERTS (16 * 1024)   /* 384 KB each; a village frame uses ~3000 */
static GuVertex sArenaVerts[ARENA_VERTS] __attribute__((aligned(64)));
static GuVertex sArenaFog[ARENA_VERTS] __attribute__((aligned(64)));
static uint32_t sArenaTop = 0;   /* in vertices; the fog arena mirrors it */
static bool sArenaReady = false;
static GuVertex sStaticVerts[3][BATCH_MAX_VERTS];
static GuVertex sStaticFog[3][BATCH_MAX_VERTS];

#define UNCACHED(p) ((GuVertex*)(void*)((uintptr_t)(p) | 0x40000000u))

static void batch_use_static(int i) {
    sBatches[i].verts = sStaticVerts[i];
    sBatches[i].fogv = sStaticFog[i];
    sBatches[i].in_arena = false;
}

/* Gives the (empty) normal batch its room at the top of the arena. */
static void arena_reserve(void) {
    Batch* b = &sBatches[DEPTH_NORMAL];
    if (sArenaReady && sArenaTop + BATCH_MAX_VERTS <= ARENA_VERTS) {
        b->verts = UNCACHED(&sArenaVerts[sArenaTop]);
        b->fogv = UNCACHED(&sArenaFog[sArenaTop]);
        b->in_arena = true;
    } else {
        batch_use_static(DEPTH_NORMAL);
    }
}

/* A new frame: the GE has finished with everything in the arena. */
void gfx_batches_new_frame(void) {
    if (!sArenaReady) {
        /* Only ever written through the uncached mirror from here on, so no
         * cache line may hold these addresses. */
        sceKernelDcacheWritebackInvalidateRange(sArenaVerts, sizeof(sArenaVerts));
        sceKernelDcacheWritebackInvalidateRange(sArenaFog, sizeof(sArenaFog));
        for (int i = 0; i < 3; i++) {
            batch_use_static(i);
        }
        sArenaReady = true;
    }
    sArenaTop = 0;
    if (sBatches[DEPTH_NORMAL].nverts == 0) {
        arena_reserve();
    }
}

/*
 * Vertices packed for the GE, one slot per loaded vertex, reused while the
 * state lasts: the shade colour in particular costs real work to work out.
 * sPackedStamp says the packed vertex still matches the render state;
 * sPackedBatch says it is already in the batch's vertex array, at sPackedIndex.
 */
static GuVertex sPacked[MAX_VERTICES];
static uint8_t sPackedFog[MAX_VERTICES];
static uint32_t sPackedStamp[MAX_VERTICES];
static uint32_t sPackedBatch[MAX_VERTICES];
static uint16_t sPackedIndex[MAX_VERTICES];
static uint32_t sBatchGen = 1;   /* render state generation */
static uint32_t sVertGen = 1;    /* vertex array generation (a flush starts a new one) */
/* Each vertex's snapped eye x,y: a vertex packed again for a new render state
 * is snapped (and welded) once. Valid while sSnapStamp == sSnapGen. */
static float sSnapX[MAX_VERTICES], sSnapY[MAX_VERTICES];
static uint32_t sSnapStamp[MAX_VERTICES];

RT_SPRAM void gfx_forget_packed(int first, int count) {
    for (int i = first; i < first + count; i++) {
        sPackedStamp[i] = 0;
        sPackedBatch[i] = 0;
        sSnapStamp[i] = 0;
    }
}

/* ---- vertex snapping ---------------------------------------------------- */

/*
 * The RSP hands the RDP screen positions in quarter pixels, and the RDP draws
 * every pixel a triangle covers any part of (it samples four sub-scanlines per
 * row). So neighbouring meshes whose edges nearly meet -- the village's ground
 * blocks, 0.08 pixel apart where they join -- never leave an empty pixel
 * between them on the N64. The GE samples one point per pixel, its centre, and
 * a sliver of a pixel between two edges can hold a PSP pixel centre: a line of
 * background across the screen, coming and going as the camera moves.
 *
 * So vertices are snapped to a grid, and vertices that nearly coincide are
 * welded: rounded on their own, two vertices 0.08 pixel apart land on
 * different grid points some of the time, and the snap then *widens* the
 * sliver to a grid step (a line of blue across the grass on the PSP-1000).
 * Every vertex sent in a task is remembered with its grid point, and one
 * within WELD_RADIUS of an earlier one takes that one's grid point instead of
 * its own, if it is at the same depth: two meshes meeting at a point agree on
 * its depth, something in front of them does not. The radius has to cover the
 * beach, where blocks meet 0.15-0.18 N64 pixel apart (a dotted line down the
 * grass and sand), and a diagonal join in the village whose sides are 0.23 N64
 * pixel (0.35 PSP pixel) apart, 0.51 PSP pixel at one corner.
 *
 * The grid is the PSP's, not the N64's: half a PSP pixel apart, at 5/16 and
 * 13/16 of each pixel (SNAP_PHASE). Welded vertices are not equal on the
 * hardware. The GE rounds screen positions to 1/16 pixel, and its own
 * transform put the two sides of an acre join 0.03-0.09 pixel apart although
 * they were sent to the same point (the village's coordinates are in the
 * thousands; PPSSPP, which rasterises on the host's GPU, shows none of this).
 * So a shared edge can come out one 1/16 step apart, and when the GE's sample
 * point -- between 9/16 and 10/16 down the pixel, as measured by shifting the
 * picture a sixteenth at a time -- is in that step, the row belongs to
 * neither side: on the N64's quarter-pixel grid, which falls anywhere on the
 * PSP's, the join at y = 269.595 went to 269.5625 above and 269.625 below
 * and left row 269 blue. The grid points here are a quarter pixel from the
 * sample points on both sides.
 *
 * The GE projects the eye-space position itself (GU_TRANSFORM_3D), so the snap
 * is applied as a nudge in eye space: the 2x2 Jacobian of the screen position
 * with respect to eye x and y is inverted for each vertex. The nudge is at most
 * a quarter of a pixel, so the linearisation is exact to well under that. It
 * has to be general: in the village AF folds the camera into the projection,
 * so eye y feeds clip w too (P[1][3] != 0), and a nudge that assumed a plain
 * perspective left the village unsnapped -- a line of blue across the ground.
 */
#define SNAP_PHASE 0.3125f /* grid points are at SNAP_PHASE + n / 2 PSP pixels */
static struct {
    bool ok;
    float hw, tx, hh, ty;   /* grid steps / 4 = ndc * hw + tx, ty - ndc * hh */
    float p00, p10, p01, p11, p03, p13; /* how eye x and y feed clip x, y and w */
} sSnap;

/*
 * The weld table: an open-addressed hash of grid point -> the vertices sent
 * to it this task, 8 bytes each, and a bitmap of the grid points in use (mod
 * 128), so that a vertex with no earlier one nearby costs nine bit tests and
 * an insert. It has to stay small and cheap. On the PSP-1000 a data cache miss
 * costs ~200 ns, floorf (a library call) ~190 ns, this branchy code runs at
 * 2-3 cycles an instruction, and the triangle code does not fit in the
 * instruction cache: welding every vertex cost 0.8-1.3 ms a frame in the
 * village, whatever the table looked like. Only the corners of big triangles
 * are welded (WELD_MIN_EXTENT) -- seams run along the joins of large meshes,
 * the ground -- which in the frame of the seam above is 240 of 542 vertices
 * and ~0.4 ms. no_weld.txt turns welding off.
 */
#define WELD_SLOTS 1024        /* a power of two; the village draws ~550 vertices a task */
#define WELD_MAX_FILL 768
#define WELD_OFS_BITS 6        /* positions in 1/64 grid step */
#define WELD_RADIUS 80         /* in 1/64 grid step: 0.63 PSP pixel */
#define WELD_EMPTY INT16_MIN
#define WELD_MIN_EXTENT 0.6f  /* sum of a triangle's x and y edge extents, in NDC (~50 px) */
typedef struct {
    int16_t gx, gy;            /* grid point, in grid steps (WELD_EMPTY: free) */
    int8_t ox, oy;             /* the vertex, unsnapped: its offset from the grid point */
    uint16_t w;                /* clip w in 1/4 */
} WeldSlot;
static WeldSlot sWeld[WELD_SLOTS] __attribute__((aligned(64)));
static uint32_t sWeldUsed[128 * 128 / 32] __attribute__((aligned(64)));
static int sWeldFill = -1;     /* -1: the table needs clearing */
static bool sWeldOff;
static bool sUprightOff;       /* no_upright.txt: the stencil doesn't keep what stands upright (see sStencilClass) */
/* Snapped positions are cached per vertex (pack_vertex) while this stays the
 * same: it changes with the viewport, the projection and every task. */
static uint32_t sSnapGen = 1;

/* Forgets every vertex: a new task. */
void gfx_weld_reset(void) {
    if (sWeldFill != 0) {
        for (int i = 0; i < WELD_SLOTS; i++) {
            sWeld[i].gx = WELD_EMPTY;
        }
        memset(sWeldUsed, 0, sizeof(sWeldUsed));
        sWeldFill = 0;
    }
    sWeldOff = RT_SWITCH("no_weld.txt");
    sUprightOff = RT_SWITCH("no_upright.txt");
    sSnapGen++;
}

/* The top bits of a multiplicative hash (its low bits depend only on the low
 * bits of the grid point). */
static inline uint32_t weld_slot(int gx, int gy) {
    return ((uint32_t)gx * 0x9E3779B1u ^ (uint32_t)gy * 0x85EBCA77u) >> (32 - 10);
}
_Static_assert(WELD_SLOTS == 1 << 10, "weld_slot makes 10 bits");

static inline uint32_t weld_bit(int gx, int gy) {
    uint32_t cell = (uint32_t)(gy & 127) << 7 | (uint32_t)(gx & 127);
    return (sWeldUsed[cell >> 5] >> (cell & 31)) & 1;
}

/* Grid point (gx, gy) and the eight around it, its own first. */
static const int8_t kWeldNear[9][2] = {
    { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
};

/* Is any earlier vertex at (gx, gy) or a grid point next to it? */
static inline bool weld_near(int gx, int gy) {
    uint32_t any = 0;
    for (int k = 0; k < 9; k++) {
        any |= weld_bit(gx + kWeldNear[k][0], gy + kWeldNear[k][1]);
    }
    return any != 0;
}

/*
 * The nearest earlier vertex within WELD_RADIUS at depth wq (+-dw) of a vertex
 * at offset (fx, fy) from grid point (gx, gy): its grid point in *bx, *by. 0
 * if there is none, 1 if there is, 2 if it is this very vertex, sent before.
 * Such a vertex is at (gx, gy) or at a grid point next to it.
 */
RT_SPRAM static __attribute__((noinline)) int weld_search(int gx, int gy, int fx, int fy, int wq, int dw, int* bx, int* by) {
    int best = WELD_RADIUS + 1, found = 0;
    for (int k = 0; k < 9; k++) {
        int ix = kWeldNear[k][0], iy = kWeldNear[k][1];
        int cx = gx + ix, cy = gy + iy;
        if (!weld_bit(cx, cy)) {
            continue;
        }
        /* this vertex's offset from that grid point */
        int ox = fx - ix * (1 << WELD_OFS_BITS), oy = fy - iy * (1 << WELD_OFS_BITS);
        for (uint32_t i = weld_slot(cx, cy);; i = (i + 1) & (WELD_SLOTS - 1)) {
            const WeldSlot* e = &sWeld[i];
            if (e->gx == WELD_EMPTY) {
                break;
            }
            if (e->gx != cx || e->gy != cy) {
                continue;
            }
            int ddw = e->w - wq;
            if (ddw > dw || ddw < -dw) {
                continue;
            }
            int dx = e->ox - ox, dy = e->oy - oy;
            dx = dx < 0 ? -dx : dx;
            dy = dy < 0 ? -dy : dy;
            int d = dx > dy ? dx : dy;
            if (d == 0 && ddw == 0) {
                *bx = cx;
                *by = cy;
                return 2;
            }
            if (d < best) {
                best = d;
                found = 1;
                *bx = cx;
                *by = cy;
            }
        }
    }
    return found;
}

/*
 * The grid point for a vertex at (qx, qy) grid steps (|q| < 32000) and
 * clip w: its own, or that of the nearest earlier vertex within WELD_RADIUS at
 * the same depth (within 1/128).
 */
static bool sWeldTri; /* the triangle being drawn is big enough to weld its corners */
static inline void weld_grid_point(float qx, float qy, float w, int* out_x, int* out_y) {
    /* floor(q + 0.5) without floorf */
    float rx = qx + 0.5f, ry = qy + 0.5f;
    int gx = (int)rx, gy = (int)ry;
    gx -= (float)gx > rx;
    gy -= (float)gy > ry;
    *out_x = gx;
    *out_y = gy;
    if (!sWeldTri || sWeldOff || sWeldFill >= WELD_MAX_FILL || !(w > 0.0f && w < 16000.0f)) {
        return;
    }
    /* the offset from the grid point, in 1/64 grid step: -32 .. 32 */
    int fx = (int)((qx - (float)gx) * (1 << WELD_OFS_BITS)), fy = (int)((qy - (float)gy) * (1 << WELD_OFS_BITS));
    int wq = (int)(w * 4.0f);
    int bx = gx, by = gy;
    if (weld_near(gx, gy)) {
        /* Most of these are this very vertex again (the same position in
         * another vertex slot): look for it at its own grid point first. */
        if (weld_bit(gx, gy)) {
            for (uint32_t i = weld_slot(gx, gy); sWeld[i].gx != WELD_EMPTY; i = (i + 1) & (WELD_SLOTS - 1)) {
                const WeldSlot* e = &sWeld[i];
                if (e->gx == gx && e->gy == gy && e->ox == fx && e->oy == fy && e->w == wq) {
                    return;
                }
            }
        }
        int r = weld_search(gx, gy, fx, fy, wq, (wq >> 7) + 1, &bx, &by);
        *out_x = bx;
        *out_y = by;
        if (r == 2) {
            return;
        }
        if (gTracing && r) {
            rt_log("      weld psp (%.3f,%.3f w%.1f) to (%.4f,%.4f)", qx * 0.5f + SNAP_PHASE, qy * 0.5f + SNAP_PHASE, w,
                   bx * 0.5f + SNAP_PHASE, by * 0.5f + SNAP_PHASE);
        }
    }
    uint32_t i = weld_slot(bx, by);
    while (sWeld[i].gx != WELD_EMPTY) {
        i = (i + 1) & (WELD_SLOTS - 1);
    }
    /* offsets from the grid point taken: within +-96, as bx, by are at most one step away */
    sWeld[i] = (WeldSlot){ (int16_t)bx, (int16_t)by, (int8_t)(fx - (bx - gx) * (1 << WELD_OFS_BITS)),
                           (int8_t)(fy - (by - gy) * (1 << WELD_OFS_BITS)), (uint16_t)wq };
    uint32_t cell = (uint32_t)(by & 127) << 7 | (uint32_t)(bx & 127);
    sWeldUsed[cell >> 5] |= 1u << (cell & 31);
    sWeldFill++;
}

/* no_snap.txt turns the snap off, no_weld.txt just the welding. */
void gfx_update_snap(void) {
    const float (*p)[4] = (const float (*)[4])gRsp.proj;
    /* PSP pixel p is grid step (p - SNAP_PHASE) * 2; sSnap counts in quarter steps. */
    int cx, cy, w, h;
    gfx_ge_viewport(&cx, &cy, &w, &h);
    float hw = (float)w * 0.25f, hh = (float)h * 0.25f;
    float tx = ((float)cx - SNAP_PHASE) * 0.5f, ty = ((float)cy - SNAP_PHASE) * 0.5f;
    if (hw != sSnap.hw || hh != sSnap.hh || tx != sSnap.tx || ty != sSnap.ty || p[0][0] != sSnap.p00 ||
        p[1][0] != sSnap.p10 || p[0][1] != sSnap.p01 || p[1][1] != sSnap.p11 || p[0][3] != sSnap.p03 ||
        p[1][3] != sSnap.p13) {
        sSnapGen++;
    }
    sSnap.hw = hw;
    sSnap.hh = hh;
    sSnap.tx = tx;
    sSnap.ty = ty;
    sSnap.p00 = p[0][0];
    sSnap.p10 = p[1][0];
    sSnap.p01 = p[0][1];
    sSnap.p11 = p[1][1];
    sSnap.p03 = p[0][3];
    sSnap.p13 = p[1][3];
    sSnap.ok = sSnap.hw != 0.0f && sSnap.hh != 0.0f && !RT_SWITCH("no_snap.txt");
}

/* Nudges an eye-space x,y so the vertex lands on grid point (gx, gy). */
static void snap_to(float ndc_x, float ndc_y, float cw, int gx, int gy, float* ex, float* ey) {
    float dx = (float)gx * 0.25f - (ndc_x * sSnap.hw + sSnap.tx);
    float dy = (float)gy * 0.25f - (sSnap.ty - ndc_y * sSnap.hh);
    /* d(clip/w)/d(eye) * w^2 for x and y; the screen Jacobian is these times hw and -hh. */
    float cx = ndc_x * cw, cy = ndc_y * cw;
    float a = sSnap.p00 * cw - cx * sSnap.p03; /* ndc x per eye x */
    float b = sSnap.p10 * cw - cx * sSnap.p13; /* ndc x per eye y */
    float c = sSnap.p01 * cw - cy * sSnap.p03; /* ndc y per eye x */
    float d = sSnap.p11 * cw - cy * sSnap.p13; /* ndc y per eye y */
    float det = a * d - b * c;
    if (det > -1e-12f && det < 1e-12f) {
        return;
    }
    /* Solve [hw*a hw*b; -hh*c -hh*d] / w^2 * (ex', ey') = (dx, dy). */
    float nx = dx / sSnap.hw, ny = -dy / sSnap.hh; /* wanted change in ndc */
    float k = cw * cw / det;
    *ex += (d * nx - b * ny) * k;
    *ey += (a * ny - c * nx) * k;
}

/* Snaps (and welds) a vertex. Out of line: see the weld table. */
RT_SPRAM static __attribute__((noinline)) void snap_vertex(float ndc_x, float ndc_y, float cw, float* ex, float* ey) {
    if (!sSnap.ok || cw <= 0.0f) {
        return;
    }
    float qx = (ndc_x * sSnap.hw + sSnap.tx) * 4.0f, qy = (sSnap.ty - ndc_y * sSnap.hh) * 4.0f;
    if (!(qx > -32000.0f && qx < 32000.0f && qy > -32000.0f && qy < 32000.0f)) {
        return; /* far off screen */
    }
    int gx, gy;
    weld_grid_point(qx, qy, cw, &gx, &gy);
    snap_to(ndc_x, ndc_y, cw, gx, gy, ex, ey);
}

/*
 * Everything about snapping a vertex that does not depend on its weld, worked
 * out for four vertices at a time on the VFPU when a G_VTX loads them (the scalar
 * code above is a chain of dependent float operations, ~500 cycles a vertex).
 * Per vertex: its position in grid steps (qx, qy), whether snapping applies, the
 * task's snap generation, and the 2x2 matrix that turns the grid correction
 * (gx - qx, gy - qy) into an eye-space nudge:
 *   ex += e11 * (gx - qx) + e12 * (gy - qy),  ey += e21 * (gx - qx) + e22 * (gy - qy)
 * which is snap_to with its division folded in.
 */
typedef struct {
    float qx, qy, ok;
    uint32_t gen;
    float e11, e12, e21, e22;
} SnapPre;
static SnapPre sSnapPre[MAX_VERTICES + 4] __attribute__((aligned(64)));
_Static_assert(sizeof(SnapPre) == 32 && offsetof(SnapPre, e11) == 16, "gfx_snap_precompute stores two quads");

RT_SPRAM void gfx_snap_precompute(const RspVertex* verts, int start, int count) {
    if (!sSnap.ok) {
        return;
    }
    static float consts[16] __attribute__((aligned(16)));
    consts[0] = 4.0f * sSnap.hw;
    consts[1] = 4.0f * sSnap.tx;
    consts[2] = 4.0f * sSnap.hh;
    consts[3] = 4.0f * sSnap.ty;
    consts[4] = sSnap.p00;
    consts[5] = sSnap.p10;
    consts[6] = sSnap.p01;
    consts[7] = sSnap.p11;
    consts[8] = sSnap.p03;
    consts[9] = sSnap.p13;
    consts[10] = 0.25f / sSnap.hw;
    consts[11] = 0.25f / sSnap.hh;
    consts[12] = 1.0e-12f;
    consts[13] = 32000.0f;
    consts[14] = 0.0f;
    consts[15] = 0.0f;
    __asm__ volatile(
        "lv.q C500,  0(%0)\n"
        "lv.q C510, 16(%0)\n"
        "lv.q C520, 32(%0)\n"
        "lv.q C530, 48(%0)\n"
        :
        : "r"(consts)
        : "memory");
    for (int i = start; i < start + count; i += 4) {
        const RspVertex* v = &verts[i];
        SnapPre* out = &sSnapPre[i];
        uint32_t gen = sSnapGen;
        __asm__ volatile(
            /* rows of M000 / M100 are the four vertices: the columns are cx[4], cy[4], cw[4] / nx[4], ny[4] */
            "lv.q R000,   0(%0)\n"
            "lv.q R001,  64(%0)\n"
            "lv.q R002, 128(%0)\n"
            "lv.q R003, 192(%0)\n"
            "lv.q R100,  16(%0)\n"
            "lv.q R101,  80(%0)\n"
            "lv.q R102, 144(%0)\n"
            "lv.q R103, 208(%0)\n"
            "vscl.q C200, C100, S500\n"                      /* qx = nx * 4hw + 4tx */
            "vscl.q C210, C110, S502\n"                      /* qy = 4ty - ny * 4hh */
            "vscl.q C300, C030, S510\n"                      /* a = p00 cw - cx p03 */
            "vscl.q C400, C000, S520\n"
            "vscl.q C310, C030, S511\n"                      /* b = p10 cw - cx p13 */
            "vscl.q C410, C000, S521\n"
            "vscl.q C320, C030, S512\n"                      /* c = p01 cw - cy p03 */
            "vscl.q C420, C010, S520\n"
            "vscl.q C330, C030, S513\n"                      /* d = p11 cw - cy p13 */
            "vscl.q C430, C010, S521\n"
            "vadd.q C200, C200, C500[y,y,y,y]\n"
            "vsub.q C210, C500[w,w,w,w], C210\n"
            "vsub.q C300, C300, C400\n"
            "vsub.q C310, C310, C410\n"
            "vsub.q C320, C320, C420\n"
            "vsub.q C330, C330, C430\n"
            "vmul.q C400, C300, C330\n"                      /* det = a d - b c */
            "vmul.q C410, C310, C320\n"
            "vsub.q C400, C400, C410\n"
            "vrcp.q C410, C400\n"
            "vmul.q C420, C030, C030\n"                      /* k = cw^2 / det */
            "vmul.q C420, C420, C410\n"
            "vscl.q C430, C420, S522\n"                      /* k / 4hw (as 0.25/hw) */
            "vscl.q C420, C420, S523\n"
            "vmul.q C600, C330, C430\n"                      /* e11 = d k e1 */
            "vmul.q C610, C310, C420\n"                      /* e12 = b k e2 */
            "vmul.q C620, C320[-x,-y,-z,-w], C430\n"         /* e21 = -c k e1 */
            "vmul.q C630, C300[-x,-y,-z,-w], C420\n"         /* e22 = -a k e2 */
            /* ok = cw > 0 and |det| > eps and |qx|, |qy| < limit */
            "vzero.q C700\n"
            "vslt.q C710, C700, C030\n"
            "vabs.q C720, C400\n"
            "vslt.q C720, C530[x,x,x,x], C720\n"
            "vmul.q C710, C710, C720\n"
            "vabs.q C720, C200\n"
            "vslt.q C720, C720, C530[y,y,y,y]\n"
            "vmul.q C710, C710, C720\n"
            "vabs.q C720, C210\n"
            "vslt.q C720, C720, C530[y,y,y,y]\n"
            "vmul.q C220, C710, C720\n"
            "mtv %2, S230\n"
            "mtv %2, S231\n"
            "mtv %2, S232\n"
            "mtv %2, S233\n"
            "sv.q R200,  0(%1)\n"
            "sv.q R201, 32(%1)\n"
            "sv.q R202, 64(%1)\n"
            "sv.q R203, 96(%1)\n"
            "sv.q R600, 16(%1)\n"
            "sv.q R601, 48(%1)\n"
            "sv.q R602, 80(%1)\n"
            "sv.q R603, 112(%1)\n"
            :
            : "r"(v), "r"(out), "r"(gen)
            : "memory");
    }
}

/* The scalar snap, with the VFPU's precomputed matrix when it is there (see SnapPre). */
static void snap_vertex_pre(const RspVertex* v, int index, float* ex, float* ey) {
    const SnapPre* pre = &sSnapPre[index];
    if (pre->gen != sSnapGen || !(pre->ok > 0.5f)) {
        snap_vertex(v->nx, v->ny, v->cw, ex, ey);
        return;
    }
    int gx, gy;
    weld_grid_point(pre->qx, pre->qy, v->cw, &gx, &gy);
    float dx = (float)gx - pre->qx, dy = (float)gy - pre->qy;
    *ex += pre->e11 * dx + pre->e12 * dy;
    *ey += pre->e21 * dx + pre->e22 * dy;
}

/* ---- render state ------------------------------------------------------- */

/*
 * The blender's last cycle is P * 0 + MEM * 1: the pixel keeps the colour it
 * has. AF uses such passes to rewrite only the coverage bits (the pocket
 * screen's portrait: I8 rectangles, alpha from coverage), which the port
 * doesn't keep; drawn as ordinary rectangles they covered the portrait black.
 *
 * The same goes for P * 0 + BLEND * memory alpha, which turns a picture into
 * its coverage values (PreRender, see gfx_coverage_rect): there are none here,
 * and the picture is left as it is.
 */
static bool blender_keeps_memory(void) {
    uint32_t cycle = rdp_cycle_type();
    if (!(gRdp.other_l & FORCE_BL) || cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
        return false;
    }
    int a = rdp_blend_last(BL_SHIFT_A), m = rdp_blend_last(BL_SHIFT_M), b = rdp_blend_last(BL_SHIFT_B);
    return a == 3 && ((m == 1 && b == 2) || (m == 2 && b == 1));
}

void gfx_other_mode_changed(void) {
    gRdp.keeps_memory = blender_keeps_memory();
    gRdp.state_dirty = true;
}

static bool sAaEdge = false; /* an antialiased texture edge, blended by its alpha (see gfx_apply_aa_edge) */

/* Is the render mode an antialiased texture edge (see gfx_apply_aa_edge)? The
 * texture is bound with TEXVAR_BLEED for these, in case it gets blended.
 * hard_edges.txt: never. */
RT_SPRAM bool gfx_aa_edge_mode(void) {
    bool hard_edges = RT_SWITCH("hard_edges.txt");
    uint32_t l = gRdp.other_l;
    uint32_t cycle = rdp_cycle_type();
    uint32_t aa_bits = AA_EN | IM_RD | CVG_X_ALPHA | ALPHA_CVG_SEL;
    bool z_writes = (l & Z_UPD) && (gRsp.geometry_mode & G_ZBUFFER);
    return !hard_edges && !rdp_blends_by_alpha() && !z_writes && (l & aa_bits) == aa_bits && !(l & FORCE_BL) &&
           (l & 3) == 0 && cycle != G_CYC_FILL && cycle != G_CYC_COPY;
}

/* The blend function while gGu.blend is on. */
static void gu_blend_func(void) {
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

/* Alpha blending on or off (gGu.blend). */
static inline void gu_blending(bool on) {
    if ((int)on != gGu.blend) {
        if (on) {
            sceGuEnable(GU_BLEND);
            gu_blend_func();
        } else {
            sceGuDisable(GU_BLEND);
        }
        gGu.blend = on;
    }
}

void gfx_gu_blending(bool on) {
    gu_blending(on);
}

/*
 * A decal on the ground (a shadow) also passes the depth test on whatever
 * stands in it, near its foot: a tree trunk, the legs of the notice board, a
 * wall. The RDP darkens a row or two of those as well; without its blur that
 * shows as dark pixels at their feet. So every surface that writes depth to
 * the screen also writes what it is to the stencil (the screen's alpha, not
 * used otherwise): upright or not, by the way its depth runs on the screen.
 * A decal that lies flat leaves upright surfaces alone.
 *
 * The triangles of an opaque batch are sorted for this, the upright ones to
 * the end of the batch's indices, and drawn as two ranges. About 0.3-0.5 ms a
 * frame on the PSP. The functions for it are kept out of line: inlined into
 * the batch and triangle code they cost 0.7 ms more, even switched off.
 * no_upright.txt: none of this.
 */
#define STENCIL_UPRIGHT 0x2A   /* a value the cut passes don't leave behind */
static bool sStencilClass = false; /* the batch writes depth to the screen: the stencil gets its class */
static bool sClassify = false;     /* ... and its triangles are sorted by class (opaque) */
static bool sDecalFlat = true;     /* every triangle of the decal batch lies flat */

bool gfx_stencil_classes(void) {
    return !sUprightOff;
}

RT_SPRAM static __attribute__((noinline)) void gu_stencil(int mode) {
    if (mode == gGu.stencil) {
        return;
    }
    if (mode == GU_STENCIL_OFF) {
        sceGuDisable(GU_STENCIL_TEST);
    } else {
        if (gGu.stencil <= GU_STENCIL_OFF) {
            sceGuEnable(GU_STENCIL_TEST);
        }
        bool writes = gGu.stencil == GU_STENCIL_FLAT || gGu.stencil == GU_STENCIL_UPRIGHT;
        if (mode == GU_STENCIL_KEEP) {
            sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP); /* (the test stays as it was: it passes, or it is unknown) */
            if (!writes) {
                sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
            }
        } else if (mode == GU_STENCIL_DECAL) {
            sceGuStencilFunc(GU_NOTEQUAL, STENCIL_UPRIGHT, 0xFF);
            if (gGu.stencil != GU_STENCIL_KEEP) {
                sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
            }
        } else {
            if (!writes) {
                sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
            }
            sceGuStencilFunc(GU_ALWAYS, mode == GU_STENCIL_UPRIGHT ? STENCIL_UPRIGHT : 0, 0xFF);
        }
    }
    gGu.stencil = mode;
}

/* (a render target's stencil is its coverage, and its own business) */
void gfx_gu_stencil_off(void) {
    if (!gTarget.bound) {
        gu_stencil(GU_STENCIL_OFF);
    }
}

/* Does the triangle stand upright? Up the screen it does not get farther
 * away as the ground does: it comes nearer, or its distance changes sideways
 * more than upward. (1/w is linear on the screen; the differences below are
 * those of 1/w times the product of the three w, which is positive.) */
RT_SPRAM static __attribute__((noinline)) bool tri_upright(const RspVertex* v0, const RspVertex* v1, const RspVertex* v2) {
    if (v0->cw <= 0 || v1->cw <= 0 || v2->cw <= 0) {
        return false;
    }
    float x1 = v1->nx - v0->nx, y1 = v1->ny - v0->ny;
    float x2 = v2->nx - v0->nx, y2 = v2->ny - v0->ny;
    float q1 = (v0->cw - v1->cw) * v2->cw;
    float q2 = (v0->cw - v2->cw) * v1->cw;
    float dqdx = q1 * y2 - q2 * y1, dqdy = x1 * q2 - x2 * q1;
    if (x1 * y2 - x2 * y1 < 0) {
        dqdy = -dqdy;
    }
    return dqdy >= -0.25f * fabsf(dqdx); /* 1/w falls with distance */
}

/*
 * Decals (ZMODE_DEC: shadows, the light from a door) lie on or near the
 * surface they mark. The RDP draws them where their depth is within a
 * tolerance of the surface's: the depth a pixel spans (|dz/dx| + |dz/dy|),
 * rounded up to a power of two and doubled, so one to two pixels' worth. Here
 * they are pulled toward the eye instead (gGu.decal is the offset in use), by
 * two pixels' worth (DECAL_SLOPES): snapping moves vertices on the screen and
 * not in depth, which shifts the depth of a surface at a pixel by up to
 * another 0.7 of a pixel's.
 *
 * A fixed offset of 32 was one pixel's worth in the village. The police
 * station's door light lies on the ground, 0.9 of that under the top of the
 * doorstep: part of the step was left unlit, along its diagonal or a jagged
 * line, differently at every camera position.
 */
#define DECAL_MIN_OFFSET 32 /* depth buffer units */
#define DECAL_SLOPES 2.0f
static bool sDecal = false;                 /* the batch is a decal */
static int sDecalOffset = DECAL_MIN_OFFSET; /* the largest tolerance of its triangles */

/* The offset a decal triangle needs, in depth buffer units. */
static int decal_tolerance(const RspVertex* v0, const RspVertex* v1, const RspVertex* v2) {
    if (v0->cw <= 0 || v1->cw <= 0 || v2->cw <= 0) {
        return DECAL_MIN_OFFSET;
    }
    /* N64 pixels and the RDP's 15-bit depth (viewport scale 511, 5 fraction bits) */
    float x1 = (v1->nx - v0->nx) * (N64_SCREEN_W / 2), y1 = (v1->ny - v0->ny) * (N64_SCREEN_H / 2);
    float x2 = (v2->nx - v0->nx) * (N64_SCREEN_W / 2), y2 = (v2->ny - v0->ny) * (N64_SCREEN_H / 2);
    float z0 = v0->cz / v0->cw;
    float z1 = (v1->cz / v1->cw - z0) * (511 * 32), z2 = (v2->cz / v2->cw - z0) * (511 * 32);
    float area = x1 * y2 - x2 * y1;
    if (fabsf(area) < 1e-3f) {
        return DECAL_MIN_OFFSET;
    }
    float dzdx = fabsf((z1 * y2 - z2 * y1) / area), dzdy = fabsf((x1 * z2 - x2 * z1) / area);
    float tolerance = (dzdx + dzdy) * DECAL_SLOPES * 2; /* 15 bits of depth to the PSP's 16 */
    return tolerance < 0xFFFF ? (int)tolerance : 0xFFFF;
}

RT_SPRAM void gfx_apply_render_state(bool depth_allowed) {
    uint32_t l = gRdp.other_l;
    bool zbuf = depth_allowed && (gRsp.geometry_mode & G_ZBUFFER) != 0;
    int depth_test = zbuf && (l & Z_CMP);
    int depth_mask = zbuf && (l & Z_UPD);
    int decal = zbuf && (l & ZMODE_DEC) == ZMODE_DEC;

    if (depth_test != gGu.depth_test) {
        if (depth_test) {
            sceGuEnable(GU_DEPTH_TEST);
            sceGuDepthFunc(GU_GEQUAL);
        } else {
            sceGuDisable(GU_DEPTH_TEST);
        }
        gGu.depth_test = depth_test;
    }
    if (depth_mask != gGu.depth_mask) {
        /* sceGuDepthMask(1) disables writes */
        sceGuDepthMask(depth_mask ? 0 : 1);
        gGu.depth_mask = depth_mask;
    }
    /* a decal's offset is set with its triangles (see decal_tolerance) */
    sDecal = decal;
    if (!decal && gGu.decal != 0) {
        sceGuDepthOffset(0);
        gGu.decal = 0;
    }
    int fog = 0;
    int blend = rdp_blends_by_alpha();
    /* an antialiased texture edge, maybe (gfx_apply_aa_edge decides once the texture is known) */
    sAaEdge = gfx_aa_edge_mode();
    sFogPass = false;
    /* the first cycle blends the fog colour by the shade alpha */
    bool fogged = depth_allowed && (gRsp.geometry_mode & G_FOG) && ((l >> BL_SHIFT_P) & 3) == 3 &&
                  ((l >> BL_SHIFT_A) & 3) == 2;
    if (fogged && depth_mask && !blend) {
        /* opaque: exact per-vertex fog in a second pass (see sFogPass) */
        sFogPass = true;
        sFogColor = rgb32_to_ge(gRdp.fog);
    } else if (fogged) {
        /* translucent: the GE's linear fog as an approximation */
        float fnear, ffar;
        if (gfx_compute_fog_range(&fnear, &ffar)) {
            fog = 1;
            sceGuFog(fnear, ffar, rgb32_to_ge(gRdp.fog));
        }
    }
    if (fog != gGu.fog) {
        if (fog) sceGuEnable(GU_FOG);
        else sceGuDisable(GU_FOG);
        gGu.fog = fog;
    }
    int variant = fog ? PROJ_FOG : (!depth_test && !depth_mask) ? PROJ_FLAT_Z : PROJ_NORMAL;
    if (variant != gProjVariant) {
        gfx_upload_projection(variant);
    }

    sStencilClass = depth_mask && depth_allowed && !gTarget.bound && !sUprightOff;
    sClassify = sStencilClass && depth_test && !blend && !decal;

    gu_blending(blend);

    int alpha_ref = -1;
    if ((l & 3) == G_AC_THRESHOLD) {
        alpha_ref = (int)(gRdp.blend & 0xFF);
        if (alpha_ref > 0) alpha_ref--;
    } else if ((l & 3) == G_AC_DITHER) {
        alpha_ref = 0x3F;
    } else if ((l & CVG_X_ALPHA) && !blend) {
        alpha_ref = 0x7F;
    }
    int alpha_test = alpha_ref >= 0;
    if (alpha_test != gGu.alpha_test || (alpha_test && alpha_ref != gGu.alpha_ref)) {
        if (alpha_test) {
            sceGuEnable(GU_ALPHA_TEST);
            sceGuAlphaFunc(GU_GREATER, alpha_ref, 0xFF);
        } else {
            sceGuDisable(GU_ALPHA_TEST);
        }
        gGu.alpha_test = alpha_test;
        gGu.alpha_ref = alpha_ref;
    }
}

/*
 * A two-texture combiner whose colour is one tile's and whose alpha is the
 * other tile's alone (a colour image cut out by a separate mask) needs no
 * bake when the pixel isn't blended: the alpha only decides, through the
 * alpha test, which pixels are drawn. The name-entry window is this (a
 * scrolling pattern inside fixed window pieces); baked, every scroll step was
 * a new texture per piece -- more than the cache holds, so it never settled.
 * Instead the batch is drawn in passes (draw_cut): the mask tile into the
 * stencil (the framebuffer's alpha), then the colour tile where it was set,
 * each from its own cached texture. Without an alpha test the alpha does
 * nothing and the colour tile is simply drawn on its own.
 *
 * Either tile may be the mask. The pocket screen's windows are the other way
 * round from the name-entry one -- TEXEL0 is the polka dots, which scroll a
 * texel every three frames, TEXEL1 the window's shape -- and were baked for
 * that reason: three new textures per window and step, 10 ms each on the
 * PSP, most of them put off by the bake budget and the pattern drawn stale.
 */
static const GuTexture* sCutTex = NULL; /* the colour tile, drawn through the batch texture's alpha */
static float sCutScaleU, sCutScaleV, sCutOffU, sCutOffV; /* its UVs from the batch's */

/* The baked alpha is tile `base`'s own alpha (0: TEXEL0, 1: TEXEL1), whatever the other's. */
static bool alpha_is_tile_probe(int base) {
    static const uint8_t v[] = { 0, 1, 63, 127, 128, 191, 254, 255 };
    for (unsigned i = 0; i < sizeof(v); i++) {
        for (unsigned j = 0; j < sizeof(v); j++) {
            uint8_t a = base == 0 ? gfx_bake_alpha(v[i], v[j]) : gfx_bake_alpha(v[j], v[i]);
            if (a != v[i]) {
                return false;
            }
        }
    }
    return true;
}

/* The same, remembered per combiner state: a window is a dozen draws, each of which asks. */
static bool alpha_is_tile(int base) {
    static struct {
        CombinerState state;
        bool known[2], is_tile[2];
    } sMemo[8];
    static unsigned sNext = 0;
    CombinerState state = rdp_combiner_state();
    unsigned at = 8;
    for (unsigned i = 0; i < 8; i++) {
        if (combiner_state_equal(&sMemo[i].state, &state)) {
            at = i;
            break;
        }
    }
    if (at == 8) {
        at = sNext++ & 7;
        sMemo[at].state = state;
        sMemo[at].known[0] = sMemo[at].known[1] = false;
    }
    if (!sMemo[at].known[base]) {
        sMemo[at].known[base] = true;
        sMemo[at].is_tile[base] = alpha_is_tile_probe(base);
    }
    return sMemo[at].is_tile[base];
}

static bool bind_cut(void) {
    if (RT_SWITCH("no_cut.txt") || !gFit.combine2 || gFit.split || gTarget.bound || gGu.blend || sFogPass) {
        return false;
    }
    int colour_tile = gFit.bake_rgb_tile, mask_tile = colour_tile ^ 1;
    bool mask = gGu.alpha_test;
    if (mask && !alpha_is_tile(mask_tile)) {
        return false;
    }
    CombinerFit plain = gFit;
    plain.combine2 = false;
    plain.product = false;
    int first = gRsp.tex_tile + gFit.tex_tile - gFit.bake_base; /* TEXEL0's tile (the fit's is the bake's grid) */
    const GuTexture* colour = gfx_bind_texture(first + colour_tile, &plain);
    if (colour == NULL) {
        return false;
    }
    if (!mask) {
        gBatchTex = colour; /* the vertices get the colour tile's coordinates */
        return true;
    }
    TexTransform c = gTexTransform;
    gBatchTex = gfx_bind_texture(first + mask_tile, &plain);
    if (gBatchTex == NULL || gTexTransform.scale_u == 0.0f || gTexTransform.scale_v == 0.0f) {
        return false;
    }
    /* u = s * scale - off for each tile, so u_colour = u * ku + cu */
    sCutTex = colour;
    sCutScaleU = c.scale_u / gTexTransform.scale_u;
    sCutScaleV = c.scale_v / gTexTransform.scale_v;
    sCutOffU = gTexTransform.off_u * sCutScaleU - c.off_u;
    sCutOffV = gTexTransform.off_v * sCutScaleV - c.off_v;
    return true;
}

/*
 * Antialiased texture edges (G_RM_AA_TEX_EDGE and its z-buffered kin): the
 * texel alpha becomes the pixel's coverage, and the VI's filter softens the
 * pixels that are only partly covered -- the soft rim of the pocket screen's
 * window, the edges of cut-out textures. The GE has neither, so these were
 * alpha-tested at one half, and every such edge came out hard.
 *
 * Where the texture's own alpha is all or nothing, partial alpha only comes
 * from the bilinear filter along its edges, which is what the VI would soften:
 * those draws are alpha blended instead, down to 1/8 coverage (the least the
 * RDP draws). Only where they don't write depth, though (2D overlays such as
 * the pocket screen, drawn over a finished scene): 3D cut-outs are often drawn
 * before what lies behind them, so their blended rims would take the clear
 * colour and keep it (the depth they wrote holds the rest out) -- blue fringes
 * round the title screen's flowers. The VI blends such a rim with its
 * neighbours only after the frame is done, which the GE can't; the softening
 * pass at present (soften_frame) stands in for it there.
 * Textures with alpha of their own in between keep the alpha test:
 * the RDP draws such texels solid, and blending them by their alpha shows
 * through what the N64 covers (the pocket screen's item slots). So does a
 * draw whose vertices carry an alpha of their own. hard_edges.txt: never.
 */
RT_SPRAM void gfx_apply_aa_edge(const GuTexture* tex, const CombinerFit* fit) {
    if (!sAaEdge) {
        return;
    }
    if (tex == NULL || !tex->alpha_binary || !fit->tex_alpha || gGu.blend || !gGu.alpha_test ||
        fabsf(fit->abase - 1.0f) > 0.004f || fabsf(fit->as) > 0.004f) {
        sAaEdge = false;
        return;
    }
    gu_blending(true);
    sceGuAlphaFunc(GU_GREATER, 0x1F, 0xFF); /* under 1/8 coverage: not drawn, depth untouched */
    gGu.alpha_ref = 0x1F;
}

RT_SPRAM static void apply_texture_state(void) {
    gBatchTex = NULL;
    sBatchTex2 = NULL;
    sCutTex = NULL;
    gBakeWindow = 0;
    if (gFit.uses_texture && gRsp.tex_on) {
        if (!bind_cut()) {
            sCutTex = NULL;
            gBatchTex = gfx_bind_texture(gRsp.tex_tile + gFit.tex_tile, &gFit);
        }
        if (gBatchTex != NULL && gFit.split_two) {
            sBatchTex2 = gfx_split_second_texture(gRsp.tex_tile + gFit.tex_tile, &gFit);
        }
    }
    gfx_gu_texturing(gBatchTex != NULL);
    if (gBatchTex != NULL) {
        gfx_gu_tex_func(&gFit);
    }
}

/* ---- drawing batches ---------------------------------------------------- */

/* Draws nidx / 3 triangles of the vertices by their indices. */
static inline void draw_indexed(int nidx, const uint16_t* indices, const GuVertex* verts) {
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);
}

/* Points the GE at texture `to` for an extra pass over a batch drawn with
 * `from`. gGu is left alone: every pass switches back to `from` afterwards. */
static void pass_texture(const GuTexture* to, const GuTexture* from) {
    if (to->swizzled != from->swizzled || to->psm != from->psm) {
        sceGuTexMode(to->psm, 0, 0, to->swizzled);
    }
    sceGuTexImage(0, to->gu_width, to->gu_height, to->buf_width ? to->buf_width : to->gu_width, to->pixels);
    sceGuTexFlush();
}

/* The fog pass draws the same triangles again in the fog colour, from the
 * batch's fog vertices and with its indices. */
static void draw_fog_pass(const Batch* b, const uint16_t* indices, int nidx) {
    const GuVertex* fv = b->fogv;
    if (!b->in_arena) {
        GuVertex* mem = sceGuGetMemory(b->nverts * sizeof(GuVertex));
        memcpy(mem, b->fogv, b->nverts * sizeof(GuVertex));
        fv = mem;
    }
    if (gGu.texture) sceGuDisable(GU_TEXTURE_2D);
    if (!gGu.blend) {
        sceGuEnable(GU_BLEND);
        gu_blend_func();
    }
    if (!gGu.depth_test) sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_EQUAL);
    if (gGu.depth_mask) sceGuDepthMask(1);
    if (gGu.alpha_test) sceGuDisable(GU_ALPHA_TEST);
    draw_indexed(nidx, indices, fv);
    if (gGu.texture) sceGuEnable(GU_TEXTURE_2D);
    if (!gGu.blend) sceGuDisable(GU_BLEND);
    if (!gGu.depth_test) sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    if (gGu.depth_mask) sceGuDepthMask(0);
    if (gGu.alpha_test) sceGuEnable(GU_ALPHA_TEST);
}

/* A split combiner's second pass: the same triangles, adding BAKE_Y weighted by its alpha. */
static void draw_split_pass(const GuVertex* verts, const uint16_t* indices, int nidx) {
    const GuTexture* t2 = sBatchTex2;
    const GuTexture* t1 = gBatchTex;
    pass_texture(t2, t1);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    if (!gGu.blend) sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, gGu.blend ? GU_SRC_ALPHA : GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
    if (gGu.depth_mask) sceGuDepthMask(1);
    if (gGu.fog) sceGuDisable(GU_FOG);
    draw_indexed(nidx, indices, verts);
    /* back to the first pass's state, which the batches after this one still use */
    pass_texture(t1, t2);
    sceGuTexFunc(gGu.tex_func & 0xFF, gGu.tex_func >> 8);
    if (gGu.blend) {
        gu_blend_func();
    } else {
        sceGuDisable(GU_BLEND);
    }
    if (gGu.depth_mask) sceGuDepthMask(0);
    if (gGu.fog) sceGuEnable(GU_FOG);
}

/*
 * See bind_cut: the stencil cleared under the triangles, set where the mask
 * tile passes the alpha test, then the colour tile drawn where it is set.
 *
 * For an antialiased edge (sAaEdge) the mask's alpha is coverage, and weights
 * the colour tile against what is behind it. The GE writes its destination
 * alpha only through stencil operations, so the weight goes there in four
 * steps: set to 1/4 .. 4/4 by alpha-tested passes of the mask at rising
 * thresholds. The colour tile is then blended through it (DST_ALPHA), where
 * it is not zero.
 */
static void draw_cut(const GuVertex* verts, const uint16_t* indices, int nidx) {
    static const uint8_t kSoftRef[4] = { 0x1F, 0x5F, 0x9F, 0xDF }; /* mask alpha above these: 1/4 .. 4/4 */
    const GuTexture* t1 = gBatchTex;
    const GuTexture* t2 = sCutTex;
    bool soft = sAaEdge && gGu.alpha_test;

    /* the stencil cleared under the triangles: colour kept, alpha (the stencil) written */
    sceGuPixelMask(0x00FFFFFF);
    if (gGu.depth_mask) sceGuDepthMask(1);
    if (gGu.blend) sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    draw_indexed(nidx, indices, verts);

    /* the mask, alpha-tested, sets it */
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    if (soft) {
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        for (int i = 0; i < 4; i++) {
            sceGuAlphaFunc(GU_GREATER, kSoftRef[i], 0xFF);
            sceGuStencilFunc(GU_ALWAYS, i == 3 ? 0xFF : 0x40 * (i + 1), 0xFF);
            draw_indexed(nidx, indices, verts);
        }
    } else {
        sceGuStencilFunc(GU_ALWAYS, 1, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
        draw_indexed(nidx, indices, verts);
    }

    /* the colour tile where it is set */
    sceGuPixelMask(0);
    if (gGu.depth_mask) sceGuDepthMask(0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuStencilFunc(soft ? GU_NOTEQUAL : GU_EQUAL, soft ? 0 : 1, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    if (soft) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_DST_ALPHA, GU_ONE_MINUS_DST_ALPHA, 0, 0);
    }
    sceGuTexFunc(gGu.tex_func & 0xFF, gGu.tex_func >> 8);
    pass_texture(t2, t1);
    sceGuTexWrap(t2->clamp_s ? GU_CLAMP : GU_REPEAT, t2->clamp_t ? GU_CLAMP : GU_REPEAT);
    sceGuTexScale(sCutScaleU, sCutScaleV);
    sceGuTexOffset(sCutOffU, sCutOffV);
    draw_indexed(nidx, indices, verts);

    /* back to the batch's state */
    sceGuDisable(GU_STENCIL_TEST);
    gGu.stencil = GU_STENCIL_OFF;
    if (soft) gu_blend_func();
    sceGuEnable(GU_ALPHA_TEST);
    if (soft) sceGuAlphaFunc(GU_GREATER, gGu.alpha_ref, 0xFF);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pass_texture(t1, t2);
    sceGuTexWrap(gGu.wrap_u, gGu.wrap_v);
}

/* A batch that writes depth to the screen: where it does, so is the class of
 * the surface written, the two ranges of its indices in turn. */
RT_SPRAM static __attribute__((noinline)) void draw_classes(const Batch* b, const GuVertex* verts, const uint16_t* indices,
                                                   bool more_passes) {
    if (b->nidx > 0) {
        gu_stencil(GU_STENCIL_FLAT);
        draw_indexed(b->nidx, indices, verts);
    }
    if (b->nidx_up > 0) {
        gu_stencil(GU_STENCIL_UPRIGHT);
        draw_indexed(b->nidx_up, indices + b->nidx, verts);
    }
    if (more_passes && b->nidx > 0 && b->nidx_up > 0) {
        gu_stencil(GU_STENCIL_KEEP); /* the passes over both ranges would write one class for all of it */
    }
}

/* Any other batch: a decal on the ground stays off what stands in it. */
static __attribute__((noinline)) void batch_stencil(void) {
    if (!gTarget.bound) {
        gu_stencil(sDecal && sDecalFlat && !sUprightOff ? GU_STENCIL_DECAL : GU_STENCIL_OFF);
    }
}

RT_SPRAM static void draw_batch(Batch* b) {
    const GuVertex* verts = b->verts;
    if (b->in_arena) {
        /* drawn where it is: keep what it used (two at a time, for alignment) */
        sArenaTop += (uint32_t)(b->nverts + 1) & ~1u;
    } else {
        GuVertex* mem = sceGuGetMemory(b->nverts * sizeof(GuVertex));
        memcpy(mem, b->verts, b->nverts * sizeof(GuVertex));
        verts = mem;
    }
    /* the upright triangles' indices follow the others */
    int nidx = b->nidx + b->nidx_up;
    uint16_t* indices = sceGuGetMemory(nidx * sizeof(uint16_t));
    memcpy(indices, b->idx, b->nidx * sizeof(uint16_t));
    memcpy(indices + b->nidx, &b->idx[BATCH_MAX_INDICES - b->nidx_up], b->nidx_up * sizeof(uint16_t));
    if (sCutTex != NULL && gGu.texture) {
        draw_cut(verts, indices, nidx);
        return;
    }
    bool split = sBatchTex2 != NULL && gGu.texture, fog = sFogPass && b->fog_max > 0;
    if (sStencilClass) {
        draw_classes(b, verts, indices, split || fog);
    } else {
        if (gGu.stencil != GU_STENCIL_OFF || sDecal) {
            batch_stencil();
        }
        draw_indexed(nidx, indices, verts);
    }
    if (split) {
        draw_split_pass(verts, indices, nidx);
    }
    if (fog) {
        draw_fog_pass(b, indices, nidx);
    }
}

RT_SPRAM void gfx_flush_batch(void) {
    Batch* normal = &sBatches[DEPTH_NORMAL];
    Batch* near = &sBatches[DEPTH_NEAR];
    Batch* far = &sBatches[DEPTH_FAR];
    int nnormal = normal->nidx + normal->nidx_up;
    int total = nnormal + near->nidx + far->nidx;
    if (total == 0) {
        return;
    }
    if (gTracing) {
        const Batch* b = nnormal ? normal : near->nidx ? near : far;
        const GuVertex* d = &b->verts[0];
        rt_log("  draw#%u %3d+%d+%d zw %.4f/%.1f v (%.1f %.1f %.1f) uv (%.3f %.3f) col %08X | fit %d ta %d w %d tex %p %08X f%u s%u %ux%u var %X | cc %06X %08X prim %08X env %08X | gm %06X oml %08X omh %06X | z %d/%d bl %d at %d fog %d/%d",
               gStats.draw_calls, nnormal, near->nidx, far->nidx, sTraceZ, sTraceW, d->x, d->y, d->z, d->u, d->v,
               d->color, gFit.mode, gFit.tex_alpha, gFit.white_rgb, gBatchTex,
               (unsigned)(gBoundKey.addr_bits >> 3), gBoundKey.fmt, gBoundKey.siz, gBoundKey.tile_w, gBoundKey.tile_h,
               (unsigned)gBoundKey.variant,
               sBatchState.cc0, sBatchState.cc1, sBatchState.prim, sBatchState.env, sBatchState.gm, sBatchState.oml,
               sBatchState.omh, gGu.depth_test, gGu.depth_mask, gGu.blend, gGu.alpha_test ? gGu.alpha_ref : -1,
               sFogPass ? 2 : gGu.fog, normal->fog_max);
    }
    if (gfx_draw_enabled()) {
        if (sDecal && sDecalOffset != gGu.decal) {
            sceGuDepthOffset(sDecalOffset);
            gGu.decal = sDecalOffset;
        }
        if (nnormal > 0) {
            draw_batch(normal);
        }
        if (near->nidx > 0 || far->nidx > 0) {
            int variant = gProjVariant;
            if (near->nidx > 0) {
                gfx_upload_projection_depth(variant, DEPTH_NEAR);
                draw_batch(near);
            }
            if (far->nidx > 0) {
                gfx_upload_projection_depth(variant, DEPTH_FAR);
                draw_batch(far);
            }
            gfx_upload_projection(variant);
        }
    }
    gStats.draw_calls++;
    gStats.triangles += total / 3;
    gStats.ge_verts += normal->nverts + near->nverts + far->nverts;
    for (int i = 0; i < 3; i++) {
        sBatches[i].nverts = 0;
        sBatches[i].nidx = 0;
        sBatches[i].nidx_up = 0;
        sBatches[i].fog_max = 0;
    }
    sDecalOffset = DECAL_MIN_OFFSET;
    sDecalFlat = true;
    if (sArenaReady) {
        arena_reserve();
    }
    /* The vertex arrays start over, so the packed vertices are no longer in them. */
    if (++sVertGen == 0) {
        sVertGen = 1;
    }
}

/*
 * gFit follows the combiner and its constants; the 3D draws classify again only
 * when one of them moved since the last time (and the fit depended on nothing
 * else: a combiner that reads both texture tiles is fitted per triangle).
 */
static struct {
    bool valid;
    CombinerState state;
    uint32_t cycle;
} sClassified;

void gfx_fit_replaced(void) {
    sClassified.valid = false;
}

/* Classifies into `fit`, or reuses gFit if the combiner and its constants are the ones it was made for. */
RT_SPRAM void gfx_classify_cached(CombinerFit* fit) {
    CombinerState state = rdp_combiner_state();
    uint32_t cycle = rdp_cycle_type();
    if (sClassified.valid && combiner_state_equal(&sClassified.state, &state) && sClassified.cycle == cycle) {
        if (fit != &gFit) {
            *fit = gFit;
        }
        return;
    }
    gfx_classify_combiner(fit);
    if (fit != &gFit) {
        gFit = *fit;
    }
    sClassified.valid = !gfx_combiner_reads_two_textures();
    sClassified.state = state;
    sClassified.cycle = cycle;
}

static bool sColorIdentity = false; /* the fit leaves the shade colour as it is (so vertex_color has nothing to do) */

RT_SPRAM static void prepare_3d_state(void) {
    if (!gRdp.state_dirty) {
        return;
    }
    gfx_flush_batch();
    gfx_open_frame();
    gfx_classify_cached(&gFit);
    gfx_apply_render_state(true);
    apply_texture_state();
    gfx_apply_aa_edge(gBatchTex, &gFit);
    gRdp.state_dirty = false;
    sColorIdentity = true;
    for (int ch = 0; ch < 3; ch++) {
        sColorIdentity = sColorIdentity && gFit.base[ch] == 0.0f && gFit.s[ch] == 1.0f && gFit.sa[ch] == 0.0f;
    }
    sColorIdentity = sColorIdentity && gFit.abase == 0.0f && gFit.as == 1.0f;
    sBatchGen++;   /* packed vertices were made with the old state */
    if (sBatchGen == 0) {
        sBatchGen = 1;
    }
    sBatchState.cc0 = gRdp.combine0;
    sBatchState.cc1 = gRdp.combine1;
    sBatchState.prim = gRdp.prim;
    sBatchState.env = gRdp.env;
    sBatchState.gm = gRsp.geometry_mode;
    sBatchState.oml = gRdp.other_l;
    sBatchState.omh = gRdp.other_h;
}

void gfx_mark_dirty(void) {
    gRdp.state_dirty = true;
}

/* ---- triangle output ---------------------------------------------------- */

/* A vertex while it is clipped: what the GE vertex is made from. */
typedef struct {
    float cx, cy, cz, cw;
    float wx, wy, wz;
    float u, v;
    float r, g, b, a;
} ClipVertex;

/* Room for a triangle's three indices in the batch: upright ones fill idx from the end (see sClassify). */
static inline uint16_t* batch_tri_indices(Batch* b, bool upright) {
    if (upright) {
        b->nidx_up += 3;
        return &b->idx[BATCH_MAX_INDICES - b->nidx_up];
    }
    b->nidx += 3;
    return &b->idx[b->nidx - 3];
}

static void pack_vertex(const RspVertex* v, int index) {
    GuVertex* o = &sPacked[index];
    o->x = v->wx;
    o->y = v->wy;
    o->z = v->wz;
    if (sSnapStamp[index] == sSnapGen) {
        o->x = sSnapX[index];
        o->y = sSnapY[index];
    } else {
        snap_vertex_pre(v, index, &o->x, &o->y);
        sSnapX[index] = o->x;
        sSnapY[index] = o->y;
        sSnapStamp[index] = sSnapGen;
    }
    if (sColorIdentity) {
        /* the shade as it is: already within 0..1 */
        o->color = (uint32_t)(int)(v->r * 255.0f + 0.5f) | (uint32_t)(int)(v->g * 255.0f + 0.5f) << 8 |
                   (uint32_t)(int)(v->b * 255.0f + 0.5f) << 16 | (uint32_t)(int)(v->a * 255.0f + 0.5f) << 24;
    } else {
        o->color = vertex_color(&gFit, v->r, v->g, v->b, v->a);
    }
    if (gBatchTex != NULL) {
        o->u = v->u * gTexTransform.scale_u - gTexTransform.off_u;
        o->v = v->v * gTexTransform.scale_v - gTexTransform.off_v;
    } else {
        o->u = 0;
        o->v = 0;
    }
    if (sFogPass) {
        sPackedFog[index] = (uint8_t)unit_to_byte(v->a);
    }
    sPackedStamp[index] = sBatchGen;
}

/*
 * Appends a triangle whose vertices need no clipping or depth pinning: the
 * hot path. A vertex is packed once per render state and put in the batch's
 * vertex array once per batch; the triangle itself is three indices.
 */
static void emit_plain_tri(int i0, int i1, int i2, bool upright) {
    const int idx[3] = { i0, i1, i2 };
    Batch* b = BATCH;
    uint16_t* out = batch_tri_indices(b, upright);
    for (int k = 0; k < 3; k++) {
        int i = idx[k];
        if (sPackedBatch[i] != sVertGen) {
            if (sPackedStamp[i] != sBatchGen) {
                pack_vertex(&gRsp.verts[i], i);
            }
            int at = b->nverts++;
            b->verts[at] = sPacked[i];
            if (sFogPass) {
                uint8_t f = sPackedFog[i];
                GuVertex fv = sPacked[i];
                fv.color = sFogColor | ((uint32_t)f << 24);
                b->fogv[at] = fv;
                if (f > b->fog_max) {
                    b->fog_max = f;
                }
            }
            sPackedIndex[i] = (uint16_t)at;
            sPackedBatch[i] = sVertGen;
        }
        out[k] = sPackedIndex[i];
    }
}

/* Appends a clipped vertex. These are made on the spot and shared by nothing,
 * so each one gets its own slot and an index straight to it. */
static inline uint16_t emit_vertex(Batch* b, const ClipVertex* v) {
    if (b->nverts == 0 && sBatches[0].nverts + sBatches[1].nverts + sBatches[2].nverts == 0) {
        sTraceZ = v->cw != 0 ? v->cz / v->cw : 0;
        sTraceW = v->cw;
    }
    int at = b->nverts++;
    GuVertex o;
    o.x = v->wx;
    o.y = v->wy;
    o.z = v->wz;
    if (v->cw > 0.0f) {
        snap_vertex(v->cx / v->cw, v->cy / v->cw, v->cw, &o.x, &o.y);
    }
    o.color = vertex_color(&gFit, v->r, v->g, v->b, v->a);
    if (gBatchTex != NULL) {
        o.u = v->u * gTexTransform.scale_u - gTexTransform.off_u;
        o.v = v->v * gTexTransform.scale_v - gTexTransform.off_v;
    } else {
        o.u = 0;
        o.v = 0;
    }
    b->verts[at] = o;
    if (sFogPass) {
        uint32_t f = unit_to_byte(v->a);
        o.color = sFogColor | f << 24;
        b->fogv[at] = o;
        if (f > b->fog_max) {
            b->fog_max = (uint8_t)f;
        }
    }
    return (uint16_t)at;
}

#define PLANE_NEAR_Z 5
#define PLANE_FAR_Z 6

static inline float plane_dist(const ClipVertex* v, int plane) {
    switch (plane) {
        case 0: return v->cw - W_EPSILON;           /* behind the eye (NoN: no near-plane clip) */
        case 1: return GUARD_BAND * v->cw - v->cx;
        case 2: return GUARD_BAND * v->cw + v->cx;
        case 3: return GUARD_BAND * v->cw - v->cy;
        case 4: return GUARD_BAND * v->cw + v->cy;
        case PLANE_NEAR_Z: return DEPTH_PIN * v->cw + v->cz;
        default: return DEPTH_PIN * v->cw - v->cz;
    }
}

static void lerp_vertex(ClipVertex* out, const ClipVertex* a, const ClipVertex* b, float t) {
    out->cx = a->cx + (b->cx - a->cx) * t;
    out->cy = a->cy + (b->cy - a->cy) * t;
    out->cz = a->cz + (b->cz - a->cz) * t;
    out->cw = a->cw + (b->cw - a->cw) * t;
    out->wx = a->wx + (b->wx - a->wx) * t;
    out->wy = a->wy + (b->wy - a->wy) * t;
    out->wz = a->wz + (b->wz - a->wz) * t;
    out->u = a->u + (b->u - a->u) * t;
    out->v = a->v + (b->v - a->v) * t;
    out->r = a->r + (b->r - a->r) * t;
    out->g = a->g + (b->g - a->g) * t;
    out->b = a->b + (b->b - a->b) * t;
    out->a = a->a + (b->a - a->a) * t;
}

#define MAX_POLY 16

static void to_clip_vertex(ClipVertex* o, const RspVertex* v) {
    o->cx = v->cx; o->cy = v->cy; o->cz = v->cz; o->cw = v->cw;
    o->wx = v->wx; o->wy = v->wy; o->wz = v->wz;
    o->u = v->u; o->v = v->v;
    o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a;
}

/* Split a polygon by a plane: the part with distance >= 0 goes to `keep`,
 * the rest to `rest` (either may be NULL to drop that part). */
static void split_poly(const ClipVertex* in, int n, int plane, ClipVertex* keep, int* nk, ClipVertex* rest, int* nr) {
    int k = 0, r = 0;
    for (int i = 0; i < n; i++) {
        const ClipVertex* a = &in[i];
        const ClipVertex* b = &in[(i + 1) % n];
        float da = plane_dist(a, plane);
        float db = plane_dist(b, plane);
        if (da >= 0) {
            if (keep != NULL && k < MAX_POLY) keep[k++] = *a;
        } else {
            if (rest != NULL && r < MAX_POLY) rest[r++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            ClipVertex x;
            lerp_vertex(&x, a, b, da / (da - db));
            if (keep != NULL && k < MAX_POLY) keep[k++] = x;
            if (rest != NULL && r < MAX_POLY) rest[r++] = x;
        }
    }
    *nk = k;
    if (nr != NULL) {
        *nr = r;
    }
}

static void emit_poly(const ClipVertex* poly, int n, int depth, bool upright) {
    if (gTracing) {
        char line[512];
        int len = 0;
        for (int k = 0; k < n && len < 400; k++) {
            float w = poly[k].cw;
            len += snprintf(line + len, sizeof(line) - len, " (%.0f,%.0f z%.2f)", (poly[k].cx / w + 1) * 160,
                            (1 - poly[k].cy / w) * 120, poly[k].cz / w);
        }
        rt_log("      piece d%d%s", depth, line);
    }
    Batch* b = &sBatches[depth];
    for (int i = 1; i + 1 < n; i++) {
        uint16_t* out = batch_tri_indices(b, upright);
        out[0] = emit_vertex(b, &poly[0]);
        out[1] = emit_vertex(b, &poly[i]);
        out[2] = emit_vertex(b, &poly[i + 1]);
    }
}

/* The trace line of a triangle about to be drawn. */
static __attribute__((noinline)) void trace_tri(const RspVertex* v0, const RspVertex* v1, const RspVertex* v2) {
    const RspVertex* vs[3] = { v0, v1, v2 };
    char line[256];
    int len = 0;
    for (int k = 0; k < 3; k++) {
        float w = vs[k]->cw;
        len += snprintf(line + len, sizeof(line) - len, " (%.3f,%.3f z%.6f w%.2f uv %.0f,%.0f a%.2f)",
                        w != 0 ? (vs[k]->cx / w + 1) * 160 : 0, w != 0 ? (1 - vs[k]->cy / w) * 120 : 0,
                        w != 0 ? vs[k]->cz / w : 0, w, vs[k]->u / 32, vs[k]->v / 32, vs[k]->a);
    }
    rt_log("    tri%s%s", line, sClassify && tri_upright(v0, v1, v2) ? " upright" : "");
}

/* A whole triangle drawn through emit_poly, which logs its pieces. */
static __attribute__((noinline)) void emit_traced_tri(const RspVertex* v0, const RspVertex* v1, const RspVertex* v2,
                                                      bool upright) {
    ClipVertex tri[3];
    to_clip_vertex(&tri[0], v0);
    to_clip_vertex(&tri[1], v1);
    to_clip_vertex(&tri[2], v2);
    emit_poly(tri, 3, DEPTH_NORMAL, upright);
}

/* A triangle that has to be cut (or split at the depth planes) first: the rare case. */
static __attribute__((noinline)) void draw_triangle_clipped(const RspVertex* v0, const RspVertex* v1, const RspVertex* v2,
                                                            uint16_t flags, bool split_depth, bool upright) {
    /* Clipping can turn one triangle into a fan in each of the three batches. */
    for (int i = 0; i < 3; i++) {
        if (sBatches[i].nverts + 3 * MAX_POLY > BATCH_MAX_VERTS ||
            sBatches[i].nidx + sBatches[i].nidx_up + 3 * MAX_POLY > BATCH_MAX_INDICES) {
            gfx_flush_batch();
            break;
        }
    }

    ClipVertex poly_a[MAX_POLY], poly_b[MAX_POLY];
    to_clip_vertex(&poly_a[0], v0);
    to_clip_vertex(&poly_a[1], v1);
    to_clip_vertex(&poly_a[2], v2);
    int n = 3;
    ClipVertex* in = poly_a;
    ClipVertex* out = poly_b;
    if (flags & (CLIP_NEAR | CLIP_GUARD)) {
        for (int plane = 0; plane < 5 && n >= 3; plane++) {
            split_poly(in, n, plane, out, &n, NULL, NULL);
            ClipVertex* tmp = in;
            in = out;
            out = tmp;
        }
        if (n < 3) {
            gStats.tri_clipped++;
            return;
        }
    }
    if (!split_depth) {
        emit_poly(in, n, DEPTH_NORMAL, upright);
        return;
    }
    ClipVertex outside[MAX_POLY];
    int n_out;
    split_poly(in, n, PLANE_NEAR_Z, out, &n, outside, &n_out);
    if (n_out >= 3) {
        emit_poly(outside, n_out, DEPTH_NEAR, false);
    }
    split_poly(out, n, PLANE_FAR_Z, in, &n, outside, &n_out);
    if (n_out >= 3) {
        emit_poly(outside, n_out, DEPTH_FAR, false);
    }
    if (n >= 3) {
        emit_poly(in, n, DEPTH_NORMAL, upright);
    }
}

void gfx_draw_triangle(int i0, int i1, int i2) {
    const RspVertex* v0 = &gRsp.verts[i0 & 0x3F];
    const RspVertex* v1 = &gRsp.verts[i1 & 0x3F];
    const RspVertex* v2 = &gRsp.verts[i2 & 0x3F];

    gStats.tri_in++;
    if (!gfx_select_target() || gfx_blender_keeps_memory()) {
        return;
    }
    /* Trivial reject: all vertices outside the same frustum side. */
    if (v0->clip & v1->clip & v2->clip & CLIP_OUTSIDE) {
        gStats.tri_trivial++;
        return;
    }

    /* Back/front face culling in NDC when all vertices are in front of the camera. */
    uint32_t cull = gRsp.geometry_mode & (G_CULL_FRONT | G_CULL_BACK);
    if (cull && v0->cw > 0 && v1->cw > 0 && v2->cw > 0) {
        float area = (v1->nx - v0->nx) * (v2->ny - v0->ny) - (v2->nx - v0->nx) * (v1->ny - v0->ny);
        if ((cull & G_CULL_BACK) && area < 0) { gStats.tri_culled++; return; }
        if ((cull & G_CULL_FRONT) && area > 0) { gStats.tri_culled++; return; }
    }

    /* Only the corners of big triangles are welded (see the weld table). */
    {
        float ex = fabsf(v1->nx - v0->nx) + fabsf(v2->nx - v0->nx) + fabsf(v2->nx - v1->nx);
        float ey = fabsf(v1->ny - v0->ny) + fabsf(v2->ny - v0->ny) + fabsf(v2->ny - v1->ny);
        sWeldTri = v0->cw <= 0 || v1->cw <= 0 || v2->cw <= 0 || ex + ey > WELD_MIN_EXTENT;
    }
    gHintTri[0] = v0;
    gHintTri[1] = v1;
    gHintTri[2] = v2;
    if (!gRdp.state_dirty && gFit.pin_sig >= 0 && gfx_pin_signature() != gFit.pin_sig) {
        gRdp.state_dirty = true;
    }
    if (gBakeWindow != 0 && !gRdp.state_dirty && gfx_bake_window_left()) {
        gRdp.state_dirty = true;
    }
    prepare_3d_state();
    if (gTracing) {
        trace_tri(v0, v1, v2);
    }
    bool upright = false;
    if (sClassify) {
        upright = tri_upright(v0, v1, v2);
    } else if (sDecal) {
        int tolerance = decal_tolerance(v0, v1, v2);
        if (tolerance > sDecalOffset) {
            sDecalOffset = tolerance;
        }
        if (sDecalFlat && tri_upright(v0, v1, v2)) {
            sDecalFlat = false;
        }
    }
    uint16_t flags = v0->clip | v1->clip | v2->clip;
    /* The PSP drops a whole triangle if any vertex lies outside the depth
     * range, while the RDP clamps depth per pixel. Pieces in front of the
     * near plane or beyond the far plane are drawn with their depth pinned
     * to that plane instead (not needed when depth is ignored). */
    bool split_depth = (flags & (CLIP_Z_NEAR | CLIP_Z_FAR)) && gProjVariant != PROJ_FLAT_Z;
    if (!(flags & (CLIP_NEAR | CLIP_GUARD)) && !split_depth) {
        /* Whole triangle, three corners: the only batch that can fill up. */
        Batch* b = BATCH;
        if (b->nverts + 3 > BATCH_MAX_VERTS || b->nidx + b->nidx_up + 3 > BATCH_MAX_INDICES) {
            gfx_flush_batch();
        }
        if (gTracing) {
            emit_traced_tri(v0, v1, v2, upright);
        } else {
            emit_plain_tri(i0 & 0x3F, i1 & 0x3F, i2 & 0x3F, upright);
        }
        return;
    }

    draw_triangle_clipped(v0, v1, v2, flags, split_depth, upright);
}

/*
 * Code that runs from the scratchpad (spram.c): the state path -- everything a change of draw state
 * runs through, which is cold by the next time it is needed -- and the snapping the vertices use.
 */
RT_SPRAM_ENTRY(snap_vertex);
RT_SPRAM_ENTRY(weld_search);
RT_SPRAM_ENTRY(tri_upright);
RT_SPRAM_ENTRY(gfx_snap_precompute);
RT_SPRAM_ENTRY(gfx_forget_packed);
RT_SPRAM_ENTRY(gfx_apply_render_state);
RT_SPRAM_ENTRY(gfx_aa_edge_mode);
RT_SPRAM_ENTRY(apply_texture_state);
RT_SPRAM_ENTRY(gfx_classify_cached);
RT_SPRAM_ENTRY(prepare_3d_state);
RT_SPRAM_ENTRY(gfx_flush_batch);
RT_SPRAM_ENTRY(draw_batch);
RT_SPRAM_ENTRY(draw_classes);
RT_SPRAM_ENTRY(gu_stencil);
RT_SPRAM_ENTRY(gfx_gu_texture_image);
RT_SPRAM_ENTRY(gfx_gu_tex_func);
RT_SPRAM_ENTRY(gfx_apply_aa_edge);
