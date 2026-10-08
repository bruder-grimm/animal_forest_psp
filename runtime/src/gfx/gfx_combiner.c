/*
 * gfx_combiner.c -- the RDP colour combiner on the GE.
 *
 * The RDP computes each pixel's colour as (A - B) * C + D per channel, over
 * one or two cycles, from the texels, the shade and constant colours. The GE
 * has five fixed texture functions instead. So the combiner is evaluated
 * numerically (eval_combiner) and probed with test inputs, and the probes
 * decide which GE function reproduces it and how the vertex colour must be
 * set up (fit_from_probes): a CombinerFit.
 *
 * Two textures are the hard part: the GE samples one. A constant second
 * texture is folded into the vertex colour; a product of the two is baked into
 * one texture; the general case bakes the pair through the combiner itself
 * (TEXVAR_COMBINE2, gfx_tex.c), and a colour of the form shade * X + Y is
 * split into passes (try_split). What remains is approximated, and logged
 * once per combiner as "gfx approx".
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx_internal.h"

/* ---- evaluating the combiner -------------------------------------------- */

/* Probe results that differ by less than this are equal (about half an 8-bit step). */
static inline bool nz(float v) {
    return v > 0.002f || v < -0.002f;
}

static inline bool approx(float a, float b) {
    return !nz(a - b);
}

typedef struct {
    float c[4];
} Col;

typedef struct {
    Col texel;   /* the texture the GE samples (or a probe of it) */
    Col other;   /* the texture it can't sample (a constant stand-in, or the same as texel) */
    int bound;   /* 0: TEXEL0 is `texel`, 1: TEXEL1 is `texel` */
    Col shade;
    Col prim, env;
    float prim_lod;
} CombInputs;

static float unpack_ch(uint32_t color, int ch) {
    return ((color >> (24 - 8 * ch)) & 0xFF) / 255.0f;
}

/* TEXEL0's and TEXEL1's channel ch (3: alpha) */
#define TEX0(in, ch) ((in)->bound == 0 ? (in)->texel.c[ch] : (in)->other.c[ch])
#define TEX1(in, ch) ((in)->bound == 1 ? (in)->texel.c[ch] : (in)->other.c[ch])

static float cc_abd(int sel, int ch, const CombInputs* in, float combined, bool is_a, bool is_d) {
    switch (sel) {
        case 0: return combined;
        case 1: return TEX0(in, ch);
        case 2: return TEX1(in, ch);
        case 3: return in->prim.c[ch];
        case 4: return in->shade.c[ch];
        case 5: return in->env.c[ch];
        case 6: return (is_a || is_d) ? 1.0f : 0.0f; /* 1 / CENTER */
        case 7: return is_a ? 0.5f : 0.0f;           /* NOISE / K4 / 0 */
        default: return 0.0f;
    }
}

static float cc_c(int sel, int ch, const CombInputs* in, float combined, float combined_a) {
    switch (sel) {
        case 0: return combined;
        case 1: return TEX0(in, ch);
        case 2: return TEX1(in, ch);
        case 3: return in->prim.c[ch];
        case 4: return in->shade.c[ch];
        case 5: return in->env.c[ch];
        case 6: return 0.0f;  /* SCALE */
        case 7: return combined_a;
        case 8: return TEX0(in, 3);
        case 9: return TEX1(in, 3);
        case 10: return in->prim.c[3];
        case 11: return in->shade.c[3];
        case 12: return in->env.c[3];
        case 13: return 0.0f; /* LOD fraction */
        case 14: return in->prim_lod;
        default: return 0.0f;
    }
}

static float ac_abd(int sel, const CombInputs* in, float combined) {
    switch (sel) {
        case 0: return combined;
        case 1: return TEX0(in, 3);
        case 2: return TEX1(in, 3);
        case 3: return in->prim.c[3];
        case 4: return in->shade.c[3];
        case 5: return in->env.c[3];
        case 6: return 1.0f;
        default: return 0.0f;
    }
}

static float ac_c(int sel, const CombInputs* in, float combined) {
    switch (sel) {
        case 0: return 0.0f; /* LOD fraction */
        case 1: return TEX0(in, 3);
        case 2: return TEX1(in, 3);
        case 3: return in->prim.c[3];
        case 4: return in->shade.c[3];
        case 5: return in->env.c[3];
        case 6: return in->prim_lod;
        default: return 0.0f;
    }
}

/* The combiner's inputs per cycle: (A - B) * C + D for the colour and for the alpha. */
typedef struct {
    int rgb_a[2], rgb_b[2], rgb_c[2], rgb_d[2];
    int alpha_a[2], alpha_b[2], alpha_c[2], alpha_d[2];
} CombinerSels;

static void decode_combiner(CombinerSels* sel) {
    uint32_t w0 = gRdp.combine0;
    uint32_t w1 = gRdp.combine1;
    *sel = (CombinerSels){
        .rgb_a = { (w0 >> 20) & 0xF, (w0 >> 5) & 0xF },
        .rgb_b = { (w1 >> 28) & 0xF, (w1 >> 24) & 0xF },
        .rgb_c = { (w0 >> 15) & 0x1F, (w0 >> 0) & 0x1F },
        .rgb_d = { (w1 >> 15) & 0x7, (w1 >> 6) & 0x7 },
        .alpha_a = { (w0 >> 12) & 0x7, (w1 >> 21) & 0x7 },
        .alpha_b = { (w1 >> 12) & 0x7, (w1 >> 3) & 0x7 },
        .alpha_c = { (w0 >> 9) & 0x7, (w1 >> 18) & 0x7 },
        .alpha_d = { (w1 >> 9) & 0x7, (w1 >> 0) & 0x7 },
    };
}

