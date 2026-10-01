/*
 * gfx_internal.h -- what the renderer's files share.
 *
 * The renderer turns the game's graphics tasks (F3DZEX2 and S2DEX2 display
 * lists for the N64's RSP and RDP) into sceGu command lists for the PSP's GE:
 *
 *   gfx_worker.c   the renderer thread; graphics tasks, yields and presents
 *   gfx_dl.c       the display list interpreter: RSP and RDP state
 *   gfx_vertex.c   matrices and the vertex stage (transform, lighting, clip flags) on the VFPU
 *   gfx_combiner.c the RDP colour combiner fitted to the GE's texture functions
 *   gfx_draw.c     GE render state, texture binding, batching, triangle clipping
 *   gfx_rect.c     rectangles: fills, texture rectangles, S2DEX2 backgrounds
 *   gfx_frame.c    frames: framebuffers, projection, render targets, softening, present
 *   gfx_tex.c      texture decoding and the texture cache
 *   gfx_debug.c    screenshots, RDRAM dumps, replay, traces
 */
#ifndef AFPSP_GFX_INTERNAL_H
#define AFPSP_GFX_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pspgu.h>

#include "rt.h"

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240
#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* ---- the PSP screen ----------------------------------------------------- */

#define BUF_WIDTH 512  /* stride of the GE's colour and depth buffers, in pixels */
#define GU_OFFSET_X (2048 - PSP_SCREEN_W / 2)
#define GU_OFFSET_Y (2048 - PSP_SCREEN_H / 2)
/*
 * Overscan: a TV never shows the outermost pixels of the N64 picture, and the
 * game relies on it -- the black iris when entering a house stops 2 pixels
 * short of every edge, and on a PSP, which shows everything, the scene showed
 * through a 3-pixel frame. So N64 pixels CROP..320-CROP and CROP..240-CROP fill
 * the PSP screen, which crops the picture by 1.3%.
 */
#define CROP_X 2.0f
#define CROP_Y 2.0f
#define SCALE_X ((float)PSP_SCREEN_W / (N64_SCREEN_W - 2 * CROP_X))
#define SCALE_Y ((float)PSP_SCREEN_H / (N64_SCREEN_H - 2 * CROP_Y))

/*
 * Unstretched (START + SELECT, rt_gfx_toggle_stretch): the picture at the
 * shape a 4:3 TV gives it, between black bars. N64 pixels are square there,
 * so the 316 x 236 that are shown come to 364 PSP pixels at full height.
 */
#define PILLAR_W 364
#define PILLAR_X ((PSP_SCREEN_W - PILLAR_W) / 2)

/*
 * N64 pixels -> the GE's target: the screen (scaled, cropped) or a render
 * target (1:1). x0..x1 and 0..y1 are the picture's bounds on the target.
 */
typedef struct {
    float scale_x, scale_y, crop_x, crop_y;
    int x0, x1, y1;
} ScreenMap;
extern ScreenMap gMap;

/* The VRAM address the CPU and sceGuCopyImage see for a GE buffer offset. */
#define VRAM_ADDR(offset) ((void*)(0x04000000 + (uint32_t)(offset)))

/* Debugging switches (gfx_debug.c) that the drawing code checks as it goes. */
extern bool gTracing;  /* log every draw of this task (trace_tasks.txt) */

/* ---- F3DEX2 / RDP modes ------------------------------------------------- */

/* geometry mode */
#define G_ZBUFFER 0x00000001
#define G_SHADE 0x00000004
#define G_CULL_FRONT 0x00000200
#define G_CULL_BACK 0x00000400
#define G_FOG 0x00010000
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000
#define G_SHADING_SMOOTH 0x00200000
#define G_LIGHTING_POSITIONAL 0x00400000

