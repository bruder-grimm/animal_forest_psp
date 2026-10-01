/*
 * gfx_draw.c -- GE render state and triangles.
 *
 * A draw state (combiner, render mode, textures) becomes GE state when the
 * first triangle is drawn with it (prepare_3d_state): the combiner's fit
 * (gfx_combiner.c) picks the texture function and the texture to bind, the
 * render mode the depth test, blending, alpha test and fog. Triangles are
 * collected in batches until the state changes, then drawn with one
 * sceGuDrawArray.
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
#include "prof.h"

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
static float sTexScaleU = 1.0f, sTexScaleV = 1.0f;
static float sTexOffU = 0.0f, sTexOffV = 0.0f;
static float sTraceZ, sTraceW; /* first vertex of the batch, for traces */
static TexKey sTraceKey;        /* last bound texture, for traces */

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
void gfx_gu_texture_image(const GuTexture* tex) {
    if (gGu.tex_pixels != tex->pixels) {
        if (tex->swizzled != gGu.tex_swizzled || tex->psm != gGu.tex_psm) {
            sceGuTexMode(tex->psm, 0, 0, tex->swizzled);
            gGu.tex_swizzled = tex->swizzled;
            gGu.tex_psm = tex->psm;
        }
        sceGuTexImage(0, tex->gu_width, tex->gu_height, tex->gu_width, tex->pixels);
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
void gfx_gu_tex_func(const CombinerFit* fit) {
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

void gfx_forget_packed(int first, int count) {
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
 * So vertices are snapped to the N64's quarter-pixel grid, and vertices that
 * nearly coincide are welded: rounded on their own, two vertices 0.08 pixel
 * apart land on different grid points about a third of the time, and the
 * snap then *widens* the sliver to a quarter pixel (the ground blocks at
 * y = 25.85 and 25.93 went to 25.75 and 26.0 -- a line of blue across the
 * grass on the PSP-1000). Every vertex sent in a task is remembered with its
 * grid point, and one within WELD_RADIUS of an earlier one takes that one's
 * grid point instead of its own, if it is at the same depth: two meshes
 * meeting at a point agree on its depth, something in front of them does not.
 * The radius is a quarter of a pixel: along the beach, blocks meet 0.15-0.18
 * pixel apart (x = 307.03 and 306.86 became 307.0 and 306.75 -- a dotted
 * line down the grass and sand). A weld moves a vertex at most three eighths
 * of a pixel; the one edge in the reference frames it moves a row (the
 * station platform, whose corner meets the wall 0.09 pixel away) is such a
 * join.
 *
 * The GE projects the eye-space position itself (GU_TRANSFORM_3D), so the snap
 * is applied as a nudge in eye space: the 2x2 Jacobian of the screen position
 * with respect to eye x and y is inverted for each vertex. The nudge is at most
 * a quarter of a pixel, so the linearisation is exact to well under that. It
 * has to be general: in the village AF folds the camera into the projection,
 * so eye y feeds clip w too (P[1][3] != 0), and a nudge that assumed a plain
 * perspective left the village unsnapped -- a line of blue across the ground.
 */
static struct {
    bool ok;
    float hw, tx, hh, ty;   /* N64 viewport: screen = ndc * hw + tx, ty - ndc * hh */
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
#define WELD_OFS_BITS 6        /* positions in 1/64 quarter pixel */
#define WELD_RADIUS 64         /* in 1/64 quarter pixel: a quarter of a pixel */
#define WELD_EMPTY INT16_MIN
#define WELD_MIN_EXTENT 0.6f  /* sum of a triangle's x and y edge extents, in NDC (~50 px) */
typedef struct {
    int16_t gx, gy;            /* grid point, in quarter pixels (WELD_EMPTY: free) */
    int8_t ox, oy;             /* the vertex, unsnapped: its offset from the grid point */
    uint16_t w;                /* clip w in 1/4 */
} WeldSlot;
static WeldSlot sWeld[WELD_SLOTS] __attribute__((aligned(64)));
static uint32_t sWeldUsed[128 * 128 / 32] __attribute__((aligned(64)));
static int sWeldFill = -1;     /* -1: the table needs clearing */
static bool sWeldOff;
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
static __attribute__((noinline)) int weld_search(int gx, int gy, int fx, int fy, int wq, int dw, int* bx, int* by) {
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
 * The grid point for a vertex at (qx, qy) quarter pixels (|q| < 32000) and
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
    /* the offset from the grid point, in 1/64 quarter pixel: -32 .. 32 */
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
            rt_log("      weld (%.3f,%.3f w%.1f) to (%.2f,%.2f)", qx * 0.25f, qy * 0.25f, w, bx * 0.25f, by * 0.25f);
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
    float hw = gRsp.vscale[0] / 4.0f, hh = gRsp.vscale[1] / 4.0f;
    float tx = gRsp.vtrans[0] / 4.0f, ty = gRsp.vtrans[1] / 4.0f;
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
static __attribute__((noinline)) void snap_vertex(float ndc_x, float ndc_y, float cw, float* ex, float* ey) {
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


/* ---- textures ----------------------------------------------------------- */

float gfx_tile_shift(uint8_t shift) {
    if (shift > 10) return (float)(1 << (16 - shift));
    if (shift > 0) return 1.0f / (float)(1 << shift);
    return 1.0f;
}

/* Rows of a tile start `line` units apart in texture memory; resolve where
 * that puts them in RDRAM (the loaders may have copied a sub-image). */
static bool tile_source(const TileDesc* tile, uint32_t* addr_bits, uint32_t* row_bits, uint8_t* row_swap) {
    uint32_t tmem = tile->tmem & 0x1FF;
    uint32_t bits = kTexelBits[tile->siz & 3];
    uint32_t tile_w = ((tile->lrs - tile->uls) >> 2) + 1;
    uint32_t line = tile->line;
    if (line == 0) {
        line = (tile_w * bits + 63) / 64;
    }
    uint32_t start = gfx_tmem_bits(tmem);
    if (start == TMEM_INVALID) {
        return false;
    }
    uint32_t rows = line * 64;
    if (tmem + line < 512) {
        uint32_t next = gfx_tmem_bits(tmem + line);
        if (next != TMEM_INVALID && next > start && next - start <= 1024u * 32u) {
            rows = next - start;
        }
    }
    *addr_bits = start;
    *row_bits = rows;
    /* Rows read swapped when the RDP's odd-row swap and the way the row was
     * stored disagree; take the pattern from the first two rows. */
    uint8_t even = gfx_tmem_swapped(tmem);
    uint8_t odd = (uint8_t)(1 ^ (tmem + line < 512 ? gfx_tmem_swapped(tmem + line) : 0));
    *row_swap = (uint8_t)(even | (odd << 1));
    return true;
}

bool gfx_make_tile_key(const TileDesc* tile, bool white_rgb, TexKey* key) {
    memset(key, 0, sizeof(*key));
    if (!tile_source(tile, &key->addr_bits, &key->row_bits, &key->row_swap)) {
        RT_LOG_ONCE("gfx: render tile uses unloaded texture memory %03X", tile->tmem);
        return false;
    }
    key->fmt = tile->fmt;
    key->siz = tile->siz;
    key->tile_w = (uint16_t)(((tile->lrs - tile->uls) >> 2) + 1);
    key->tile_h = (uint16_t)(((tile->lrt - tile->ult) >> 2) + 1);
    key->cms = tile->cms;
    key->cmt = tile->cmt;
    key->masks = tile->masks;
    key->maskt = tile->maskt;
    key->variant = white_rgb ? TEXVAR_WHITE_RGB : 0;
    if (tile->fmt == G_IM_FMT_CI) {
        uint32_t tlut_type = (gRdp.other_h >> G_MDSFT_TEXTLUT) & 3;
        key->tlut_type = tlut_type == 3 ? TLUT_IA16 : TLUT_RGBA16;
        uint32_t slot = tile->siz == G_IM_SIZ_4b ? (uint32_t)tile->palette * 16 : 0;
        key->tlut_addr = gRdp.tlut[slot & 0xFF] & 0x1FFFFFFF;
    }
    return true;
}

static const GuTexture* bind_texture_impl(int tile_index, const CombinerFit* fit);
static bool aa_edge_mode(void);
static bool sSplitSecond = false; /* bind_texture_impl: only look up the split's BAKE_Y texture */
static const GuTexture* sBatchTex2 = NULL; /* that texture, for the batch's second pass */

const GuTexture* gfx_bind_texture(int tile_index, const CombinerFit* fit) {
    PROF_BEGIN(PROF_GFX_TEX);
    const GuTexture* tex = bind_texture_impl(tile_index, fit);
    PROF_END(PROF_GFX_TEX);
    return tex;
}

static void fill_src2(TexKey* key, const TexKey* k2) {
    key->src2.addr_bits = k2->addr_bits;
    key->src2.row_bits = k2->row_bits;
    key->src2.tlut_addr = k2->tlut_addr;
    key->src2.tile_w = k2->tile_w;
    key->src2.tile_h = k2->tile_h;
    key->src2.fmt = k2->fmt;
    key->src2.siz = k2->siz;
    key->src2.tlut_type = k2->tlut_type;
    key->src2.cms = k2->cms;
    key->src2.cmt = k2->cmt;
    key->src2.masks = k2->masks;
    key->src2.maskt = k2->maskt;
    key->src2.row_swap = k2->row_swap;
}

static const GuTexture* bind_texture_impl(int tile_index, const CombinerFit* fit) {
    TileDesc* tile = &gRdp.tiles[tile_index & 7];
    TexKey key;
    if (!gfx_make_tile_key(tile, fit->white_rgb, &key)) {
        return NULL;
    }
    if (!fit->product && !fit->combine2 && aa_edge_mode()) {
        key.variant |= TEXVAR_BLEED;
    }
    if (fit->lerp && !fit->combine2) {
        key.variant |= TEXVAR_LERP;
        key.lerp_lo = fit->lerp_lo;
        key.lerp_hi = fit->lerp_hi;
    }
    if (fit->product) {
        TexKey k2;
        if (gfx_make_tile_key(&gRdp.tiles[(tile_index + 1) & 7], false, &k2)) {
            key.variant |= TEXVAR_PRODUCT;
            fill_src2(&key, &k2);
            key.src2.tile_w = 0; /* the product path shares this tile's size */
            key.src2.tile_h = 0;
        }
    } else if (fit->combine2) {
        /* tile_index is the grid tile (fit->bake_base); the other one of the
         * pair is baked into it at the offset the tiles' origins imply. */
        int other = (fit->bake_base == 0) ? tile_index + 1 : tile_index - 1;
        TexKey k2;
        if (gfx_make_tile_key(&gRdp.tiles[other & 7], false, &k2)) {
            key.variant |= TEXVAR_COMBINE2;
            fill_src2(&key, &k2);
            key.ratio_x = fit->bake_ratio_x;
            key.ratio_y = fit->bake_ratio_y;
            key.off_x = fit->bake_off_x;
            key.off_y = fit->bake_off_y;
            key.base_second = (uint8_t)(fit->bake_base == 1);
            key.comb0 = gRdp.combine0;
            key.comb1 = gRdp.combine1;
            key.comb_prim = gRdp.prim;
            key.comb_env = gRdp.env;
            key.comb_lod = gRdp.prim_lod_frac;
            key.bake_kind = fit->split ? (uint8_t)fit->split_kind : BAKE_COLOUR;
            gBakeRgbTile = fit->bake_rgb_tile;
        }
    }
    if (sSplitSecond) {
        /* the second pass of a split combiner: same bake, the other half; not bound here */
        key.bake_kind = BAKE_Y;
        return gfx_tex_get(&key);
    }
    const GuTexture* tex = gfx_tex_get(&key);
    if (tex == NULL) {
        return NULL;
    }
    sTraceKey = key;

    /* Texture coordinate transform, as the RDP does it: shift the S/T
     * coordinate (1/32 texel units) first, then subtract the tile origin
     * (uls/ult, 1/4 texel), then normalise:
     *   u = (s * shift - uls * 8) / (32 * width) = s * scale - offset */
    float shift_u = gfx_tile_shift(tile->shifts);
    float shift_v = gfx_tile_shift(tile->shiftt);
    /* a bake on a finer grid has sub GU texels per tile texel */
    float norm_u = (float)(tex->sub_x ? tex->sub_x : 1) / (32.0f * tex->gu_width);
    float norm_v = (float)(tex->sub_y ? tex->sub_y : 1) / (32.0f * tex->gu_height);
    sTexScaleU = shift_u * norm_u;
    sTexScaleV = shift_v * norm_v;
    sTexOffU = (float)tile->uls * 8.0f * norm_u;
    sTexOffV = (float)tile->ult * 8.0f * norm_v;

    gfx_gu_texture_image(tex);
    int wrap_u = tex->clamp_s ? GU_CLAMP : GU_REPEAT;
    int wrap_v = tex->clamp_t ? GU_CLAMP : GU_REPEAT;
    if (wrap_u != gGu.wrap_u || wrap_v != gGu.wrap_v) {
        sceGuTexWrap(wrap_u, wrap_v);
        gGu.wrap_u = wrap_u;
        gGu.wrap_v = wrap_v;
    }
    int filter = ((gRdp.other_h >> G_MDSFT_TEXTFILT) & 3) == G_TF_POINT ? GU_NEAREST : GU_LINEAR;
    if (filter != gGu.tex_filter) {
        sceGuTexFilter(filter, filter);
        gGu.tex_filter = filter;
    }
    return tex;
}

/* ---- render state ------------------------------------------------------- */

/*
 * The blender's last cycle is P * 0 + MEM * 1: the pixel keeps the colour it
 * has. AF uses such passes to rewrite only the coverage bits (the pocket
 * screen's portrait: I8 rectangles, alpha from coverage), which the port
 * doesn't keep; drawn as ordinary rectangles they covered the portrait black.
 */
static bool blender_keeps_memory(void) {
    uint32_t l = gRdp.other_l;
    uint32_t cycle = (gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3;
    if (!(l & FORCE_BL) || cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
        return false;
    }
    bool two_cycle = cycle == G_CYC_2CYCLE;
    int a = two_cycle ? (l >> 24) & 3 : (l >> 26) & 3;
    int m = two_cycle ? (l >> 20) & 3 : (l >> 22) & 3;
    int b = two_cycle ? (l >> 16) & 3 : (l >> 18) & 3;
    return a == 3 && m == 1 && b == 2;
}

void gfx_other_mode_changed(void) {
    gRdp.keeps_memory = blender_keeps_memory();
    gRdp.state_dirty = true;
}

static bool sAaEdge = false; /* an antialiased texture edge, blended by its alpha (see gfx_apply_aa_edge) */

/* Is the render mode an antialiased texture edge (see gfx_apply_aa_edge)? The
 * texture is bound with TEXVAR_BLEED for these, in case it gets blended.
 * hard_edges.txt: never. */
static bool aa_edge_mode(void) {
    bool hard_edges = RT_SWITCH("hard_edges.txt");
    uint32_t l = gRdp.other_l;
    uint32_t cycle = (gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3;
    bool two_cycle = cycle == G_CYC_2CYCLE;
    int m = two_cycle ? (l >> 20) & 3 : (l >> 22) & 3;
    int b = two_cycle ? (l >> 16) & 3 : (l >> 18) & 3;
    bool xlu = ((l & FORCE_BL) || (l & ZMODE_DEC) == ZMODE_XLU) && m == 1 && b == 0;
    uint32_t aa_bits = AA_EN | IM_RD | CVG_X_ALPHA | ALPHA_CVG_SEL;
    bool z_writes = (l & Z_UPD) && (gRsp.geometry_mode & G_ZBUFFER);
    return !hard_edges && !xlu && !z_writes && (l & aa_bits) == aa_bits && !(l & FORCE_BL) && (l & 3) == 0 &&
           cycle != G_CYC_FILL && cycle != G_CYC_COPY;
}

/* The blend function while gGu.blend is on. */
static void gu_blend_func(void) {
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void gfx_apply_render_state(bool depth_allowed) {
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
    if (decal != gGu.decal) {
        sceGuDepthOffset(decal ? 32 : 0);
        gGu.decal = decal;
    }
    int fog = 0;
    bool two_cycle = ((gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3) == G_CYC_2CYCLE;
    int m = two_cycle ? (l >> 20) & 3 : (l >> 22) & 3;
    int b = two_cycle ? (l >> 16) & 3 : (l >> 18) & 3;
    int blend = ((l & FORCE_BL) || (l & ZMODE_DEC) == ZMODE_XLU) && m == 1 && b == 0;
    /* an antialiased texture edge, maybe (gfx_apply_aa_edge decides once the texture is known) */
    sAaEdge = aa_edge_mode();
    sFogPass = false;
    bool fogged = depth_allowed && (gRsp.geometry_mode & G_FOG) && ((l >> 30) & 3) == 3 && ((l >> 26) & 3) == 2;
    if (fogged && depth_mask && !blend) {
        /* opaque: exact per-vertex fog in a second pass (see sFogPass) */
        sFogPass = true;
        uint32_t fc = gRdp.fog;
        sFogColor = ((fc >> 24) & 0xFF) | (((fc >> 16) & 0xFF) << 8) | (((fc >> 8) & 0xFF) << 16);
    } else if (fogged) {
        /* translucent: the GE's linear fog as an approximation */
        float fnear, ffar;
        if (gfx_compute_fog_range(&fnear, &ffar)) {
            fog = 1;
            uint32_t fc = gRdp.fog;
            uint32_t gu_fc = ((fc >> 24) & 0xFF) | (((fc >> 16) & 0xFF) << 8) | (((fc >> 8) & 0xFF) << 16);
            sceGuFog(fnear, ffar, gu_fc);
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

    if (blend != gGu.blend) {
        if (blend) {
            sceGuEnable(GU_BLEND);
            gu_blend_func();
        } else {
            sceGuDisable(GU_BLEND);
        }
        gGu.blend = blend;
    }

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
 */
static const GuTexture* sCutTex = NULL; /* the colour tile, drawn through the batch texture's alpha */
static float sCutScaleU, sCutScaleV, sCutOffU, sCutOffV; /* its UVs from the batch's */

/* The baked alpha is the base tile's own alpha, whatever the other's. */
static bool alpha_is_tile(int base) {
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

static bool bind_cut(void) {
    if (RT_SWITCH("no_cut.txt") || !gFit.combine2 || gFit.split || gFit.bake_rgb_tile == gFit.bake_base || gTarget.bound ||
        gGu.blend || sFogPass) {
        return false;
    }
    bool mask = gGu.alpha_test;
    if (mask && !alpha_is_tile(gFit.bake_base)) {
        return false;
    }
    CombinerFit plain = gFit;
    plain.combine2 = false;
    plain.product = false;
    int base = gRsp.tex_tile + gFit.tex_tile;
    int other = gFit.bake_base == 0 ? base + 1 : base - 1;
    const GuTexture* colour = gfx_bind_texture(other, &plain);
    if (colour == NULL) {
        return false;
    }
    if (!mask) {
        gBatchTex = colour; /* the vertices get the colour tile's coordinates */
        return true;
    }
    float su = sTexScaleU, sv = sTexScaleV, ou = sTexOffU, ov = sTexOffV;
    gBatchTex = gfx_bind_texture(base, &plain);
    if (gBatchTex == NULL || sTexScaleU == 0.0f || sTexScaleV == 0.0f) {
        return false;
    }
    /* u = s * scale - off for each tile, so u_colour = u * ku + cu */
    sCutTex = colour;
    sCutScaleU = su / sTexScaleU;
    sCutScaleV = sv / sTexScaleV;
    sCutOffU = sTexOffU * sCutScaleU - ou;
    sCutOffV = sTexOffV * sCutScaleV - ov;
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
void gfx_apply_aa_edge(const GuTexture* tex, const CombinerFit* fit) {
    if (!sAaEdge) {
        return;
    }
    if (tex == NULL || !tex->alpha_binary || !fit->tex_alpha || gGu.blend || !gGu.alpha_test ||
        fabsf(fit->abase - 1.0f) > 0.004f || fabsf(fit->as) > 0.004f) {
        sAaEdge = false;
        return;
    }
    sceGuEnable(GU_BLEND);
    gu_blend_func();
    gGu.blend = 1;
    sceGuAlphaFunc(GU_GREATER, 0x1F, 0xFF); /* under 1/8 coverage: not drawn, depth untouched */
    gGu.alpha_ref = 0x1F;
}

static void apply_texture_state(void) {
    gBatchTex = NULL;
    sBatchTex2 = NULL;
    sCutTex = NULL;
    if (gFit.uses_texture && gRsp.tex_on) {
        if (!bind_cut()) {
            sCutTex = NULL;
            gBatchTex = gfx_bind_texture(gRsp.tex_tile + gFit.tex_tile, &gFit);
        }
        if (gBatchTex != NULL && gFit.split_two) {
            sSplitSecond = true;
            sBatchTex2 = bind_texture_impl(gRsp.tex_tile + gFit.tex_tile, &gFit);
            sSplitSecond = false;
        }
    }
    gfx_gu_texturing(gBatchTex != NULL);
    if (gBatchTex != NULL) {
        gfx_gu_tex_func(&gFit);
    }
}

/* ---- drawing batches ---------------------------------------------------- */

/* Points the GE at texture `to` for an extra pass over a batch drawn with
 * `from`. gGu is left alone: every pass switches back to `from` afterwards. */
static void pass_texture(const GuTexture* to, const GuTexture* from) {
    if (to->swizzled != from->swizzled || to->psm != from->psm) {
        sceGuTexMode(to->psm, 0, 0, to->swizzled);
    }
    sceGuTexImage(0, to->gu_width, to->gu_height, to->gu_width, to->pixels);
    sceGuTexFlush();
}

/* The fog pass draws the same triangles again in the fog colour, from the
 * batch's fog vertices and with its indices. */
static void draw_fog_pass(const Batch* b, const uint16_t* indices) {
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
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, b->nidx, indices, fv);
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
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);
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
 * draw_cut for an antialiased edge (sAaEdge): the mask's alpha is coverage,
 * and weights the colour tile against what is behind it. The GE
 * writes its destination alpha only through stencil operations, so the weight
 * goes there in four steps: the stencil cleared under the triangles, then set
 * to 1/4 .. 4/4 by alpha-tested passes of the mask at rising thresholds. The
 * colour tile is then blended through it (DST_ALPHA), where it is not zero.
 */
static void draw_cut_soft(const GuVertex* verts, const uint16_t* indices, int nidx) {
    static const uint8_t kRef[4] = { 0x1F, 0x5F, 0x9F, 0xDF }; /* alpha above these: 1/4 .. 4/4 */
    const GuTexture* t1 = gBatchTex;
    const GuTexture* t2 = sCutTex;
    sceGuPixelMask(0x00FFFFFF); /* colour kept; alpha (the stencil) written */
    if (gGu.depth_mask) sceGuDepthMask(1);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    for (int i = 0; i < 4; i++) {
        sceGuAlphaFunc(GU_GREATER, kRef[i], 0xFF);
        sceGuStencilFunc(GU_ALWAYS, i == 3 ? 0xFF : 0x40 * (i + 1), 0xFF);
        sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);
    }

    sceGuPixelMask(0);
    if (gGu.depth_mask) sceGuDepthMask(0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuStencilFunc(GU_NOTEQUAL, 0, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_DST_ALPHA, GU_ONE_MINUS_DST_ALPHA, 0, 0);
    sceGuTexFunc(gGu.tex_func & 0xFF, gGu.tex_func >> 8);
    pass_texture(t2, t1);
    sceGuTexWrap(t2->clamp_s ? GU_CLAMP : GU_REPEAT, t2->clamp_t ? GU_CLAMP : GU_REPEAT);
    sceGuTexScale(sCutScaleU, sCutScaleV);
    sceGuTexOffset(sCutOffU, sCutOffV);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);

    /* back to the batch's state */
    sceGuDisable(GU_STENCIL_TEST);
    gu_blend_func();
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, gGu.alpha_ref, 0xFF);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pass_texture(t1, t2);
    sceGuTexWrap(gGu.wrap_u, gGu.wrap_v);
}

/* See bind_cut: stencil cleared under the triangles, set where the mask tile
 * passes the alpha test, then the colour tile drawn where it is set. */
static void draw_cut(const GuVertex* verts, const uint16_t* indices, int nidx) {
    const GuTexture* t1 = gBatchTex;
    const GuTexture* t2 = sCutTex;
    if (sAaEdge && gGu.alpha_test) {
        draw_cut_soft(verts, indices, nidx);
        return;
    }
    sceGuPixelMask(0x00FFFFFF); /* colour kept; alpha (the stencil) written */
    if (gGu.depth_mask) sceGuDepthMask(1);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0, 0xFF);
    sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);

    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuStencilFunc(GU_ALWAYS, 1, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);

    sceGuPixelMask(0);
    if (gGu.depth_mask) sceGuDepthMask(0);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuStencilFunc(GU_EQUAL, 1, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    sceGuTexFunc(gGu.tex_func & 0xFF, gGu.tex_func >> 8);
    pass_texture(t2, t1);
    sceGuTexWrap(t2->clamp_s ? GU_CLAMP : GU_REPEAT, t2->clamp_t ? GU_CLAMP : GU_REPEAT);
    sceGuTexScale(sCutScaleU, sCutScaleV);
    sceGuTexOffset(sCutOffU, sCutOffV);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, nidx, indices, verts);

    /* back to the batch's state */
    sceGuDisable(GU_STENCIL_TEST);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    pass_texture(t1, t2);
    sceGuTexWrap(gGu.wrap_u, gGu.wrap_v);
}

static void draw_batch(Batch* b) {
    const GuVertex* verts = b->verts;
    if (b->in_arena) {
        /* drawn where it is: keep what it used (two at a time, for alignment) */
        sArenaTop += (uint32_t)(b->nverts + 1) & ~1u;
    } else {
        GuVertex* mem = sceGuGetMemory(b->nverts * sizeof(GuVertex));
        memcpy(mem, b->verts, b->nverts * sizeof(GuVertex));
        verts = mem;
    }
    uint16_t* indices = sceGuGetMemory(b->nidx * sizeof(uint16_t));
    memcpy(indices, b->idx, b->nidx * sizeof(uint16_t));
    if (sCutTex != NULL && gGu.texture) {
        draw_cut(verts, indices, b->nidx);
        return;
    }
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE | GU_INDEX_16BIT | GU_TRANSFORM_3D, b->nidx, indices, verts);
    if (sBatchTex2 != NULL && gGu.texture) {
        draw_split_pass(verts, indices, b->nidx);
    }
    if (sFogPass && b->fog_max > 0) {
        draw_fog_pass(b, indices);
    }
}

void gfx_flush_batch(void) {
    Batch* normal = &sBatches[DEPTH_NORMAL];
    Batch* near = &sBatches[DEPTH_NEAR];
    Batch* far = &sBatches[DEPTH_FAR];
    int total = normal->nidx + near->nidx + far->nidx;
    if (total == 0) {
        return;
    }
    if (gTracing) {
        const Batch* b = normal->nidx ? normal : near->nidx ? near : far;
        const GuVertex* d = &b->verts[0];
        rt_log("  draw#%u %3d+%d+%d zw %.4f/%.1f v (%.1f %.1f %.1f) uv (%.3f %.3f) col %08X | fit %d ta %d w %d tex %p %08X f%u s%u %ux%u var %X | cc %06X %08X prim %08X env %08X | gm %06X oml %08X omh %06X | z %d/%d bl %d at %d fog %d/%d",
               gStats.draw_calls, normal->nidx, near->nidx, far->nidx, sTraceZ, sTraceW, d->x, d->y, d->z, d->u, d->v,
               d->color, gFit.mode, gFit.tex_alpha, gFit.white_rgb, gBatchTex,
               (unsigned)(sTraceKey.addr_bits >> 3), sTraceKey.fmt, sTraceKey.siz, sTraceKey.tile_w, sTraceKey.tile_h,
               (unsigned)sTraceKey.variant,
               sBatchState.cc0, sBatchState.cc1, sBatchState.prim, sBatchState.env, sBatchState.gm, sBatchState.oml,
               sBatchState.omh, gGu.depth_test, gGu.depth_mask, gGu.blend, gGu.alpha_test ? gGu.alpha_ref : -1,
               sFogPass ? 2 : gGu.fog, normal->fog_max);
    }
    PROF_BEGIN(PROF_GFX_DRAW);
    if (gfx_draw_enabled()) {
        if (normal->nidx > 0) {
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
    PROF_END(PROF_GFX_DRAW);
    gStats.draw_calls++;
    gStats.triangles += total / 3;
    gStats.ge_verts += normal->nverts + near->nverts + far->nverts;
    for (int i = 0; i < 3; i++) {
        sBatches[i].nverts = 0;
        sBatches[i].nidx = 0;
        sBatches[i].fog_max = 0;
    }
    if (sArenaReady) {
        arena_reserve();
    }
    /* The vertex arrays start over, so the packed vertices are no longer in them. */
    if (++sVertGen == 0) {
        sVertGen = 1;
    }
}

static void prepare_3d_state(void) {
    if (!gRdp.state_dirty) {
        return;
    }
    gfx_flush_batch();
    gfx_open_frame();
    gfx_classify_combiner(&gFit);
    gfx_apply_render_state(true);
    apply_texture_state();
    gfx_apply_aa_edge(gBatchTex, &gFit);
    gRdp.state_dirty = false;
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

static void pack_vertex(const RspVertex* v, int index) {
    GuVertex* o = &sPacked[index];
    o->x = v->wx;
    o->y = v->wy;
    o->z = v->wz;
    if (sSnapStamp[index] == sSnapGen) {
        o->x = sSnapX[index];
        o->y = sSnapY[index];
    } else {
        snap_vertex(v->nx, v->ny, v->cw, &o->x, &o->y);
        sSnapX[index] = o->x;
        sSnapY[index] = o->y;
        sSnapStamp[index] = sSnapGen;
    }
    o->color = vertex_color(&gFit, v->r, v->g, v->b, v->a);
    if (gBatchTex != NULL) {
        o->u = v->u * sTexScaleU - sTexOffU;
        o->v = v->v * sTexScaleV - sTexOffV;
    } else {
        o->u = 0;
        o->v = 0;
    }
    int f = (int)(v->a * 255.0f + 0.5f);
    sPackedFog[index] = (uint8_t)(f < 0 ? 0 : f > 255 ? 255 : f);
    sPackedStamp[index] = sBatchGen;
}

/*
 * Appends a triangle whose vertices need no clipping or depth pinning: the
 * hot path. A vertex is packed once per render state and put in the batch's
 * vertex array once per batch; the triangle itself is three indices.
 */
static void emit_plain_tri(int i0, int i1, int i2) {
    const int idx[3] = { i0, i1, i2 };
    Batch* b = BATCH;
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
        b->idx[b->nidx++] = sPackedIndex[i];
    }
}

/* Appends a clipped vertex. These are made on the spot and shared by nothing,
 * so each one gets its own slot and an index straight to it. */
static inline void emit_vertex(Batch* b, const ClipVertex* v) {
    if (sBatches[0].nidx + sBatches[1].nidx + sBatches[2].nidx == 0) {
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
        o.u = v->u * sTexScaleU - sTexOffU;
        o.v = v->v * sTexScaleV - sTexOffV;
    } else {
        o.u = 0;
        o.v = 0;
    }
    b->verts[at] = o;
    if (sFogPass) {
        int f = (int)(v->a * 255.0f + 0.5f);
        f = f < 0 ? 0 : f > 255 ? 255 : f;
        o.color = sFogColor | ((uint32_t)f << 24);
        b->fogv[at] = o;
        if (f > b->fog_max) {
            b->fog_max = (uint8_t)f;
        }
    }
    b->idx[b->nidx++] = (uint16_t)at;
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

static void emit_poly(const ClipVertex* poly, int n, int depth) {
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
        emit_vertex(b, &poly[0]);
        emit_vertex(b, &poly[i]);
        emit_vertex(b, &poly[i + 1]);
    }
}

void gfx_draw_triangle(int i0, int i1, int i2) {
    const RspVertex* v0 = &gRsp.verts[i0 & 0x3F];
    const RspVertex* v1 = &gRsp.verts[i1 & 0x3F];
    const RspVertex* v2 = &gRsp.verts[i2 & 0x3F];

    gStats.tri_in++;
    if (!gfx_select_target() || gfx_ablated(2) || gfx_blender_keeps_memory()) {
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
    prepare_3d_state();
    if (gTracing) {
        const RspVertex* vs[3] = { v0, v1, v2 };
        char line[256];
        int len = 0;
        for (int k = 0; k < 3; k++) {
            float w = vs[k]->cw;
            len += snprintf(line + len, sizeof(line) - len, " (%.3f,%.3f z%.2f w%.0f uv %.0f,%.0f a%.2f)",
                            w != 0 ? (vs[k]->cx / w + 1) * 160 : 0, w != 0 ? (1 - vs[k]->cy / w) * 120 : 0,
                            w != 0 ? vs[k]->cz / w : 0, w, vs[k]->u / 32, vs[k]->v / 32, vs[k]->a);
        }
        rt_log("    tri%s", line);
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
        if (b->nverts + 3 > BATCH_MAX_VERTS || b->nidx + 3 > BATCH_MAX_INDICES) {
            gfx_flush_batch();
        }
        if (gfx_ablated(1)) {
            return;
        }
        if (gTracing) {
            ClipVertex tri[3];
            to_clip_vertex(&tri[0], v0);
            to_clip_vertex(&tri[1], v1);
            to_clip_vertex(&tri[2], v2);
            emit_poly(tri, 3, DEPTH_NORMAL);
        } else {
            emit_plain_tri(i0 & 0x3F, i1 & 0x3F, i2 & 0x3F);
        }
        return;
    }

    /* Clipping can turn one triangle into a fan in each of the three batches. */
    for (int i = 0; i < 3; i++) {
        if (sBatches[i].nverts + 3 * MAX_POLY > BATCH_MAX_VERTS ||
            sBatches[i].nidx + 3 * MAX_POLY > BATCH_MAX_INDICES) {
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
        emit_poly(in, n, DEPTH_NORMAL);
        return;
    }
    ClipVertex outside[MAX_POLY];
    int n_out;
    split_poly(in, n, PLANE_NEAR_Z, out, &n, outside, &n_out);
    if (n_out >= 3) {
        emit_poly(outside, n_out, DEPTH_NEAR);
    }
    split_poly(out, n, PLANE_FAR_Z, in, &n, outside, &n_out);
    if (n_out >= 3) {
        emit_poly(outside, n_out, DEPTH_FAR);
    }
    if (n >= 3) {
        emit_poly(in, n, DEPTH_NORMAL);
    }
}