static void eval_combiner(const CombInputs* in, float* out_rgb, float* out_a) {
    CombinerSels sel;
    decode_combiner(&sel);
    int cycles = rdp_two_cycle() ? 2 : 1;

    float comb[3] = { 0, 0, 0 };
    float comb_a = 0;
    CombInputs swapped;
    for (int cyc = 0; cyc < cycles; cyc++) {
        if (cyc == 1) {
            /* second cycle: TEXEL0 and TEXEL1 trade places */
            swapped = *in;
            swapped.bound = 1 - in->bound;
            in = &swapped;
        }
        float next[3];
        for (int ch = 0; ch < 3; ch++) {
            float a = cc_abd(sel.rgb_a[cyc], ch, in, comb[ch], true, false);
            float b = cc_abd(sel.rgb_b[cyc], ch, in, comb[ch], false, false);
            float c = cc_c(sel.rgb_c[cyc], ch, in, comb[ch], comb_a);
            float d = cc_abd(sel.rgb_d[cyc], ch, in, comb[ch], false, true);
            next[ch] = (a - b) * c + d;
        }
        float a = ac_abd(sel.alpha_a[cyc], in, comb_a);
        float b = ac_abd(sel.alpha_b[cyc], in, comb_a);
        float c = ac_c(sel.alpha_c[cyc], in, comb_a);
        float d = ac_abd(sel.alpha_d[cyc], in, comb_a);
        comb_a = (a - b) * c + d;
        memcpy(comb, next, sizeof(comb));
    }
    memcpy(out_rgb, comb, sizeof(comb));
    *out_a = comb_a;
}

static int sProbeBound = 0;
static bool sProbeSame = true;
/* A texture the GE can't sample may still be known: sampled only along a
 * uniform clamped edge it is a constant. */
static bool sProbeOtherConst = false;
static float sProbeOther[4];
/* The alpha combine has already been evaluated into the texture (TEXVAR_COMBINE2),
 * so as far as the fit is concerned the texture's alpha is the answer. */
static bool sProbeBakedAlpha = false;

static void probe(float t, float ta, float s, float sa, float* rgb, float* a) {
    CombInputs in;
    in.bound = sProbeBound;
    for (int ch = 0; ch < 4; ch++) {
        in.other.c[ch] = sProbeSame ? (ch == 3 ? ta : t) : sProbeOtherConst ? sProbeOther[ch] : 0.5f;
    }
    for (int ch = 0; ch < 3; ch++) {
        in.texel.c[ch] = t;
        in.shade.c[ch] = s;
        in.prim.c[ch] = unpack_ch(gRdp.prim, ch);
        in.env.c[ch] = unpack_ch(gRdp.env, ch);
    }
    in.texel.c[3] = ta;
    in.shade.c[3] = sa;
    in.prim.c[3] = unpack_ch(gRdp.prim, 3);
    in.env.c[3] = unpack_ch(gRdp.env, 3);
    in.prim_lod = gRdp.prim_lod_frac;
    eval_combiner(&in, rgb, a);
    if (sProbeBakedAlpha) {
        *a = ta;
    }
}

/* Which tile the colour combine reads, for the texel bake below. */
int gBakeRgbTile;

/* The combiner evaluated on a pair of texels (TEXEL0, TEXEL1; RGBA8888) and a grey shade. */
static void eval_pair(uint32_t t0, uint32_t t1, float shade, float shade_a, float* rgb, float* a) {
    CombInputs in;
    in.bound = 0;
    for (int ch = 0; ch < 4; ch++) {
        in.texel.c[ch] = (float)((t0 >> (8 * ch)) & 0xFF) / 255.0f;
        in.other.c[ch] = (float)((t1 >> (8 * ch)) & 0xFF) / 255.0f;
        in.shade.c[ch] = ch == 3 ? shade_a : shade;
        in.prim.c[ch] = unpack_ch(gRdp.prim, ch);
        in.env.c[ch] = unpack_ch(gRdp.env, ch);
    }
    in.prim_lod = gRdp.prim_lod_frac;
    eval_combiner(&in, rgb, a);
}

/*
 * One texel of a TEXVAR_COMBINE2 texture: the colour of whichever tile the
 * colour combine reads, and the alpha combine evaluated on the pair. The
 * caller has already established that the alpha combine ignores the shade,
 * so the shade values here cannot affect the result.
 */
static uint32_t bake_texel(uint32_t t0, uint32_t t1) {
    float rgb[3], a;
    eval_pair(t0, t1, 1.0f, 1.0f, rgb, &a);
    uint32_t colour = (gBakeRgbTile == 0 ? t0 : t1) & 0x00FFFFFFu;
    return colour | unit_to_byte(a) << 24;
}

/*
 * The N64's alpha combiner reads only alpha inputs, so a baked texel's alpha
 * is a function of the two source alphas alone (the shade is excluded, see
 * alpha_uses_shade). Memoised per combiner state: a bake that is rebuilt every
 * frame (the K.K. dust scrolls) then costs a table lookup per texel instead of
 * evaluating the combiner in floats.
 */
uint8_t gBakeAlpha[256 * 256];
uint32_t gBakeAlphaValid[256 * 256 / 32];
static CombinerState sBakeAlphaState = { 1, 1, 1, 1, 1 };

/* Starts a bake: the table is kept while the combiner state it was made for lasts. */
void gfx_bake_alpha_prepare(void) {
    CombinerState state = rdp_combiner_state();
    if (!combiner_state_equal(&state, &sBakeAlphaState)) {
        sBakeAlphaState = state;
        memset(gBakeAlphaValid, 0, sizeof(gBakeAlphaValid));
    }
}

uint8_t gfx_bake_alpha_fill(uint32_t k) {
    gBakeAlpha[k] = (uint8_t)(bake_texel((k >> 8) << 24, (k & 0xFF) << 24) >> 24);
    gBakeAlphaValid[k >> 5] |= 1u << (k & 31);
    return gBakeAlpha[k];
}

uint8_t gfx_bake_alpha(uint8_t a0, uint8_t a1) {
    gfx_bake_alpha_prepare();
    return gfx_bake_alpha_lookup(a0, a1);
}

/* ---- split two-texture combiners ---------------------------------------- */

static bool tiles_bakeable(int a, int b, int* base_out, float* ratio_x, float* off_x, float* ratio_y,
                           float* off_y);

/*
 * The water in the village (river, waterfall) uses 2-cycle combiners whose
 * colour needs both textures or scales one by a constant, with the alpha a
 * product or sum of the two: nothing one GE texture can carry. Where the
 * colour is shade * X + Y and X, Y and the alpha Z are functions of the two
 * texels alone, it splits exactly:
 *   - X the same everywhere: one pass, GE ADD, vertex colour shade * X, texture (Y, Z);
 *   - otherwise: GE MODULATE, vertex colour shade, texture (X, Z), then, unless
 *     Y is zero, the same triangles again adding the texture (Y, Z) weighted by
 *     its alpha. (N64: clamp(S*X + Y) * Z + D * (1 - Z); two passes differ only
 *     where S*X + Y saturates, and the second pass isn't fogged.)
 * Texels are memoised per combiner state: scrolling rebakes every frame.
 */