/* othermode */
#define G_MDSFT_CYCLETYPE 20
#define G_CYC_1CYCLE 0
#define G_CYC_2CYCLE 1
#define G_CYC_COPY 2
#define G_CYC_FILL 3
#define G_MDSFT_TEXTLUT 14
#define G_MDSFT_TEXTFILT 12
#define G_TF_POINT 0
#define AA_EN 0x8
#define Z_CMP 0x10
#define Z_UPD 0x20
#define IM_RD 0x40
#define ZMODE_DEC 0xC00
#define ZMODE_XLU 0x800
#define CVG_X_ALPHA 0x1000
#define ALPHA_CVG_SEL 0x2000
#define FORCE_BL 0x4000
#define G_AC_THRESHOLD 1
#define G_AC_DITHER 3
#define G_ZS_PRIM 4

#define G_MAXFBZ_FILL 0xFFFCFFFCu /* fill colour of a depth clear (G_MAXFBZ in both halves) */

#define MAX_VERTICES 64
#define MAX_MATRIX_STACK 32
#define MAX_LIGHTS 8

/* ---- geometry ----------------------------------------------------------- */

/* The VFPU loads and stores matrices a row at a time, as quads. */
typedef float Mat4[4][4] __attribute__((aligned(16)));

typedef struct {
    float cx, cy, cz, cw; /* clip space */
    float nx, ny;         /* cx / cw, cy / cw (for face culling) */
    float wx, wy, wz;     /* after modelview */
    float u, v;           /* texture coords in 1/32 texel units */
    float r, g, b, a;     /* shade, 0..1 */
    uint16_t clip;        /* CLIP_* */
} RspVertex;

/* process_vertices' VFPU code stores straight into these fields. */
_Static_assert(offsetof(RspVertex, cx) == 0 && offsetof(RspVertex, cw) == 12, "clip coords must be a quad at 0");
_Static_assert(offsetof(RspVertex, wx) == 24 && offsetof(RspVertex, wz) == 32, "world position moved");
_Static_assert(sizeof(RspVertex) % 16 == 0, "vertices must stay quad-aligned");

/*
 * Where a vertex lies: outside the guard band or behind the eye (these need
 * clipping), outside the view on a side (a triangle with all three vertices
 * out on the same side is not drawn), or outside the depth range.
 */
#define CLIP_NEAR 0x01    /* behind the eye */
#define CLIP_GUARD 0x1E   /* outside the guard band (one bit per side) */
#define CLIP_X_POS 0x20
#define CLIP_X_NEG 0x40
#define CLIP_Y_POS 0x80
#define CLIP_Y_NEG 0x100
#define CLIP_Z_NEAR 0x200 /* in front of the near plane */
#define CLIP_Z_FAR 0x400  /* beyond the far plane */
#define CLIP_OUTSIDE (CLIP_NEAR | CLIP_X_POS | CLIP_X_NEG | CLIP_Y_POS | CLIP_Y_NEG)

#define GUARD_BAND 6.0f
/* F3DZEX2 "NoN" doesn't clip at the near plane (depth is clamped); only geometry behind the eye is cut. */
#define W_EPSILON 0.001f
/* Depth pieces are split and pinned slightly inside the depth range so that
 * rounding in the GE can't push them out of it. */
#define DEPTH_PIN 0.9999f

/* A vertex as the GE takes it. */
typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} GuVertex;

#define GU_VTYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF)

typedef struct {
    float col[3];
    float dir[3];      /* world direction (normalised) */
    float model_dir[3];
    float pos[3];
    bool point;
    float kc, kl, kq;
} Light;

/* ---- RSP and RDP state (gfx_dl.c) --------------------------------------- */

typedef struct {
    uint32_t uls, ult, lrs, lrt;
    uint8_t fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    uint16_t tmem;
    uint16_t line;      /* row length in texture memory units (8 bytes) */
} TileDesc;

