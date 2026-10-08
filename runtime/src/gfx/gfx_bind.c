/*
 * gfx_bind.c -- texture binding: which texture a tile is, and how the GE samples it.
 *
 * A render tile describes texels in the RDP's texture memory, which the loads
 * (gfx_dl.c) tie back to RDRAM: gfx_make_tile_key resolves that into a
 * TexKey, the texture cache (gfx_tex.c) makes or finds the GE texture, and
 * binding it sets the GE's texture state and the transform from the RDP's
 * texture coordinates to the GE's (gTexTransform). The combiner's fit says
 * which variant to bind: a product or a bake of two tiles, a lerp, a white
 * or bled one.
 */
#include <string.h>

#include "gfx_internal.h"

TexTransform gTexTransform = { 1.0f, 1.0f, 0.0f, 0.0f };
TexKey gBoundKey;
static bool sSplitSecond = false; /* gfx_bind_texture: only look up the split's BAKE_Y texture */

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

RT_SPRAM bool gfx_make_tile_key(const TileDesc* tile, bool white_rgb, TexKey* key) {
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
        key->tlut_type = rdp_tlut_type();
        uint32_t slot = tile->siz == G_IM_SIZ_4b ? (uint32_t)tile->palette * 16 : 0;
        key->tlut_addr = gRdp.tlut[slot & 0xFF] & 0x1FFFFFFF;
    }
    return true;
}

/*
 * Everything a plain one-texture bind reads (the common case: no product, bake or lerp), so
 * that binding the same texture again can skip building its key and looking it up. The
 * answer is the cache entry; whether it still is the right one is checked by
 * gfx_tex_struct_gen (no entry was freed since) and gfx_tex_touch (its texels and palette
 * are still the ones it was made from -- the checks a lookup makes once a frame).
 */
typedef struct {
    uint32_t uls, ult, lrs, lrt;
    uint32_t misc[4];      /* fmt..shiftt, tmem and line, packed */
    uint32_t tmem_sig;     /* where texture memory's texels come from */
    uint32_t tlut;         /* a CI texture's palette address */
    uint32_t mode;         /* the other-mode bits that pick the palette format and the filter */
    uint32_t flags;        /* white_rgb (without black), aa edge */
} BindIn;

typedef struct {
    BindIn in;
    uint32_t gen;
    int entry;
    bool valid;
} BindMemo;

#define BIND_MEMOS 256
static BindMemo sBindMemo[BIND_MEMOS];

static uint32_t bind_hash(const BindIn* in) {
    const uint32_t* w = (const uint32_t*)in;
    uint32_t h = 0x811C9DC5u;
    for (unsigned i = 0; i < sizeof(BindIn) / 4; i++) {
        h = (h ^ w[i]) * 0x9E3779B1u;
        h ^= h >> 15;
    }
    return h;
}

static bool bind_same(const BindIn* a, const BindIn* b) {
    const uint32_t* x = (const uint32_t*)a;
    const uint32_t* y = (const uint32_t*)b;
    uint32_t diff = 0;
    for (unsigned i = 0; i < sizeof(BindIn) / 4; i++) {
        diff |= x[i] ^ y[i];
    }
    return diff == 0;
}