/*
 * Memo of split texels (gfx_bake_split_lookup in gfx_internal.h). Entries are
 * tagged with the combiner state they were made for (one of the last
 * SPLIT_STATES seen) instead of the table being cleared when the state
 * changes: the river and the waterfall alternate between several states every
 * frame, and clearing made every bake start cold.
 */
SplitMemo gSplitMemo[SPLIT_MEMO];
uint8_t gSplitTag[SPLIT_MEMO]; /* 0: empty, else state index + 1, and the kind in bit 7 */
static CombinerState sSplitStates[SPLIT_STATES];
static int sSplitStateCount = 0, sSplitStateNext = 0;

/* Starts a bake: the tag its texels carry in the memo (kind and current combiner state). */
uint8_t gfx_bake_split_prepare(int kind) {
    CombinerState st = rdp_combiner_state();
    int si = -1;
    for (int i = 0; i < sSplitStateCount && si < 0; i++) {
        if (combiner_state_equal(&st, &sSplitStates[i])) {
            si = i;
        }
    }
    if (si < 0) {
        /* a new state takes the oldest slot; its old entries must go */
        si = sSplitStateNext;
        sSplitStateNext = (sSplitStateNext + 1) % SPLIT_STATES;
        if (sSplitStateCount < SPLIT_STATES) {
            sSplitStateCount++;
        } else {
            for (int k = 0; k < SPLIT_MEMO; k++) {
                if ((gSplitTag[k] & 0x7F) == si + 1) {
                    gSplitTag[k] = 0;
                }
            }
        }
        sSplitStates[si] = st;
    }
    return (uint8_t)((si + 1) | (kind == BAKE_X ? 0x80 : 0));
}

uint32_t gfx_bake_split_fill(uint32_t t0, uint32_t t1, uint8_t tag, uint32_t h) {
    float c0[3], c1[3], a;
    eval_pair(t0, t1, 0.0f, 1.0f, c0, &a);
    uint32_t v = unit_to_byte(a) << 24;
    if (tag & 0x80) {
        eval_pair(t0, t1, 1.0f, 1.0f, c1, &a);
        for (int ch = 0; ch < 3; ch++) {
            v |= unit_to_byte(c1[ch] - c0[ch]) << (8 * ch);
        }
    } else {
        for (int ch = 0; ch < 3; ch++) {
            v |= unit_to_byte(c0[ch]) << (8 * ch);
        }
    }
    gSplitTag[h] = tag;
    gSplitMemo[h].t0 = t0;
    gSplitMemo[h].t1 = t1;
    gSplitMemo[h].val = v;
    return v;
}

typedef struct {
    bool valid;
    CombinerState state;
    bool two_cycle;
    bool ok, xconst, yzero;
    float x[3];
} SplitDecision;

/* Can the current combiner be split, and how? Probes a grid of texel pairs. */
static void split_decide(SplitDecision* d) {
    static const float vals[3] = { 0.0f, 0.5f, 1.0f };
    d->ok = false;
    d->xconst = true;
    d->yzero = true;
    bool first = true;
    for (int i = 0; i < 81; i++) {
        uint32_t g0 = (uint32_t)(vals[i % 3] * 255.0f), a0 = (uint32_t)(vals[(i / 3) % 3] * 255.0f);
        uint32_t g1 = (uint32_t)(vals[(i / 9) % 3] * 255.0f), a1 = (uint32_t)(vals[i / 27] * 255.0f);
        uint32_t t0 = g0 | g0 << 8 | g0 << 16 | a0 << 24;
        uint32_t t1 = g1 | g1 << 8 | g1 << 16 | a1 << 24;
        float c0[3], ch[3], c1[3], cs[3], za, zb, zc, zs;
        eval_pair(t0, t1, 0.0f, 1.0f, c0, &za);
        eval_pair(t0, t1, 0.5f, 1.0f, ch, &zb);
        eval_pair(t0, t1, 1.0f, 1.0f, c1, &zc);
        eval_pair(t0, t1, 0.5f, 0.0f, cs, &zs); /* shade alpha must not matter */
        if (nz(za - zb) || nz(za - zc) || nz(za - zs)) {
            return; /* the alpha reads the shade */
        }
        for (int k = 0; k < 3; k++) {
            if (nz(ch[k] - 0.5f * (c0[k] + c1[k])) || nz(cs[k] - ch[k])) {
                return; /* not shade * X + Y */
            }
            float x = c1[k] - c0[k];
            if (first) {
                d->x[k] = x;
            } else if (nz(x - d->x[k])) {
                d->xconst = false;
            }
            if (nz(c0[k])) {
                d->yzero = false;
            }
        }
        first = false;
    }
    d->ok = true;
}

static bool try_split(CombinerFit* fit) {
    if (RT_SWITCH("no_split.txt")) {
        return false;
    }
    static SplitDecision cache[32];
    CombinerState st = rdp_combiner_state();
    bool two_cycle = rdp_two_cycle();
    uint32_t h = (st.c0 * 2654435761u ^ st.c1 * 2246822519u ^ st.prim * 3266489917u ^ st.env ^ st.lod * 668265263u ^
                  (uint32_t)two_cycle) >> 27;
    SplitDecision* d = &cache[h];
    if (!d->valid || !combiner_state_equal(&d->state, &st) || d->two_cycle != two_cycle) {
        d->valid = true;
        d->state = st;
        d->two_cycle = two_cycle;
        split_decide(d);
    }
    if (!d->ok) {
        return false;
    }
    int base = 0;
    float rx = 1.0f, ry = 1.0f, ox = 0.0f, oy = 0.0f;
    if (!tiles_bakeable(gRsp.tex_tile, gRsp.tex_tile + 1, &base, &rx, &ox, &ry, &oy)) {
        return false;
    }
    fit->product = false;
    fit->combine2 = true;
    fit->split = true;
    fit->approx = false;
    fit->bake_base = (int8_t)base;
    fit->bake_rgb_tile = 0;
    fit->bake_ratio_x = rx;
    fit->bake_ratio_y = ry;
    fit->bake_off_x = ox;
    fit->bake_off_y = oy;
    fit->uses_texture = true;
    fit->tex_alpha = true;
    fit->white_rgb = false;
    fit->lerp = false;
    fit->env = 0;
    fit->abase = 1.0f;
    fit->as = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        fit->base[ch] = 0.0f;
        fit->sa[ch] = 0.0f;
        fit->s[ch] = d->xconst ? d->x[ch] : 1.0f;
    }
    if (d->xconst) {
        fit->mode = TEX_ADD;
        fit->split_kind = BAKE_Y;
        fit->split_two = false;
    } else {
        fit->mode = TEX_MODULATE;
        fit->split_kind = BAKE_X;
        fit->split_two = !d->yzero;
    }
    fit->tex_tile = base;
    return true;
}