typedef struct {
    uint32_t segments[16];
    /* The VFPU loads matrices and vertices as quads: keep them 16-byte aligned. */
    Mat4 mv_stack[MAX_MATRIX_STACK] __attribute__((aligned(16)));
    int mv_depth;
    Mat4 proj __attribute__((aligned(16)));
    bool mvp_dirty;
    Mat4 mvp __attribute__((aligned(16)));
    bool lights_dirty;
    int num_lights;
    Light lights[MAX_LIGHTS + 1];  /* lights[num_lights] is the ambient light */
    float lookat[2][3];
    float lookat_model[2][3];
    RspVertex verts[MAX_VERTICES] __attribute__((aligned(16)));
    uint32_t geometry_mode;
    int16_t vscale[4], vtrans[4];
    uint16_t tex_scale_s, tex_scale_t;
    bool tex_on;
    uint8_t tex_tile;
    int16_t fog_mul, fog_ofs;
    uint32_t pending_branch;
    bool s2dex;          /* the S2DEX2 microcode is loaded */
} RspState;

/*
 * A texture load (G_LOADBLOCK, G_LOADTILE) as it came. A load fills up to all
 * 512 units of texture memory, and most are replaced by the next load to the
 * same place before more than a unit or two of theirs has been looked at, so
 * they are kept as they are and a unit is worked out when it is asked for.
 */
typedef struct {
    uint16_t first, count;  /* the units it fills */
    uint16_t line;          /* G_LOADTILE: units per row; 0 for a G_LOADBLOCK */
    uint32_t bits;          /* RDRAM bit address of the first unit */
    uint32_t step;          /* G_LOADTILE: bits from a row to the next; G_LOADBLOCK: dxt */
} TmemLoad;
#define TMEM_LOADS 8

typedef struct {
    uint32_t other_h, other_l;
    uint32_t combine0, combine1;
    uint32_t env, prim, blend, fog, fill;
    float prim_lod_frac;
    uint16_t prim_depth;
    uint32_t scissor[4];
    uint32_t cimg, zimg, timg;
    uint32_t cimg_width;
    uint8_t cimg_fmt, cimg_siz;
    uint8_t timg_fmt, timg_siz;
    uint16_t timg_width;
    /* Texture memory: 512 units of 8 bytes. For each unit, the RDRAM bit
     * address its contents were loaded from (TMEM_INVALID if never loaded).
     * Render tiles address their texels relative to a unit, so they can
     * point anywhere inside a loaded block. These tables hold all but the
     * latest loads, which are in tmem_loads: read them with gfx_tmem_bits and
     * gfx_tmem_swapped. */
    uint32_t tmem_bits[512];
    uint8_t tmem_swap[512];   /* unit stored with its halves exchanged */
    TmemLoad tmem_loads[TMEM_LOADS]; /* oldest first */
    int num_tmem_loads;
    TileDesc tiles[8];
    uint32_t tlut[256];
    bool state_dirty;         /* the draw state changed since the last batch */
    bool keeps_memory;        /* the blender leaves every pixel as it is (see gfx_draw.c) */
} RdpState;

#define TMEM_INVALID 0xFFFFFFFFu

extern RspState gRsp;
extern RdpState gRdp;