/* The state a bound texture sets: the coordinate transform, the sampler's wrap and filter, the texture itself. */
static void bind_finish(const GuTexture* tex, const TileDesc* tile) {
    /* Texture coordinate transform, as the RDP does it: shift the S/T
     * coordinate (1/32 texel units) first, then subtract the tile origin
     * (uls/ult, 1/4 texel), then normalise:
     *   u = (s * shift - uls * 8) / (32 * width) = s * scale - offset */
    float shift_u = gfx_tile_shift(tile->shifts);
    float shift_v = gfx_tile_shift(tile->shiftt);
    /* a bake on a finer grid has sub GU texels per tile texel */
    float norm_u = (float)(tex->sub_x ? tex->sub_x : 1) / (32.0f * tex->gu_width);
    float norm_v = (float)(tex->sub_y ? tex->sub_y : 1) / (32.0f * tex->gu_height);
    gTexTransform.scale_u = shift_u * norm_u;
    gTexTransform.scale_v = shift_v * norm_v;
    gTexTransform.off_u = ((float)tile->uls * 8.0f + (float)tex->win_s * 32.0f) * norm_u;
    gTexTransform.off_v = ((float)tile->ult * 8.0f + (float)tex->win_t * 32.0f) * norm_v;

    gfx_gu_texture_image(tex);
    gfx_gu_tex_sampler(tex->clamp_s ? GU_CLAMP : GU_REPEAT, tex->clamp_t ? GU_CLAMP : GU_REPEAT,
                       ((gRdp.other_h >> G_MDSFT_TEXTFILT) & 3) == G_TF_POINT ? GU_NEAREST : GU_LINEAR);
}

/*
 * A two-texture bake may hold only the texels its draw reaches (gfx_bake_window): those
 * of the last vertices loaded and of the triangle it is bound for. The batch's later
 * triangles must stay inside: sWindowLo..sWindowHi per windowed axis (gfx_bake_window_left).
 */
uint8_t gBakeWindow = 0;
static int sWindowTile;
static float sWindowLo[2], sWindowHi[2];

/* Widens [lo, hi] to the texels of `tile` that v reaches, per axis (as tile_pinned reads them). */
static void reach(const TileDesc* tile, const RspVertex* v, float lo[2], float hi[2]) {
    float shift[2] = { gfx_tile_shift(tile->shifts), gfx_tile_shift(tile->shiftt) };
    float st[2] = { v->u * shift[0] / 32.0f - tile->uls / 4.0f, v->v * shift[1] / 32.0f - tile->ult / 4.0f };
    for (int a = 0; a < 2; a++) {
        lo[a] = st[a] < lo[a] ? st[a] : lo[a];
        hi[a] = st[a] > hi[a] ? st[a] : hi[a];
    }
}

static __attribute__((noinline)) void bake_window(TexKey* key, int tile_index) {
    const TileDesc* tile = &gRdp.tiles[tile_index & 7];
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    for (int i = 0; i < 3; i++) {
        reach(tile, gHintTri[i], lo, hi);
    }
    for (int i = gRsp.vtx_first; i < gRsp.vtx_first + gRsp.vtx_count && i < MAX_VERTICES; i++) {
        reach(tile, &gRsp.verts[i], lo, hi);
    }
    if (!gfx_bake_window(key, lo, hi) || sSplitSecond) {
        return;
    }
    gBakeWindow = (uint8_t)((key->win_w ? 1 : 0) | (key->win_h ? 2 : 0));
    sWindowTile = tile_index;
    /* where the GE's filter stays inside the window */
    sWindowLo[0] = key->win_x0 + 0.5f;
    sWindowHi[0] = key->win_x0 + key->win_w - 0.5f;
    sWindowLo[1] = key->win_y0 + 0.5f;
    sWindowHi[1] = key->win_y0 + key->win_h - 0.5f;
}

/* Does the triangle reach past the bound bake's window? */
__attribute__((noinline)) bool gfx_bake_window_left(void) {
    float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
    for (int i = 0; i < 3; i++) {
        reach(&gRdp.tiles[sWindowTile & 7], gHintTri[i], lo, hi);
    }
    for (int a = 0; a < 2; a++) {
        if ((gBakeWindow >> a & 1) && (lo[a] < sWindowLo[a] || hi[a] > sWindowHi[a])) {
            return true;
        }
    }
    return false;
}