/* ---- two textures ------------------------------------------------------- */

/* Which textures does the current combiner reference? bit 0: the render
 * tile, bit 1: the tile after it. In the second cycle of a 2-cycle combiner
 * the RDP feeds TEXEL0 from the second tile and TEXEL1 from the first. */
static int combiner_texture_use(void) {
    CombinerSels sel;
    decode_combiner(&sel);
    int use = 0;
    for (int cyc = 0; cyc < (rdp_two_cycle() ? 2 : 1); cyc++) {
        int t0 = cyc == 0 ? 1 : 2;
        int t1 = cyc == 0 ? 2 : 1;
        /* (the colour's C can also be a texture's alpha: 8 TEXEL0, 9 TEXEL1) */
        const int sels[8] = { sel.rgb_a[cyc], sel.rgb_b[cyc], sel.rgb_c[cyc], sel.rgb_d[cyc],
                              sel.alpha_a[cyc], sel.alpha_b[cyc], sel.alpha_c[cyc], sel.alpha_d[cyc] };
        for (int i = 0; i < 8; i++) {
            if (sels[i] == 1 || (i == 2 && sels[i] == 8)) use |= t0;
            if (sels[i] == 2 || (i == 2 && sels[i] == 9)) use |= t1;
        }
    }
    return use;
}

static bool tiles_share_image(int a, int b) {
    TileDesc* ta = &gRdp.tiles[a & 7];
    TileDesc* tb = &gRdp.tiles[b & 7];
    return gfx_tmem_bits(ta->tmem) == gfx_tmem_bits(tb->tmem) && ta->line == tb->line &&
           ta->fmt == tb->fmt && ta->siz == tb->siz && ta->uls == tb->uls && ta->ult == tb->ult &&
           ta->lrs == tb->lrs && ta->lrt == tb->lrt;
}

/* Triangle being drawn, for the constant-texture test (NULL for rectangles). */
const RspVertex* gHintTri[3];

/* Does the hint triangle sample `tile_index` only at a clamped edge? Returns
 * a bit per axis and the edge texel index per axis. */
static int tile_pinned(int tile_index, int edge[2]) {
    edge[0] = edge[1] = -1;
    if (gHintTri[0] == NULL) {
        return 0;
    }
    const TileDesc* tile = &gRdp.tiles[tile_index & 7];
    int len[2] = { (int)((tile->lrs - tile->uls) >> 2) + 1, (int)((tile->lrt - tile->ult) >> 2) + 1 };
    uint8_t cm[2] = { tile->cms, tile->cmt };
    uint8_t mask[2] = { tile->masks, tile->maskt };
    float shift[2] = { gfx_tile_shift(tile->shifts), gfx_tile_shift(tile->shiftt) };
    float origin[2] = { tile->uls / 4.0f, tile->ult / 4.0f };
    int bits = 0;
    for (int a = 0; a < 2; a++) {
        if (!((cm[a] & 2) || mask[a] == 0)) {
            continue;
        }
        int below = 0, above = 0;
        for (int i = 0; i < 3; i++) {
            float st = (a == 0 ? gHintTri[i]->u : gHintTri[i]->v) * shift[a] / 32.0f - origin[a];
            if (st <= 0) below++;
            else if (st >= (float)(len[a] - 1)) above++;
        }
        if (below == 3 || above == 3) {
            edge[a] = below == 3 ? 0 : len[a] - 1;
            bits |= 1 << a;
        }
    }
    return bits;
}

/* The value of a tile the hint triangle samples only along a uniform clamped edge. */
static bool tile_constant(int tile_index, float out[4]) {
    int edge[2];
    if (!tile_pinned(tile_index, edge)) {
        return false;
    }
    TexKey key;
    if (!gfx_make_tile_key(&gRdp.tiles[tile_index & 7], false, &key)) {
        return false;
    }
    const GuTexture* tex = gfx_tex_get(&key);
    if (tex == NULL) {
        return false;
    }
    uint32_t gw = tex->gu_width, gh = tex->gu_height;
    uint32_t c;
    if (edge[0] >= 0 && edge[1] >= 0) {
        c = gfx_tex_texel(tex, (uint32_t)edge[0], (uint32_t)edge[1]);
    } else if (edge[0] >= 0) {
        c = gfx_tex_texel(tex, (uint32_t)edge[0], 0);
        for (uint32_t y = 1; y < gh; y++) {
            if (gfx_tex_texel(tex, (uint32_t)edge[0], y) != c) return false;
        }
    } else {
        c = gfx_tex_texel(tex, 0, (uint32_t)edge[1]);
        for (uint32_t x = 1; x < gw; x++) {
            if (gfx_tex_texel(tex, x, (uint32_t)edge[1]) != c) return false;
        }
    }
    for (int ch = 0; ch < 4; ch++) {
        out[ch] = ((c >> (8 * ch)) & 0xFF) / 255.0f;
    }
    return true;
}

/* Do the tiles address their texels identically (same size, origin, shift)? */
static bool tiles_share_coords(int a, int b) {
    TileDesc* ta = &gRdp.tiles[a & 7];
    TileDesc* tb = &gRdp.tiles[b & 7];
    return ta->uls == tb->uls && ta->ult == tb->ult && ta->lrs == tb->lrs && ta->lrt == tb->lrt &&
           ta->shifts == tb->shifts && ta->shiftt == tb->shiftt;
}