static inline uint32_t seg_addr(uint32_t addr) {
    return (gRsp.segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x1FFFFFFF;
}

static inline bool rdp_two_cycle(void) {
    return ((gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3) == G_CYC_2CYCLE;
}

/* Runs a graphics task's display list (on the renderer thread). */
void gfx_run_task(uint32_t task);
/* Where a unit of texture memory was loaded from (an RDRAM bit address,
 * TMEM_INVALID if it never was), and whether it is stored with its halves exchanged. */
uint32_t gfx_tmem_bits(uint32_t unit);
bool gfx_tmem_swapped(uint32_t unit);
void gfx_tmem_reset(void);

/* ---- the vertex stage (gfx_vertex.c) ------------------------------------ */

void gfx_vertex_init(void);
void mat_identity(Mat4 m);
void mat_mul(Mat4 out, const Mat4 a, const Mat4 b); /* out = a * b; out may be a or b */
void normalize3(float* v);
/* G_VTX: loads count vertices from RDRAM at addr into gRsp.verts[start...]. */
void gfx_process_vertices(uint32_t addr, int start, int count);

/* ---- the combiner (gfx_combiner.c) -------------------------------------- */

/* How the GE draws the current combiner: its texture function and the vertex colour. */
typedef enum {
    TEX_NONE,
    TEX_MODULATE,
    TEX_BLEND,
    TEX_ADD,
    TEX_DECAL,
} TexMode;

typedef struct {
    TexMode mode;
    int tex_tile;       /* which texture (0 = TEXEL0, 1 = TEXEL1) the GE samples */
    bool tex_alpha;     /* GU_TCC_RGBA */
    bool white_rgb;     /* texture variant with RGB forced to white */
    bool lerp;          /* texture variant with RGB mapped lerp(lerp_lo, lerp_hi, texel) (TEXVAR_LERP) */
    uint32_t lerp_lo, lerp_hi;
    bool uses_texture;
    /* vertex rgb = base + s * shade_rgb + sa * shade_a (per channel) */
    float base[3], s[3], sa[3];
    /* vertex alpha = abase + as * shade_a */
    float abase, as;
    uint32_t env;
    int pin_sig;        /* two-texture combiners: which tiles were sampled only at a clamped edge (-1: n/a) */
    bool product;       /* sample the texel-wise product of both tiles (TEXVAR_PRODUCT) */
    bool combine2;      /* sample both tiles baked through the combiner (TEXVAR_COMBINE2) */
    int8_t bake_rgb_tile;   /* which tile the colour combine reads (0 or 1) */
    int8_t bake_base;       /* which tile's grid the bake is laid out on */
    float bake_ratio_x, bake_off_x, bake_ratio_y, bake_off_y;
    bool approx;            /* fit_from_probes fell back to an approximation */
    bool split;             /* a split two-texture combiner (see try_split) */
    bool split_two;         /* ... drawn in two passes: BAKE_X modulated, then BAKE_Y added */
    int8_t split_kind;      /* the bake the first (or only) pass samples */
} CombinerFit;

/* The triangle about to be drawn, which some fits depend on (NULL for rectangles). */
extern const RspVertex* gHintTri[3];

void gfx_classify_combiner(CombinerFit* fit);
/* Which tiles the hint triangle samples only at a clamped edge (bits 0 and 1). */
int gfx_pin_signature(void);
/* Which of a TEXVAR_COMBINE2 pair the baked colour comes from (0: T0, 1: T1; set when binding one). */
extern int gBakeRgbTile;

/* The colour a vertex gets: the fit's function of its shade. */
static inline uint32_t vertex_color(const CombinerFit* fit, float r, float g, float b, float a) {
    float sh[3] = { r, g, b };
    uint32_t out = 0;
    for (int ch = 0; ch < 3; ch++) {
        float v = fit->base[ch] + fit->s[ch] * sh[ch] + fit->sa[ch] * a;
        int iv = (int)(v * 255.0f + 0.5f);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        out |= (uint32_t)iv << (8 * ch);
    }
    float va = fit->abase + fit->as * a;
    int ia = (int)(va * 255.0f + 0.5f);
    if (ia < 0) ia = 0;
    if (ia > 255) ia = 255;
    return out | ((uint32_t)ia << 24);
}

/* ---- textures (gfx_tex.c) ----------------------------------------------- */

/* Texture formats (fmt << 4 | siz) */
#define G_IM_FMT_RGBA 0
#define G_IM_FMT_YUV 1
#define G_IM_FMT_CI 2
#define G_IM_FMT_IA 3
#define G_IM_FMT_I 4
#define G_IM_SIZ_4b 0
#define G_IM_SIZ_8b 1
#define G_IM_SIZ_16b 2
#define G_IM_SIZ_32b 3

#define G_TX_MIRROR 0x1
#define G_TX_CLAMP 0x2

#define TLUT_NONE 0
#define TLUT_RGBA16 2
#define TLUT_IA16 3

/* Bits per texel, by G_IM_SIZ_* */
static const uint8_t kTexelBits[4] = { 4, 8, 16, 32 };

/* Texture variant flags */
#define TEXVAR_WHITE_RGB 0x1 /* keep alpha, force RGB to white */
#define TEXVAR_PRODUCT 0x2   /* texel-wise product with the second source (src2) */
/*
 * The combiner's own function of the two sources, baked per texel: RGB from
 * whichever tile the colour combine uses, alpha from evaluating the alpha
 * combine on the pair. Unlike TEXVAR_PRODUCT the two tiles need not address
 * their texels identically -- off_x/off_y carry the second tile's offset --
 * which is what lets a scrolling tile be baked against a static one.
 */
#define TEXVAR_COMBINE2 0x4
#define TEXVAR_TWO (TEXVAR_PRODUCT | TEXVAR_COMBINE2)
/* Transparent texels take the average colour of their opaque neighbours, so
 * that filtering fades an alpha-blended edge out without tinting it with
 * whatever colour the transparent texels happen to hold. Single textures only. */
#define TEXVAR_BLEED 0x8
/* Each colour channel mapped through lerp(lerp_lo, lerp_hi, texel): a combiner
 * of the form shade * lerp(ENV, PRIM, TEXEL0) (the beach's sand and wet band)
 * becomes an exact GE modulate of this texture by the shade. */
#define TEXVAR_LERP 0x10

/*
 * What a TEXVAR_COMBINE2 bake holds. BAKE_COLOUR: one tile's colour and the
 * combined alpha. A combiner whose colour is shade * X + Y (X, Y and the alpha
 * Z functions of the two texels alone) is split instead -- the water in the
 * village: BAKE_X holds X and Z, BAKE_Y holds Y and Z (see try_split in gfx_combiner.c).
 */
#define BAKE_COLOUR 0
#define BAKE_X 1
#define BAKE_Y 2

/*
 * A texture as the RDP samples it. Texel (x, y) of the tile lives at bit
 * address addr_bits + y * row_bits + x * texel_bits in RDRAM (the RDP's
 * texture memory is resolved to its RDRAM source by the loader commands).
 * Sampling coordinates are clamped to the tile size and/or wrapped by the
 * masks exactly like the RDP (clamp, then mask with optional mirroring);
 * the cache bakes that into an image the GE can sample with plain
 * GU_CLAMP / GU_REPEAT.
 */
typedef struct {
    uint32_t addr_bits;   /* physical RDRAM address * 8 (+4 for odd 4-bit texels) */
    uint32_t row_bits;    /* bits from one row's start to the next */
    uint32_t tlut_addr;   /* palette address (CI formats) */
    uint16_t tile_w;      /* tile size in texels: the clamp range */
    uint16_t tile_h;
    uint8_t fmt;
    uint8_t siz;
    uint8_t tlut_type;
    uint8_t cms;          /* G_TX_MIRROR | G_TX_CLAMP */
    uint8_t cmt;
    uint8_t masks;        /* wrap mask bits, 0 = none */
    uint8_t maskt;
    uint8_t variant;
    uint8_t row_swap;     /* bit 0: even rows, bit 1: odd rows read with swapped unit halves */
    /* TEXVAR_PRODUCT: a second texture over the same tile, multiplied in
     * (the RDP combining two textures sampled at the same coordinates). */
    struct {
        uint32_t addr_bits, row_bits, tlut_addr;
        uint16_t tile_w, tile_h;
        uint8_t fmt, siz, tlut_type, cms, cmt, masks, maskt, row_swap;
    } src2;
    /* TEXVAR_COMBINE2 only: src2's texel = grid texel * ratio + off, per axis.
     * The ratio is the two tiles' texture-shift scales; it is not always 1,
     * because the tiles may sample the same coordinates at different rates. */
    float ratio_x, off_x, ratio_y, off_y;
    uint8_t base_second;    /* this tile is the combiner's TEXEL1, not TEXEL0 */
    uint8_t bake_kind;      /* BAKE_COLOUR, or which half of a split combiner (BAKE_X, BAKE_Y) */
    uint32_t comb0, comb1;  /* the combine words the bake was made from */
    uint32_t comb_prim, comb_env;
    float comb_lod;         /* primitive LOD fraction, a combiner input too */
    uint32_t lerp_lo, lerp_hi; /* TEXVAR_LERP: RGB at texel 0 and texel 255 */
} TexKey;

typedef struct {
    void* pixels;         /* texels in `psm`, power-of-two dims */
    uint8_t psm;          /* GU_PSM_8888, _5551 or _4444 (see pick_psm) */
    uint16_t gu_width;
    uint16_t gu_height;
    bool clamp_s;         /* GE wrap mode per axis */
    bool clamp_t;
    bool swizzled;        /* stored in the GE's block order (see build) */
    uint8_t sub_x, sub_y; /* GU texels per tile texel (a TEXVAR_COMBINE2 bake laid out on the finer tile) */
    float mean[4];        /* average RGBA (0..1) over the image */
    float dev;            /* largest per-channel standard deviation */
    bool alpha_binary;    /* every texel's alpha is 0 or 255 (partial only where filtered) */
} GuTexture;

void gfx_tex_init(void);
void gfx_tex_new_frame(void);
/* Returns a decoded texture for key (cached; content-hashed for RAM textures). */
const GuTexture* gfx_tex_get(const TexKey* key);
/* Texture builds (and two-texture bakes among them) and their time since the last call. */
void gfx_tex_take_build_stats(uint32_t* builds, uint32_t* bakes, uint32_t* us);
/* Bakes put off by the per-frame budget since the last call (an older bake of the pair was used). */
uint32_t gfx_tex_take_deferred(void);
/* Textures read from RDRAM [addr, addr + len) re-check their contents on next use. */
void gfx_tex_invalidate_range(uint32_t addr, uint32_t len);
/* Frees texture buffers retired since the previous frame's sync. */
void gfx_tex_flush_retired(void);
/* One texel as RGBA8888, whatever format and layout it is stored in. */
uint32_t gfx_tex_texel(const GuTexture* t, uint32_t x, uint32_t y);
/* A split combiner's baked texel (BAKE_X or BAKE_Y) for a pair of source texels, memoised:
 * gfx_bake_split_prepare once per bake (the tag), then gfx_bake_split_lookup per texel. */
#define SPLIT_MEMO 16384
#define SPLIT_STATES 16
typedef struct {
    uint32_t t0, t1, val;
} SplitMemo;
extern SplitMemo gSplitMemo[SPLIT_MEMO];
extern uint8_t gSplitTag[SPLIT_MEMO];
uint8_t gfx_bake_split_prepare(int kind);
uint32_t gfx_bake_split_fill(uint32_t t0, uint32_t t1, uint8_t tag, uint32_t h);
static inline uint32_t gfx_bake_split_lookup(uint32_t t0, uint32_t t1, uint8_t tag) {
    uint32_t h = ((t0 * 2654435761u) ^ (t1 * 2246822519u) ^ ((uint32_t)tag * 3266489917u)) >> 18;
    const SplitMemo* m = &gSplitMemo[h];
    if (__builtin_expect(gSplitTag[h] == tag && m->t0 == t0 && m->t1 == t1, 1)) {
        return m->val;
    }
    return gfx_bake_split_fill(t0, t1, tag, h);
}
/* The baked alpha for a pair of source alphas (memoised per combiner state). */
uint8_t gfx_bake_alpha(uint8_t a0, uint8_t a1);
/* The same for a loop: prepare once per bake, then look up per texel. */
void gfx_bake_alpha_prepare(void);
uint8_t gfx_bake_alpha_fill(uint32_t k);
extern uint8_t gBakeAlpha[256 * 256];
extern uint32_t gBakeAlphaValid[256 * 256 / 32];
static inline uint8_t gfx_bake_alpha_lookup(uint8_t a0, uint8_t a1) {
    uint32_t k = (uint32_t)a0 << 8 | a1;
    if (__builtin_expect(gBakeAlphaValid[k >> 5] & (1u << (k & 31)), 1)) {
        return gBakeAlpha[k];
    }
    return gfx_bake_alpha_fill(k);
}

/* ---- GE state and drawing (gfx_draw.c) ---------------------------------- */

/* What the GE was last told, so unchanged state isn't sent again (-1: unknown). */
typedef struct {
    int depth_test;
    int depth_mask;
    int blend;
    int alpha_test;
    int alpha_ref;
    int texture;
    int tex_func;
    int tex_filter;
    int wrap_u, wrap_v;
    uint32_t env;
    int decal;
    int fog;
    const void* tex_pixels;
    int tex_swizzled;
    int tex_psm;
} GuCache;

extern GuCache gGu;
extern CombinerFit gFit;                /* the fit of the current draw state */
extern const GuTexture* gBatchTex;      /* the texture it samples, NULL for none */

void gfx_gu_reset_cache(void);
/* Tile coordinates -> texture key; false if the tile's texture memory was never loaded. */
bool gfx_make_tile_key(const TileDesc* tile, bool white_rgb, TexKey* key);
float gfx_tile_shift(uint8_t shift);
const GuTexture* gfx_bind_texture(int tile_index, const CombinerFit* fit);
void gfx_gu_texture_image(const GuTexture* tex);
void gfx_gu_texturing(bool on);
void gfx_gu_tex_func(const CombinerFit* fit);
void gfx_apply_render_state(bool depth_allowed);
void gfx_apply_aa_edge(const GuTexture* tex, const CombinerFit* fit);
/* The other modes changed: the draw state is dirty, and gRdp.keeps_memory is worked out again. */
void gfx_other_mode_changed(void);
/* Nothing is drawn while the blender keeps every pixel as it is. */
static inline bool gfx_blender_keeps_memory(void) {
    return gRdp.keeps_memory;
}
void gfx_mark_dirty(void);
void gfx_draw_triangle(int i0, int i1, int i2);
void gfx_flush_batch(void);
/* The vertices in gRsp.verts[first, first + count) changed. */
void gfx_forget_packed(int first, int count);
/* A new frame: the vertex arena starts over. */
void gfx_batches_new_frame(void);
void gfx_update_snap(void);
void gfx_weld_reset(void);

/* ---- rectangles (gfx_rect.c) -------------------------------------------- */

/* A rectangle in N64 screen pixels, textured with whatever is bound (tex is for traces). */
bool gfx_draw_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                   uint32_t color, const GuTexture* tex, bool flip); /* false if it was left out */
void gfx_fill_rect(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry);
void gfx_tex_rect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip);
void gfx_s2dex_bg(uint32_t addr, bool copy);