RT_SPRAM const GuTexture* gfx_bind_texture(int tile_index, const CombinerFit* fit) {
    TileDesc* tile = &gRdp.tiles[tile_index & 7];
    TexKey key;
    if (!sSplitSecond) {
        gBakeWindow = 0;
    }
    /* Modulated by black, a texture's colour is all the same: the plain one will do
     * (and can be a render target's picture as it is -- the shadow under a pocket item). */
    bool black = fit->mode == TEX_MODULATE;
    for (int ch = 0; ch < 3; ch++) {
        black = black && fit->base[ch] == 0.0f && fit->s[ch] == 0.0f && fit->sa[ch] == 0.0f;
    }
    bool aa_edge = !fit->product && !fit->combine2 && gfx_aa_edge_mode();
    BindMemo* memo = NULL;
    if (!fit->product && !fit->combine2 && !fit->lerp && !sSplitSecond && !gTracing && !gTarget.dirty) {
        BindIn in;
        in.uls = tile->uls;
        in.ult = tile->ult;
        in.lrs = tile->lrs;
        in.lrt = tile->lrt;
        in.misc[0] = (uint32_t)tile->fmt | (uint32_t)tile->siz << 8 | (uint32_t)tile->palette << 16 | (uint32_t)tile->cms << 24;
        in.misc[1] = (uint32_t)tile->cmt | (uint32_t)tile->masks << 8 | (uint32_t)tile->maskt << 16 | (uint32_t)tile->shifts << 24;
        in.misc[2] = (uint32_t)tile->shiftt | (uint32_t)tile->tmem << 8 | (uint32_t)tile->line << 24;
        in.misc[3] = (uint32_t)tile->line >> 8;
        in.tmem_sig = gRdp.tmem_sig;
        in.tlut = 0;
        if (tile->fmt == G_IM_FMT_CI) {
            in.tlut = gRdp.tlut[(tile->siz == G_IM_SIZ_4b ? (uint32_t)tile->palette * 16 : 0) & 0xFF];
        }
        in.mode = gRdp.other_h & ((3u << G_MDSFT_TEXTLUT) | (3u << G_MDSFT_TEXTFILT));
        in.flags = (uint32_t)(fit->white_rgb && !black) | (uint32_t)aa_edge << 1;
        memo = &sBindMemo[bind_hash(&in) & (BIND_MEMOS - 1)];
        if (memo->valid && memo->gen == gfx_tex_struct_gen() && bind_same(&memo->in, &in) && gfx_tex_touch(memo->entry)) {
            const GuTexture* tex = gfx_tex_entry(memo->entry);
            bind_finish(tex, tile);
            return tex;
        }
        memo->in = in;   /* (filled in as the answer is found) */
        memo->valid = false;
    }
    if (!gfx_make_tile_key(tile, fit->white_rgb && !black, &key)) {
        return NULL;
    }
    if (aa_edge) {
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
            gfx_tex_set_second(&key, &k2);
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
            gfx_tex_set_second(&key, &k2);
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
            if (gHintTri[0] != NULL) {
                bake_window(&key, tile_index);
            }
        }
    }
    if (sSplitSecond) {
        /* the second pass of a split combiner: same bake, the other half; not bound here */
        key.bake_kind = BAKE_Y;
        return gfx_tex_get(&key);
    }
    const GuTexture* tex = gfx_tex_get_to_draw(&key);
    if (tex == NULL) {
        return NULL;
    }
    gBoundKey = key;

    bind_finish(tex, tile);
    if (memo != NULL) {
        int entry = gfx_tex_entry_index(tex);
        if (entry >= 0) {
            memo->gen = gfx_tex_struct_gen();
            memo->entry = entry;
            memo->valid = true;
        }
    }
    return tex;
}

const GuTexture* gfx_split_second_texture(int tile_index, const CombinerFit* fit) {
    sSplitSecond = true;
    const GuTexture* tex = gfx_bind_texture(tile_index, fit);
    sSplitSecond = false;
    return tex;
}

/* (run from the scratchpad: see spram.c) */
RT_SPRAM_ENTRY(gfx_make_tile_key);
RT_SPRAM_ENTRY(gfx_bind_texture);