/* Does the combiner depend on the two textures only through their product? */
static bool combiner_uses_product(void) {
    static const float kPairs[3][2] = { { 0.25f, 0.8f }, { 0.6f, 0.5f }, { 1.0f, 0.3f } };
    static const float kShade[3][2] = { { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.5f, 0.2f } };
    sProbeBound = 0;
    sProbeOtherConst = true;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float rgb1[3], rgb2[3], a1, a2;
            float a = kPairs[i][0], b = kPairs[i][1];
            for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = b;
            probe(a, a, kShade[j][0], kShade[j][1], rgb1, &a1);
            for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 1.0f;
            probe(a * b, a * b, kShade[j][0], kShade[j][1], rgb2, &a2);
            if (!approx(a1, a2)) return false;
            for (int ch = 0; ch < 3; ch++) {
                if (!approx(rgb1[ch], rgb2[ch])) return false;
            }
        }
    }
    return true;
}

static const GuTexture* tile_texture(int tile_index) {
    TexKey key;
    if (!gfx_make_tile_key(&gRdp.tiles[tile_index & 7], false, &key)) {
        return NULL;
    }
    return gfx_tex_get(&key);
}

/* Change in the combiner output (summed over channels) when TEXEL0 moves
 * by d0 and TEXEL1 by d1 around the given values. */
static float combiner_sensitivity(const float t0[4], const float t1[4], float d0, float d1) {
    float rgb_a[3], rgb_b[3], a_a, a_b;
    float lo0 = t0[3] - d0 < 0 ? 0 : t0[3] - d0, hi0 = t0[3] + d0 > 1 ? 1 : t0[3] + d0;
    float lo1 = t1[3] - d1 < 0 ? 0 : t1[3] - d1, hi1 = t1[3] + d1 > 1 ? 1 : t1[3] + d1;
    sProbeBound = 0;
    sProbeOtherConst = true;
    for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = d1 > 0 ? lo1 : t1[ch];
    probe(d0 > 0 ? lo0 : t0[0], d0 > 0 ? lo0 : t0[3], 1, 1, rgb_a, &a_a);
    for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = d1 > 0 ? hi1 : t1[ch];
    probe(d0 > 0 ? hi0 : t0[0], d0 > 0 ? hi0 : t0[3], 1, 1, rgb_b, &a_b);
    float sum = a_b - a_a;
    sum = sum < 0 ? -sum : sum;
    for (int ch = 0; ch < 3; ch++) {
        float d = rgb_b[ch] - rgb_a[ch];
        sum += d < 0 ? -d : d;
    }
    return sum;
}

/*
 * Can the two tiles be laid out on one grid? They are sampled with the same
 * texture coordinates, so tile i's texel is (s >> shift_i) - uls_i / 4: with
 * equal shifts that is a constant whole-texel offset between them, which the
 * bake can absorb. The tile with the larger extent becomes the grid, so
 * neither is cut short.
 */
/* Texels the bake may write, so a pair that would need a huge grid is left to
 * the cheaper approximation rather than rebuilt every frame. */
#define BAKE_MAX_TEXELS 8192

static const char* sBakeRefusal = "";   /* why the last bake was refused (for note_approx) */

/*
 * The other tile's offset, reduced by its repeat period where it wraps: the
 * bake is the same, and scrolling textures then come back to bakes already in
 * the cache instead of needing a new one every frame.
 */
static float wrap_offset(float off, uint8_t mask, uint8_t cm) {
    if (mask == 0 || (cm & G_TX_CLAMP)) {
        return off;
    }
    float period = (float)(1u << mask) * ((cm & G_TX_MIRROR) ? 2.0f : 1.0f);
    off = fmodf(off, period);
    return off < 0.0f ? off + period : off;
}

static bool tiles_bakeable(int a, int b, int* base_out, float* ratio_x, float* off_x, float* ratio_y,
                           float* off_y) {
    TileDesc* t[2] = { &gRdp.tiles[a & 7], &gRdp.tiles[b & 7] };
    int32_t w[2], h[2];
    for (int i = 0; i < 2; i++) {
        w[i] = ((int32_t)(t[i]->lrs - t[i]->uls) >> 2) + 1;
        h[i] = ((int32_t)(t[i]->lrt - t[i]->ult) >> 2) + 1;
        if (w[i] <= 0 || h[i] <= 0) {
            sBakeRefusal = "empty tile";
            return false;
        }
    }
    /* The bake is laid out on the tile that covers more; gfx_tex.c then
     * refines that grid to the finer tile's rate (bake_sub). */
    int base = (w[1] * h[1] > w[0] * h[0]) ? 1 : 0;
    int other = base ^ 1;
    if ((int64_t)w[base] * h[base] > BAKE_MAX_TEXELS) {
        sBakeRefusal = "too large to bake every frame";
        return false;
    }
    /*
     * Both tiles see the same coordinate s: tile i's texel is
     * s * shift_i - uls_i / 4. Eliminating s, the other tile's texel for grid
     * texel x is x * (shift_other / shift_base) + (uls_base / 4) * ratio
     * - uls_other / 4.
     */
    float fs[2] = { gfx_tile_shift(t[0]->shifts), gfx_tile_shift(t[1]->shifts) };
    float ft[2] = { gfx_tile_shift(t[0]->shiftt), gfx_tile_shift(t[1]->shiftt) };
    if (fs[base] == 0.0f || ft[base] == 0.0f) {
        sBakeRefusal = "zero tile shift";
        return false;
    }
    float rx = fs[other] / fs[base];
    float ry = ft[other] / ft[base];
    *ratio_x = rx;
    *ratio_y = ry;
    *off_x = wrap_offset((float)t[base]->uls / 4.0f * rx - (float)t[other]->uls / 4.0f, t[other]->masks, t[other]->cms);
    *off_y = wrap_offset((float)t[base]->ult / 4.0f * ry - (float)t[other]->ult / 4.0f, t[other]->maskt, t[other]->cmt);
    *base_out = base;
    return true;
}

/* Does the alpha combine read the shade? Then it is not a function of the two
 * texels alone and cannot be baked into a texture. */
static bool alpha_uses_shade(void) {
    float rgb[3], a0, a1;
    sProbeBound = 0;
    sProbeOtherConst = true;
    for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 0.4f;
    probe(0.3f, 0.7f, 0.5f, 0.0f, rgb, &a0);
    probe(0.3f, 0.7f, 0.5f, 1.0f, rgb, &a1);
    if (!approx(a0, a1)) {
        return true;
    }
    probe(0.6f, 0.2f, 0.25f, 0.3f, rgb, &a0);
    probe(0.6f, 0.2f, 0.8f, 0.3f, rgb, &a1);
    return !approx(a0, a1);
}