/* ---- frames and render targets (gfx_frame.c) ---------------------------- */

#define PROJ_NORMAL 0 /* the N64 projection, identity view */
#define PROJ_FLAT_Z 1 /* depth ignored: clip z pinned to 0, so NoN geometry in front of the near plane (2D UI) isn't dropped */
#define PROJ_FOG 2    /* a view whose z is the eye distance, so the GE's linear fog can stand in for the RSP's */

#define DEPTH_NORMAL 0 /* whole triangles, and pieces pinned to the near or far plane */
#define DEPTH_NEAR 1
#define DEPTH_FAR 2

extern bool gFrameOpen;   /* a GE display list is open for the current frame */
extern int gProjVariant;  /* PROJ_* uploaded to the GE, -1 unknown or pinned */

/* A small colour image of the game's own that is drawn on the GE and copied back (see gfx_frame.c). */
typedef struct {
    bool bound;            /* the GE draws into the target */
    bool dirty;            /* RT_VRAM holds drawing that RDRAM doesn't have yet */
    bool was_clean;        /* ... and didn't before the draw now being set up (gfx_select_target) */
    uint32_t addr, width, height, siz;
    uint32_t vram_addr, vram_sum; /* what RT_VRAM holds: RDRAM address and checksum when they last agreed */
    uint32_t pending_zclear; /* a depth image the game cleared as a colour image (0: none) */
} RenderTarget;