/* Does the colour (or alpha) combine depend on the tile at `which`? */
static void combiner_tile_use(int which, bool* rgb_used, bool* alpha_used) {
    float rgb0[3], rgb1[3], a0, a1;
    sProbeBound = (which == 0) ? 1 : 0; /* vary `which` through sProbeOther */
    sProbeOtherConst = true;
    for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 0.2f;
    probe(0.5f, 0.5f, 0.7f, 0.7f, rgb0, &a0);
    for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 0.9f;
    probe(0.5f, 0.5f, 0.7f, 0.7f, rgb1, &a1);
    *alpha_used = !approx(a0, a1);
    *rgb_used = false;
    for (int ch = 0; ch < 3; ch++) {
        if (!approx(rgb0[ch], rgb1[ch])) {
            *rgb_used = true;
        }
    }
}

/*
 * The general two-texture case: bake the pair into one texture. Possible when
 * the alpha combine is a function of the two texels alone (no shade) and the
 * colour combine reads at most one of them, so the baked texel can carry that
 * tile's colour and the combined alpha. The K.K. Slider spotlight is this:
 * colour from the scrolling dust, alpha the cone's times one plus the dust's.
 */
static bool combiner_bakeable(CombinerFit* fit) {
    bool rgb0, a0, rgb1, a1;
    combiner_tile_use(0, &rgb0, &a0);
    combiner_tile_use(1, &rgb1, &a1);
    /*
     * The baked texel carries one tile's colour and an alpha computed from both
     * tiles' alphas. That covers any combiner whose colour reads at most one
     * tile and whose alpha reads no shade -- including colour from one tile and
     * alpha from the other (a colour image with a separate mask).
     */
    if (rgb0 && rgb1) {
        sBakeRefusal = "colour needs both textures";
        return false; /* one texel cannot carry that */
    }
    if (alpha_uses_shade()) {
        sBakeRefusal = "alpha reads the shade";
        return false;
    }
    int base = 0;
    float rx = 1.0f, ry = 1.0f, ox = 0.0f, oy = 0.0f;
    if (!tiles_bakeable(gRsp.tex_tile, gRsp.tex_tile + 1, &base, &rx, &ox, &ry, &oy)) {
        return false; /* tiles_bakeable says why */
    }
    fit->bake_rgb_tile = (int8_t)(rgb1 ? 1 : 0);
    fit->bake_base = (int8_t)base;
    fit->bake_ratio_x = rx;
    fit->bake_ratio_y = ry;
    fit->bake_off_x = ox;
    fit->bake_off_y = oy;
    return true;
}

/*
 * Where the renderer approximates a combiner, say so -- once per kind and
 * combiner, so a whole play-through gives the list of effects to check
 * against the N64 (it found the K.K. spotlight's lost dust).
 */
static void note_approx(const char* kind, float extra) {
    static uint32_t seen[128];
    static int nseen = 0;
    uint32_t h = gRdp.combine0 * 2654435761u ^ gRdp.combine1 * 2246822519u ^ (uint32_t)(uintptr_t)kind;
    for (int i = 0; i < nseen; i++) {
        if (seen[i] == h) {
            return;
        }
    }
    if (nseen < 128) {
        seen[nseen++] = h;
    }
    rt_log("gfx approx: %s (cc %06X %08X, prim %08X env %08X, %.2f)", kind, (unsigned)gRdp.combine0,
           (unsigned)gRdp.combine1, (unsigned)gRdp.prim, (unsigned)gRdp.env, (double)extra);
}

/* Two unrelated textures: sample the one that shapes the result more and
 * stand in the other's average colour. */
static void pick_varying_texture(void) {
    const GuTexture* a = tile_texture(gRsp.tex_tile);
    const GuTexture* b = tile_texture(gRsp.tex_tile + 1);
    if (a == NULL || b == NULL) {
        return;
    }
    float s0 = combiner_sensitivity(a->mean, b->mean, a->dev, 0);
    float s1 = combiner_sensitivity(a->mean, b->mean, 0, b->dev);
    const GuTexture* other = s1 > s0 ? a : b;
    {
        char why[160];
        const TileDesc* t0 = &gRdp.tiles[gRsp.tex_tile & 7];
        const TileDesc* t1 = &gRdp.tiles[(gRsp.tex_tile + 1) & 7];
        snprintf(why, sizeof(why), "two textures, one replaced by its average colour -- not baked: %s; tiles %ux%u and %ux%u",
                 sBakeRefusal, (unsigned)(((t0->lrs - t0->uls) >> 2) + 1), (unsigned)(((t0->lrt - t0->ult) >> 2) + 1),
                 (unsigned)(((t1->lrs - t1->uls) >> 2) + 1), (unsigned)(((t1->lrt - t1->ult) >> 2) + 1));
        note_approx(why, other->dev);
    }
    sProbeBound = s1 > s0 ? 1 : 0;
    sProbeOtherConst = true;
    memcpy(sProbeOther, other->mean, sizeof(sProbeOther));
}

/* ---- fitting ------------------------------------------------------------ */

int gfx_pin_signature(void) {
    int e[2];
    return (tile_pinned(gRsp.tex_tile, e) ? 1 : 0) | (tile_pinned(gRsp.tex_tile + 1, e) ? 2 : 0);
}

/* Does the current combiner read both texture tiles? (Its fit then depends on the tiles and the triangle.) */
bool gfx_combiner_reads_two_textures(void) {
    return combiner_texture_use() == 3;
}

static void fit_from_probes(CombinerFit* fit);
static bool sDeferApprox = false; /* a baked pair may still be split exactly: report fallbacks after */

/* Fits depend only on the combiner, its constant inputs and the texture
 * choice made above them, so they are memoised. */
typedef struct {
    uint32_t c0, c1, prim, env;
    float lod;
    float other[4];
    uint8_t two_cycle, bound, same, other_const;
    uint8_t combine2, bake_rgb_tile;
} FitKey;

typedef struct {
    bool valid;
    FitKey key;
    CombinerFit fit;
} FitCacheEntry;

#define FIT_CACHE_SIZE 256
static FitCacheEntry sFitCache[FIT_CACHE_SIZE];

static void cached_fit(CombinerFit* fit) {
    FitKey key;
    memset(&key, 0, sizeof(key));
    key.c0 = gRdp.combine0;
    key.c1 = gRdp.combine1;
    key.prim = gRdp.prim;
    key.env = gRdp.env;
    key.lod = gRdp.prim_lod_frac;
    key.two_cycle = rdp_two_cycle();
    key.bound = (uint8_t)sProbeBound;
    key.same = sProbeSame;
    key.other_const = sProbeOtherConst;
    key.combine2 = fit->combine2;
    key.bake_rgb_tile = (uint8_t)fit->bake_rgb_tile;
    if (sProbeOtherConst) {
        memcpy(key.other, sProbeOther, sizeof(key.other));
    }
    uint32_t h = key.c0 * 2654435761u ^ key.c1 * 2246822519u ^ key.prim * 3266489917u ^ key.env * 668265263u ^
                 (uint32_t)(key.lod * 255.0f) * 374761393u ^ (uint32_t)key.two_cycle << 1 ^ (uint32_t)key.bound << 2 ^
                 (uint32_t)key.same << 3;
    h ^= h >> 15;
    FitCacheEntry* e = &sFitCache[h & (FIT_CACHE_SIZE - 1)];
    int pin_sig = fit->pin_sig;
    bool product = fit->product;
    bool combine2 = fit->combine2;
    int8_t bake_rgb_tile = fit->bake_rgb_tile, bake_base = fit->bake_base;
    float bake_ratio_x = fit->bake_ratio_x, bake_off_x = fit->bake_off_x;
    float bake_ratio_y = fit->bake_ratio_y, bake_off_y = fit->bake_off_y;
    int tex_tile = fit->tex_tile;
    if (e->valid && memcmp(&e->key, &key, sizeof(key)) == 0) {
        *fit = e->fit;
    } else {
        fit_from_probes(fit);
        e->valid = true;
        e->key = key;
        e->fit = *fit;
    }
    fit->pin_sig = pin_sig;
    fit->product = product;
    fit->combine2 = combine2;
    fit->bake_rgb_tile = bake_rgb_tile;
    fit->bake_base = bake_base;
    fit->bake_ratio_x = bake_ratio_x;
    fit->bake_ratio_y = bake_ratio_y;
    fit->bake_off_x = bake_off_x;
    fit->bake_off_y = bake_off_y;
    fit->tex_tile = tex_tile;
}

void gfx_classify_combiner(CombinerFit* fit) {
    int use = combiner_texture_use();
    sProbeBound = (use == 2) ? 1 : 0;
    sProbeSame = !(use & 1) || !(use & 2) || tiles_share_image(gRsp.tex_tile, gRsp.tex_tile + 1);
    sProbeOtherConst = false;
    sProbeBakedAlpha = false;
    fit->pin_sig = -1;
    fit->product = false;
    fit->combine2 = false;
    fit->bake_rgb_tile = 0;
    fit->bake_base = 0;
    fit->bake_ratio_x = fit->bake_ratio_y = 1.0f;
    fit->bake_off_x = fit->bake_off_y = 0.0f;
    fit->approx = false;
    fit->split = false;
    fit->split_two = false;
    fit->split_kind = BAKE_COLOUR;
    if (!sProbeSame) {
        /* Two different textures: sample the one that varies, fold a constant
         * one in, or bake their product when that's all the combiner uses. */
        fit->pin_sig = gfx_pin_signature();
        if (tile_constant(gRsp.tex_tile, sProbeOther)) {
            sProbeBound = 1;
            sProbeOtherConst = true;
        } else if (tile_constant(gRsp.tex_tile + 1, sProbeOther)) {
            sProbeBound = 0;
            sProbeOtherConst = true;
        } else if (tiles_share_coords(gRsp.tex_tile, gRsp.tex_tile + 1) && combiner_uses_product()) {
            sProbeBound = 0;
            sProbeOtherConst = true;
            for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 1.0f;
            fit->product = true;
        } else if (combiner_bakeable(fit)) {
            if (fit->bake_ratio_x > 1.5f || fit->bake_ratio_y > 1.5f || fit->bake_ratio_x < -1.5f ||
                fit->bake_ratio_y < -1.5f) {
                note_approx("two textures baked, the second at a finer rate",
                            fit->bake_ratio_x > fit->bake_ratio_y ? fit->bake_ratio_x : fit->bake_ratio_y);
            }
            /* The pair is baked into one texture: the fit sees that tile's
             * colour in TEXEL0 and takes the texture's alpha as the answer. */
            fit->combine2 = true;
            sProbeBound = fit->bake_rgb_tile;
            sProbeOtherConst = true;
            for (int ch = 0; ch < 4; ch++) sProbeOther[ch] = 1.0f;
            sProbeBakedAlpha = true;
        } else if (try_split(fit)) {
            fit->pin_sig = gfx_pin_signature();
            return;
        } else {
            sProbeBound = (use == 2) ? 1 : 0;
            sProbeOtherConst = false;
            pick_varying_texture();
        }
    }
    /* The baked texture is laid out on one tile's grid, so the texture
     * coordinates must be normalised by that tile, whichever one the colour
     * combine happened to read. */
    fit->tex_tile = fit->combine2 ? fit->bake_base : sProbeBound;
    sDeferApprox = fit->combine2;
    cached_fit(fit);
    sDeferApprox = false;
    sProbeBakedAlpha = false;
    if (fit->combine2 && fit->approx) {
        /* baked, but the colour didn't fit the GE: split it instead if it can be */
        CombinerFit split = *fit;
        if (try_split(&split)) {
            *fit = split;
        } else {
            note_approx("combiner fallback (modulate by the white-texel result)", 0);
        }
    }
}

/*
 * Is the colour shade * (lo + (hi - lo) * texel) per channel, lo and hi
 * colours? Then the GE modulates a texture holding the lerp by the shade --
 * exact where the white-texel fallback turns every dark texel black (the wet
 * sand along the beach: shade * lerp(ENV, PRIM, TEXEL0) with an I texture that
 * is 0 on the water side). The probes only see the corners, so a midpoint
 * must agree too.
 */