extern RenderTarget gTarget;
extern uint32_t gDisplayZimg; /* the depth image drawn with the screen */

void gfx_open_frame(void);
void gfx_set_viewport(void);
void gfx_ge_viewport(int* cx, int* cy, int* w, int* h);
void gfx_set_scissor(void);
void gfx_set_projection(void);
void gfx_upload_projection(int variant);
void gfx_upload_projection_depth(int variant, int depth);
bool gfx_compute_fog_range(float* near_out, float* far_out);
/* Is the current colour image one that ends up on screen? */
bool gfx_drawing_to_display(void);
bool gfx_is_display_fb(uint32_t addr);
/* Points the GE at the current colour image before a draw: the screen, or a
 * render target; false if a draw there is skipped (depth images, large
 * off-screen buffers). The screen, the usual answer, is remembered. */
extern bool gScreenSelected;
bool gfx_select_target_slow(void);
static inline bool gfx_select_target(void) {
    return gScreenSelected || gfx_select_target_slow();
}
/* G_SETCIMG / G_SETZIMG: the target has to be chosen again. */
void gfx_color_image_changed(void);
/* The display list leaves the target for the screen; its picture stays in VRAM. */
void gfx_target_leave(void);
/* Brings RDRAM up to date with what was drawn into the target (waits for the GE). */
void gfx_target_flush(void);
void gfx_target_flush_range(uint32_t addr, uint32_t len);
/* Before reading RDRAM [addr, addr + len) for a picture: a target drawn over it is copied back. */
static inline void gfx_target_need(uint32_t addr, uint32_t len) {
    if (gTarget.dirty) {
        gfx_target_flush_range(addr, len);
    }
}
/* A fill of the whole bound target, done in RDRAM too instead of copied back (see gfx_frame.c). */
bool gfx_target_fill_known(bool keep_alpha);
void gfx_target_filled(uint32_t pixel, bool keep_alpha);
void gfx_target_not_drawn(void);
bool gfx_screen_position(uint32_t addr, uint32_t* fb, float* x, float* y);
void gfx_copy_from_screen(uint32_t fb, float dx0, float dy0, float dx1, float dy1, float sx0, float sy0,
                          float sx1, float sy1);