static bool shade_times_lerp(const float p0[3], const float kT[3], const float kS[3], const float kTS[3],
                             const float kSa[3], const float kTSa[3], CombinerFit* fit) {
    uint32_t lo = 0, hi = 0;
    for (int ch = 0; ch < 3; ch++) {
        if (nz(p0[ch]) || nz(kT[ch]) || nz(kSa[ch]) || nz(kTSa[ch])) {
            return false;
        }
        float l = kS[ch], h = kS[ch] + kTS[ch];
        if (l < -0.002f || l > 1.002f || h < -0.002f || h > 1.002f) {
            return false;
        }
        lo |= unit_to_byte(l) << (8 * ch);
        hi |= unit_to_byte(h) << (8 * ch);
    }
    float mid[3], mid_a;
    probe(0.5f, 0.0f, 0.5f, 0.0f, mid, &mid_a);
    for (int ch = 0; ch < 3; ch++) {
        if (!approx(mid[ch], 0.5f * (kS[ch] + 0.5f * kTS[ch]))) {
            return false;
        }
    }
    fit->lerp = true;
    fit->lerp_lo = lo;
    fit->lerp_hi = hi;
    return true;
}

/*
 * Probes the combiner at the corners of (texel, texel alpha, shade, shade
 * alpha) and reads off, per channel, the constant and the coefficient of each
 * input and product of inputs; the pattern of those says which GE texture
 * function reproduces it.
 */
static void fit_from_probes(CombinerFit* fit) {
    float p0[3], p_t[3], p_ta[3], p_s[3], p_sa[3], p_ts[3], p_tsa[3], p_tta[3], p_tas[3];
    float a0, a_ta, a_sa, a_unused;

    probe(0, 0, 0, 0, p0, &a0);
    probe(1, 0, 0, 0, p_t, &a_unused);
    probe(0, 1, 0, 0, p_ta, &a_ta);
    probe(0, 0, 1, 0, p_s, &a_unused);
    probe(0, 0, 0, 1, p_sa, &a_sa);
    probe(1, 0, 1, 0, p_ts, &a_unused);
    probe(1, 0, 0, 1, p_tsa, &a_unused);
    probe(1, 1, 0, 0, p_tta, &a_unused);
    probe(0, 1, 1, 0, p_tas, &a_unused);

    bool uses_t = false, uses_ta_rgb = false;
    bool decal = true, modulate = true, add = true, blend = true;
    float kT[3], kTS[3], kTSa[3], kS[3], kSa[3];

    for (int ch = 0; ch < 3; ch++) {
        float c = p0[ch];
        float dT = p_t[ch] - c;
        float dTa = p_ta[ch] - c;
        float dS = p_s[ch] - c;
        float dSa = p_sa[ch] - c;
        float dTS = p_ts[ch] - c - dT - dS;
        float dTSa = p_tsa[ch] - c - dT - dSa;
        float dTTa = p_tta[ch] - c - dT - dTa;
        float dTaS = p_tas[ch] - c - dTa - dS;

        kT[ch] = dT;
        kTS[ch] = dTS;
        kTSa[ch] = dTSa;
        kS[ch] = dS;
        kSa[ch] = dSa;

        if (nz(dT) || nz(dTS) || nz(dTSa) || nz(dTTa)) uses_t = true;
        if (nz(dTa) || nz(dTTa) || nz(dTaS)) uses_ta_rgb = true;

        if (!(approx(dTTa, 1) && approx(dTaS, -1))) decal = false;
        if (nz(c) || nz(dS) || nz(dSa) || nz(dTa)) modulate = false;
        if (!approx(dT, 1) || nz(dTS) || nz(dTSa) || nz(dTa)) add = false;
        if (!approx(dTS, -dS) || !approx(dTSa, -dSa) || nz(dTa)) blend = false;

        fit->base[ch] = c;
        fit->s[ch] = dS;
        fit->sa[ch] = dSa;
    }

    /* The alpha combiner reads the texel alpha probe `ta` and the shade alpha `sa`. */
    float rgb_unused[3], a_tasa;
    probe(0, 1, 0, 1, rgb_unused, &a_tasa);
    float alpha_t = a_ta - a0;
    float alpha_s = a_sa - a0;
    float alpha_ts = a_tasa - a0 - alpha_t - alpha_s;
    bool alpha_uses_t = nz(alpha_t) || nz(alpha_ts);

    if (!alpha_uses_t) {
        fit->tex_alpha = false;
        fit->abase = a0;
        fit->as = alpha_s;
    } else if (!nz(a0) && !nz(alpha_s)) {
        fit->tex_alpha = true;
        fit->abase = alpha_t;
        fit->as = alpha_ts;
    } else {
        fit->tex_alpha = true;
        fit->abase = a0 + alpha_t;
        fit->as = alpha_s + alpha_ts;
    }

    fit->white_rgb = false;
    fit->lerp = false;
    fit->env = 0;
    fit->approx = false;

    if (!uses_t && !uses_ta_rgb) {
        if (alpha_uses_t) {
            fit->mode = TEX_MODULATE;
            fit->white_rgb = true;
        } else {
            fit->mode = TEX_NONE;
        }
    } else if (uses_ta_rgb && decal) {
        fit->mode = TEX_DECAL;
    } else if (modulate) {
        fit->mode = TEX_MODULATE;
        for (int ch = 0; ch < 3; ch++) {
            fit->base[ch] = kT[ch];
            fit->s[ch] = kTS[ch];
            fit->sa[ch] = kTSa[ch];
        }
    } else if (add) {
        fit->mode = TEX_ADD;
    } else if (blend) {
        fit->mode = TEX_BLEND;
        uint32_t env = 0xFF000000;
        for (int ch = 0; ch < 3; ch++) {
            env |= unit_to_byte(p0[ch] + kT[ch]) << (8 * ch);
        }
        fit->env = env;
    } else if (!uses_ta_rgb && !sProbeBakedAlpha && shade_times_lerp(p0, kT, kS, kTS, kSa, kTSa, fit)) {
        /* shade * lerp(lo, hi, texel): the lerp goes into the texture (TEXVAR_LERP) */
        fit->mode = TEX_MODULATE;
        for (int ch = 0; ch < 3; ch++) {
            fit->base[ch] = 0.0f;
            fit->s[ch] = 1.0f;
            fit->sa[ch] = 0.0f;
        }
    } else {
        /* Fallback: modulate by the value the combiner produces for a white texel. */
        fit->mode = TEX_MODULATE;
        for (int ch = 0; ch < 3; ch++) {
            fit->base[ch] = p0[ch] + kT[ch];
            fit->s[ch] = kS[ch] + kTS[ch];
            fit->sa[ch] = kSa[ch] + kTSa[ch];
        }
        if (!sDeferApprox) {
            note_approx("combiner fallback (modulate by the white-texel result)", 0);
        }
        fit->approx = true;
    }
    fit->uses_texture = fit->mode != TEX_NONE;
}