void gfx_capture_framebuffer(uint32_t src, uint32_t dst, uint32_t width, uint32_t height);
/* Closes the frame and shows it (on the renderer thread). */
void gfx_present_frame(uint32_t framebuffer);

/* ---- statistics --------------------------------------------------------- */

/* Counters for the periodic stats lines (gfx_frame.c logs them every 120 frames). */
typedef struct {
    /* per frame */
    uint32_t draw_calls, triangles, ge_verts, vertices;
    uint32_t tri_in, tri_trivial, tri_culled, tri_clipped;
    /* per stats window */
    uint64_t render_us;       /* wall time spent rendering tasks */
    uint64_t render_cpu_us;   /* ... of which the renderer thread was on the CPU */
    uint32_t blocked_us;      /* the game waiting for the renderer */
    uint32_t submit_wait_max, submit_wait_sum, submit_wait_n;
    uint32_t vblank_wait_us;
    uint32_t captures, capture_us;
    uint32_t target_copies, target_us;
    uint32_t yields;
    /* since boot */
    uint32_t tasks, frames;
} GfxStats;

extern GfxStats gStats;

/* ---- debugging (gfx_debug.c; the switches are declared at the top) ------ */

/* The start of each graphics task: dumps, captures and traces it asks for. */
void gfx_debug_task_start(uint32_t task_number, uint32_t task);
/* False for a draw left out (skip_draws.txt, replay step mode). */
bool gfx_draw_enabled(void);
/* A buffer for this frame's screenshot, or NULL if none is wanted. */
uint32_t* gfx_debug_shot_buffer(uint32_t frame);
void gfx_debug_save_shot(uint32_t frame);

#endif
