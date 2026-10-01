/*
 * gfx_tex.c -- N64 texture decoding and the texture cache.
 *
 * Textures are decoded at power-of-two sizes, with the RDP's clamp/mask/mirror
 * addressing baked in (the GE only clamps or repeats), and stored in the GE's
 * swizzled block order, as 16-bit texels where that loses nothing (pick_psm).
 * Two-texture variants -- products and combiner bakes (TEXVAR_*) -- are built
 * here too, from texels the combiner code evaluates (gfx_combiner.c).
 *
 * Entries are validated against content hashes, since the game can rewrite
 * texture memory (player-designed patterns, time-of-day palettes): palettes
 * every frame they are used, texels every TEXEL_CHECK_PERIOD frames.
 */
#include <malloc.h>
#include <math.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_internal.h"

#define CACHE_SLOTS 2048
#define CACHE_MAX_ENTRIES 1024
#define CACHE_BUDGET_MIN (3u * 1024 * 1024)
#define CACHE_BUDGET_MAX (10u * 1024 * 1024)
static uint32_t sBudget = CACHE_BUDGET_MIN;
#define MAX_TEX_DIM 512
#define TEXEL_CHECK_PERIOD 8 /* power of two */

typedef struct {
    bool used;
    TexKey key;
    uint32_t texel_hash;
    uint32_t pal_hash;
    uint32_t checked_frame;
    uint32_t last_used;
    bool stale;           /* its memory was drawn into (gfx_tex_invalidate_range) */
    uint32_t bytes;
    GuTexture tex;
} CacheEntry;

/*
 * The RDRAM each entry's texels are read from (and its second source's; empty
 * for an entry that has none, or is free). Kept apart from the entries because
 * every render target copy-back looks through all of them, and an entry is
 * 200 bytes: that was a quarter of a millisecond a time on the PSP.
 */
typedef struct {
    uint32_t start, end;
    uint32_t start2, end2;
} SourceSpan;

static CacheEntry sEntries[CACHE_MAX_ENTRIES];
static SourceSpan sSpans[CACHE_MAX_ENTRIES];
static int sEntriesTop = 0; /* no entry at or above this index has been used */
static int16_t sSlots[CACHE_SLOTS];
static uint32_t sFrame = 1;
static uint32_t sBytesUsed = 0;

/* Decode scratch: N64 textures are at most 256x256; S2DEX backgrounds (and
 * captured framebuffers) are up to 320x240. */
#define MAX_DECODED_TEXELS (MAX_TEX_DIM * 256)
static uint8_t sSrc[256 * 256 * 4 + MAX_TEX_DIM * 64];
static uint8_t sPalette[256 * 2];
static uint32_t sDecoded[MAX_DECODED_TEXELS];
#define MAX_TEX2_DIM 256 /* second sources of product textures */
static uint32_t sDecoded2[MAX_TEX2_DIM * MAX_TEX2_DIM];

/*
 * Bakes of scrolling texture pairs (the village's water) change every frame
 * until every scroll position has been baked once and cached. New bakes get
 * BAKE_BUDGET_US per frame; past that, a pair reuses its latest bake (one or
 * a few frames behind) instead of stalling the frame.
 */
#define BAKE_BUDGET_US 4000
#define BAKE_FAMILIES 64
static uint32_t sBakeUsFrame = 0, sBakesDeferred = 0;
static int16_t sFamily[BAKE_FAMILIES]; /* latest entry of a pair, whatever its scroll offset */

static uint32_t key_hash(const TexKey* k);
static bool key_equal(const TexKey* a, const TexKey* b);

static uint32_t family_slot(const TexKey* k) {
    TexKey x = *k;
    x.off_x = x.off_y = 0.0f;
    return key_hash(&x) % BAKE_FAMILIES;
}

static bool same_family(const TexKey* a, const TexKey* b) {
    TexKey x = *a, y = *b;
    x.off_x = x.off_y = y.off_x = y.off_y = 0.0f;
    return key_equal(&x, &y);
}

void gfx_tex_init(void) {
    memset(sEntries, 0, sizeof(sEntries));
    memset(sSlots, 0xFF, sizeof(sSlots));
    memset(sFamily, 0xFF, sizeof(sFamily));
    /* A roomier machine keeps more textures decoded (see rt_free_memory). */
    uint32_t free_mem = rt_free_memory();
    sBudget = free_mem / 4;
    if (sBudget < CACHE_BUDGET_MIN) {
        sBudget = CACHE_BUDGET_MIN;
    }
    if (sBudget > CACHE_BUDGET_MAX) {
        sBudget = CACHE_BUDGET_MAX;
    }
    rt_log("texture cache: %u KB", (unsigned)(sBudget / 1024));
}

void gfx_tex_new_frame(void) {
    sFrame++;
    sBakeUsFrame = 0;
}

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

static uint32_t key_hash(const TexKey* k) {
    uint32_t h = k->addr_bits * 2654435761u;
    h ^= k->tlut_addr * 2246822519u;
    h ^= k->row_bits * 3266489917u;
    h ^= (uint32_t)k->tile_w << 20 ^ (uint32_t)k->tile_h << 8;
    h ^= (uint32_t)k->fmt << 28 ^ (uint32_t)k->siz << 26 ^ (uint32_t)k->variant << 24;
    h ^= (uint32_t)k->cms << 22 ^ (uint32_t)k->cmt << 16 ^ (uint32_t)k->tlut_type << 30;
    h ^= (uint32_t)k->masks << 12 ^ (uint32_t)k->maskt << 4 ^ (uint32_t)k->row_swap << 1;
    if (k->variant & TEXVAR_TWO) {
        h ^= k->src2.addr_bits * 2246822519u ^ k->src2.tlut_addr;
    }
    if (k->variant & TEXVAR_COMBINE2) {
        uint32_t fx, fy, rx, ry;
        memcpy(&fx, &k->off_x, 4);
        memcpy(&fy, &k->off_y, 4);
        memcpy(&rx, &k->ratio_x, 4);
        memcpy(&ry, &k->ratio_y, 4);
        h ^= fx * 2654435761u ^ fy * 374761393u ^ rx * 2246822519u ^ ry * 3266489917u;
        uint32_t lod;
        memcpy(&lod, &k->comb_lod, 4);
        h ^= k->comb0 * 668265263u ^ k->comb1 * 3266489917u ^ k->comb_prim ^ k->comb_env ^ lod ^
             (uint32_t)k->bake_kind << 29;
    }
    if (k->variant & TEXVAR_LERP) {
        h ^= k->lerp_lo * 2654435761u ^ k->lerp_hi * 668265263u;
    }
    /* Final mix: textures are often a multiple of 0x400 bytes apart, and without
     * it their hashes agreed in the low bits the slot indices are taken from. */
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h;
}

static bool key_equal(const TexKey* a, const TexKey* b) {
    return a->addr_bits == b->addr_bits && a->row_bits == b->row_bits && a->tlut_addr == b->tlut_addr &&
           a->tile_w == b->tile_w && a->tile_h == b->tile_h && a->fmt == b->fmt && a->siz == b->siz &&
           a->tlut_type == b->tlut_type && a->cms == b->cms && a->cmt == b->cmt && a->masks == b->masks &&
           a->maskt == b->maskt && a->variant == b->variant && a->row_swap == b->row_swap &&
           (!(a->variant & TEXVAR_TWO) || memcmp(&a->src2, &b->src2, sizeof(a->src2)) == 0) &&
           (!(a->variant & TEXVAR_COMBINE2) ||
            (a->off_x == b->off_x && a->off_y == b->off_y && a->ratio_x == b->ratio_x &&
             a->ratio_y == b->ratio_y && a->base_second == b->base_second &&
             a->comb0 == b->comb0 && a->comb1 == b->comb1 && a->comb_prim == b->comb_prim &&
             a->comb_env == b->comb_env && a->comb_lod == b->comb_lod && a->bake_kind == b->bake_kind)) &&
           (!(a->variant & TEXVAR_LERP) || (a->lerp_lo == b->lerp_lo && a->lerp_hi == b->lerp_hi));
}

/* The second source of a product texture as a key of its own. */
static TexKey second_key(const TexKey* k) {
    TexKey k2 = *k;
    k2.addr_bits = k->src2.addr_bits;
    k2.row_bits = k->src2.row_bits;
    k2.tlut_addr = k->src2.tlut_addr;
    k2.fmt = k->src2.fmt;
    k2.siz = k->src2.siz;
    k2.tlut_type = k->src2.tlut_type;
    k2.cms = k->src2.cms;
    k2.cmt = k->src2.cmt;
    k2.masks = k->src2.masks;
    k2.maskt = k->src2.maskt;
    k2.row_swap = k->src2.row_swap;
    if (k->src2.tile_w != 0) {
        k2.tile_w = k->src2.tile_w;
        k2.tile_h = k->src2.tile_h;
    }
    k2.variant = 0;
    return k2;
}

/*
 * One axis of the RDP's texture coordinate handling: coordinates are clamped
 * to the tile when clamping is on (explicitly, or implicitly with no mask),
 * then wrapped by the mask, mirrored on odd periods if requested.
 */
typedef struct {
    bool clamp;
    uint8_t mask;       /* 0..10 */
    bool mirror;
    uint32_t size;      /* baked image size along this axis */
    uint32_t src_max;   /* largest source texel index sampled */
} Axis;

static Axis make_axis(uint32_t tile_len, uint8_t cm, uint8_t mask) {
    Axis a;
    a.mask = mask > 10 ? 10 : mask;
    a.mirror = (cm & G_TX_MIRROR) != 0 && a.mask != 0;
    a.clamp = (cm & G_TX_CLAMP) != 0 || a.mask == 0;
    if (a.clamp) {
        a.size = tile_len;
    } else {
        a.size = (1u << a.mask) << (a.mirror ? 1 : 0);
    }
    uint32_t period = a.mask ? (1u << a.mask) : a.size;
    a.src_max = (a.size < period ? a.size : period) - 1;
    return a;
}

static inline uint32_t axis_map(const Axis* a, uint32_t x) {
    if (a->mask != 0) {
        if (a->mirror && ((x >> a->mask) & 1)) {
            x = ~x;
        }
        x &= (1u << a->mask) - 1;
    }
    return x;
}

/* Source bytes the texture reads, from the byte containing texel (0,0). */
static uint32_t texel_bytes(const TexKey* k) {
    Axis ax = make_axis(k->tile_w, k->cms, k->masks);
    Axis ay = make_axis(k->tile_h, k->cmt, k->maskt);
    uint64_t last_bit = (uint64_t)(k->addr_bits & 7) + (uint64_t)ay.src_max * k->row_bits +
                        (uint64_t)(ax.src_max + 1) * kTexelBits[k->siz & 3];
    uint64_t bytes = ((last_bit + 7) >> 3) + 8; /* swapped reads reach past the last texel's unit half */
    return bytes > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)bytes;
}

static uint32_t palette_entries(const TexKey* k) {
    if (k->fmt != G_IM_FMT_CI) {
        return 0;
    }
    return k->siz == G_IM_SIZ_4b ? 16 : 256;
}

/* Loads a key's source texels and palette into sSrc / sPalette. */
static void read_source(const TexKey* k) {
    rt_copy_from_rdram(k->addr_bits >> 3, sSrc, texel_bytes(k));
    uint32_t pal = palette_entries(k);
    if (pal != 0) {
        rt_copy_from_rdram(k->tlut_addr, sPalette, pal * 2);
    }
}

/* Hash of RDRAM [addr, addr + len), read in place a word at a time. */
static uint32_t hash_rdram(uint32_t addr, uint32_t len) {
    uint32_t start = addr & ~3u;
    uint32_t words = (addr + len - start + 3) >> 2;
    if (start >= RDRAM_SIZE) {
        return 0;
    }
    if (start + words * 4 > RDRAM_SIZE) {
        words = (RDRAM_SIZE - start) >> 2;
    }
    const uint32_t* w = (const uint32_t*)(void*)(g_rdram + start);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < words; i++) {
        h = ((h << 5) | (h >> 27)) ^ w[i];
        h *= 0x9E3779B1u;
    }
    return h;
}

static uint32_t texel_hash(const TexKey* k) {
    uint32_t h = hash_rdram((k->addr_bits >> 3) & RDRAM_MASK, texel_bytes(k));
    if (k->variant & TEXVAR_TWO) {
        TexKey k2 = second_key(k);
        h = h * 31 ^ hash_rdram((k2.addr_bits >> 3) & RDRAM_MASK, texel_bytes(&k2));
    }
    return h;
}

static uint32_t pal_hash(const TexKey* k) {
    uint32_t h = 0;
    uint32_t pal = palette_entries(k);
    if (pal != 0) {
        h = hash_rdram(k->tlut_addr & RDRAM_MASK, pal * 2);
    }
    if (k->variant & TEXVAR_TWO) {
        TexKey k2 = second_key(k);
        pal = palette_entries(&k2);
        if (pal != 0) {
            h = h * 31 ^ hash_rdram(k2.tlut_addr & RDRAM_MASK, pal * 2);
        }
    }
    return h;
}

static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline uint32_t from_rgba16(uint16_t v) {
    uint32_t r = (v >> 11) & 31;
    uint32_t g = (v >> 6) & 31;
    uint32_t b = (v >> 1) & 31;
    return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (v & 1) ? 255 : 0);
}

static inline uint32_t from_ia16(uint16_t v) {
    uint32_t i = v >> 8;
    return rgba(i, i, i, v & 0xFF);
}

static uint32_t palette_color(const TexKey* k, uint32_t index) {
    uint16_t v = (uint16_t)((sPalette[index * 2] << 8) | sPalette[index * 2 + 1]);
    return k->tlut_type == TLUT_IA16 ? from_ia16(v) : from_rgba16(v);
}

/* Byte i of a row, where the row starts on a texture memory unit boundary. */
static inline uint32_t row_byte(uint32_t row_byte0, uint32_t i, bool swap) {
    return row_byte0 + (swap ? (i ^ 4) : i);
}

static uint32_t decode_texel(const TexKey* k, uint32_t mode, uint32_t row_bit, uint32_t x_bits, bool swap) {
    const uint8_t* s = sSrc;
    uint32_t r0 = row_bit >> 3;
    uint32_t xb = x_bits >> 3;
    bool odd_nibble = ((row_bit + x_bits) & 4) != 0;
    #define B(n) s[row_byte(r0, xb + (n), swap)]
    switch (mode) {
        case (G_IM_FMT_RGBA << 4) | G_IM_SIZ_16b:
            return from_rgba16((uint16_t)((B(0) << 8) | B(1)));
        case (G_IM_FMT_RGBA << 4) | G_IM_SIZ_32b:
            return rgba(B(0), B(1), B(2), B(3));
        case (G_IM_FMT_IA << 4) | G_IM_SIZ_4b: {
            uint32_t p = (B(0) >> (odd_nibble ? 0 : 4)) & 0xF;
            uint32_t v = ((p >> 1) * 255) / 7;
            return rgba(v, v, v, (p & 1) ? 255 : 0);
        }
        case (G_IM_FMT_IA << 4) | G_IM_SIZ_8b: {
            uint32_t b = B(0);
            uint32_t v = (b >> 4) * 17;
            return rgba(v, v, v, (b & 0xF) * 17);
        }
        case (G_IM_FMT_IA << 4) | G_IM_SIZ_16b:
            return rgba(B(0), B(0), B(0), B(1));
        case (G_IM_FMT_I << 4) | G_IM_SIZ_4b: {
            uint32_t v = ((B(0) >> (odd_nibble ? 0 : 4)) & 0xF) * 17;
            return rgba(v, v, v, v);
        }
        case (G_IM_FMT_I << 4) | G_IM_SIZ_8b:
            return rgba(B(0), B(0), B(0), B(0));
        case (G_IM_FMT_CI << 4) | G_IM_SIZ_4b:
            return palette_color(k, (B(0) >> (odd_nibble ? 0 : 4)) & 0xF);
        case (G_IM_FMT_CI << 4) | G_IM_SIZ_8b:
            return palette_color(k, B(0));
        default:
            RT_LOG_ONCE("unsupported texture format fmt=%u siz=%u", k->fmt, k->siz);
            return rgba(255, 0, 255, 255);
    }
    #undef B
}

/* TEXVAR_BLEED: each transparent texel gets the average colour of its opaque
 * neighbours (clamped at the edges). Only transparent texels change, and only
 * opaque ones are read, so it works in place. */
static void bleed_colour(uint32_t* px, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            if ((px[y * w + x] >> 24) != 0) {
                continue;
            }
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = (int)y + dy;
                if (yy < 0 || yy >= (int)h) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = (int)x + dx;
                    if (xx < 0 || xx >= (int)w) continue;
                    uint32_t c = px[yy * w + xx];
                    if ((c >> 24) == 0) continue;
                    r += c & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += (c >> 16) & 0xFF;
                    n++;
                }
            }
            if (n != 0) {
                px[y * w + x] = (r / n) | ((g / n) << 8) | ((b / n) << 16);
            }
        }
    }
}

/* Decodes the loaded source texels into out (src_max+1 wide/high). */
static void decode_into(const TexKey* k, const Axis* ax, const Axis* ay, uint32_t* out_buf) {
    uint32_t w = ax->src_max + 1;
    uint32_t h = ay->src_max + 1;
    uint32_t mode = (k->fmt << 4) | k->siz;
    uint32_t bits = kTexelBits[k->siz & 3];
    uint32_t bit0 = k->addr_bits & 7;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = bit0 + y * k->row_bits;
        bool swap = (k->row_swap >> (y & 1)) & 1;
        uint32_t* out = &out_buf[y * w];
        for (uint32_t x = 0; x < w; x++) {
            out[x] = decode_texel(k, mode, row, x * bits, swap);
        }
    }
}

/*
 * Index map for the second source of a baked pair: its texel for grid column x
 * is x * ratio + off, which covers both a scrolling origin and the two tiles
 * sampling the same coordinates at different rates. Point-sampled, so a source
 * finer than the grid is decimated.
 */
static void layout_index_map(const Axis* a, const Axis* base, uint32_t n, float ratio, float off,
                             uint32_t* out) {
    for (uint32_t x = 0; x < n; x++) {
        uint32_t gx = x < base->size ? x : base->size - 1;
        float t = (float)gx * ratio + off;
        int32_t bx = (int32_t)floorf(t + 0.5f);
        if (a->clamp) {
            if (bx < 0) {
                bx = 0;
            }
            if ((uint32_t)bx >= a->size) {
                bx = (int32_t)a->size - 1;
            }
        }
        uint32_t sx = axis_map(a, (uint32_t)bx);
        out[x] = sx <= a->src_max ? sx : a->src_max;
    }
}

static void layout_index_off(const Axis* a, const Axis* base, uint32_t n, int32_t off, uint32_t* out) {
    for (uint32_t x = 0; x < n; x++) {
        /* past a clamped axis' size its last texel repeats; a wrapped one goes round again (min_gu_width) */
        int32_t bx = (int32_t)(x < base->size || !base->clamp ? x : base->size - 1) + off;
        if (a->clamp) {
            if (bx < 0) {
                bx = 0;
            }
            if ((uint32_t)bx >= a->size) {
                bx = (int32_t)a->size - 1;
            }
        }
        uint32_t sx = axis_map(a, (uint32_t)bx);
        out[x] = sx <= a->src_max ? sx : a->src_max;
    }
}

/* Where each GU texel's column (or row) comes from, the way the RDP samples the
 * tile; past the tile's size the last one repeats on a clamped axis (which
 * never samples that padding), and a wrapped axis repeats as a whole. */
static void layout_index(const Axis* a, const Axis* base, uint32_t n, uint32_t* out) {
    layout_index_off(a, base, n, 0, out);
}

static inline uint32_t mul_rgba(uint32_t p, uint32_t q) {
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t v = ((p >> sh) & 0xFF) * ((q >> sh) & 0xFF);
        r |= ((v + 127) / 255) << sh;
    }
    return r;
}

/* Average and spread per channel, from the sums taken while building;
 * n: how many texels the sums cover (0: all of them). */
static void measure(GuTexture* t, const uint64_t* sum, const uint64_t* sq, uint32_t n) {
    if (n == 0) {
        n = (uint32_t)t->gu_width * t->gu_height;
    }
    t->dev = 0;
    for (int ch = 0; ch < 4; ch++) {
        float mean = (float)sum[ch] / (float)n;
        float var = (float)sq[ch] / (float)n - mean * mean;
        float dev = var > 0 ? sqrtf(var) / 255.0f : 0;
        t->mean[ch] = mean / 255.0f;
        if (dev > t->dev) {
            t->dev = dev;
        }
    }
}

/*
 * The GE reads half as much for a 16-bit texture, which matters in scenes that
 * cover the screen. A format is only used where it loses nothing: the N64's own
 * RGBA16 texels expand to 8 bits reversibly (see from_rgba16), as do IA8 and I4,
 * whose channels are a nibble times 17. Everything else keeps 8888.
 */
static int pick_psm(const TexKey* k) {
    if (k->variant & (TEXVAR_TWO | TEXVAR_LERP)) {
        return GU_PSM_8888;  /* a combination of two texels (or of texel and colours) is not a source texel */
    }
    switch ((k->fmt << 4) | k->siz) {
        case (G_IM_FMT_RGBA << 4) | G_IM_SIZ_16b:
            return GU_PSM_5551;
        case (G_IM_FMT_CI << 4) | G_IM_SIZ_4b:
        case (G_IM_FMT_CI << 4) | G_IM_SIZ_8b:
            return k->tlut_type == TLUT_IA16 ? GU_PSM_8888 : GU_PSM_5551;
        case (G_IM_FMT_IA << 4) | G_IM_SIZ_8b:
        case (G_IM_FMT_I << 4) | G_IM_SIZ_4b:
            return GU_PSM_4444;
        default:
            return GU_PSM_8888;
    }
}

static uint32_t psm_bytes(int psm) {
    return psm == GU_PSM_8888 ? 4u : 2u;
}

/*
 * The GE fetches a texture's rows in units of 16 bytes. A narrower texture --
 * the 4x4 colour swatches of the pocket menu's item models, 8 bytes a row as
 * 5551 -- is read wrongly on hardware: the rows it shows come from further on,
 * past the end of the buffer, so the picture depended on what the heap had put
 * there. (PPSSPP reads such a texture as it is laid out.) So no texture is
 * narrower than this; the columns added repeat the tile the way its axis does.
 */
static uint32_t min_gu_width(int psm) {
    return 16 / psm_bytes(psm);
}

static inline uint16_t to_5551(uint32_t p) {
    return (uint16_t)(((p >> 3) & 0x1F) | ((p >> 6) & 0x3E0) | ((p >> 9) & 0x7C00) | ((p >> 16) & 0x8000));
}

static inline uint16_t to_4444(uint32_t p) {
    return (uint16_t)(((p >> 4) & 0x0F) | ((p >> 8) & 0xF0) | ((p >> 12) & 0xF00) | ((p >> 16) & 0xF000));
}

/*
 * TEXVAR_COMBINE2 bakes two tiles that the N64 samples at different rates --
 * the K.K. spotlight's dust is read at twice the cone's rate (a tile shift of
 * 15 doubles its coordinates). Laid out on the coarser tile's grid, as this
 * was at first, the finer one was point-sampled at every other texel and then
 * magnified with the rest: half of its detail was thrown away and the other
 * half blurred, and the dust all but vanished from the beam. So the bake is
 * laid out at the finer tile's rate instead: `sub` GU texels per base texel,
 * each finer texel taken once, the coarser tile interpolated between its
 * texels the way the RDP's bilinear filter would. Rates are rounded to 2 or 4;
 * a coarser second tile needs nothing extra (sub 1).
 */
static uint32_t bake_sub(float ratio, uint32_t gu_size) {
    float r = ratio < 0 ? -ratio : ratio;
    uint32_t sub = r >= 3.0f ? 4 : r >= 1.5f ? 2 : 1;
    while (sub > 1 && gu_size * sub > MAX_TEX_DIM) {
        sub /= 2;
    }
    return sub;
}

/* One fine-grid axis of a COMBINE2 bake: for each GU texel, the two base
 * texels to interpolate (and the weight of the second, 0..256), and the texel
 * of the other tile. */
static void bake_axis(const Axis* a, const Axis* a2, uint32_t n_base, uint32_t sub, float ratio, float off,
                      bool clamp, uint32_t* b0, uint32_t* b1, uint32_t* w, uint32_t* o) {
    uint32_t cols[MAX_TEX_DIM];
    layout_index(a, a, n_base, cols);
    for (uint32_t x = 0; x < n_base * sub; x++) {
        float cb = ((float)x + 0.5f) / (float)sub - 0.5f;   /* base texel-centre coordinate */
        int32_t g0 = (int32_t)floorf(cb);
        float f = cb - (float)g0;
        int32_t g1 = g0 + 1;
        if (clamp) {
            g0 = g0 < 0 ? 0 : g0 >= (int32_t)n_base ? (int32_t)n_base - 1 : g0;
            g1 = g1 < 0 ? 0 : g1 >= (int32_t)n_base ? (int32_t)n_base - 1 : g1;
        } else {
            g0 = ((g0 % (int32_t)n_base) + (int32_t)n_base) % (int32_t)n_base;
            g1 = ((g1 % (int32_t)n_base) + (int32_t)n_base) % (int32_t)n_base;
        }
        b0[x] = cols[g0];
        b1[x] = cols[g1];
        w[x] = (uint32_t)(f * 256.0f + 0.5f);
        /* the other tile, as layout_index_map places it, at this finer position */
        float gx = cb < (float)(a->size - 1) ? cb : (float)(a->size - 1);
        int32_t bx = (int32_t)floorf(gx * ratio + off + 0.5f);
        if (a2->clamp) {
            bx = bx < 0 ? 0 : (uint32_t)bx >= a2->size ? (int32_t)a2->size - 1 : bx;
        }
        uint32_t sx = axis_map(a2, (uint32_t)bx);
        o[x] = sx <= a2->src_max ? sx : a2->src_max;
    }
}

/* Per channel (p * (256 - w) + q * w + 128) >> 8, two channels per multiply. */
static inline uint32_t lerp_rgba(uint32_t p, uint32_t q, uint32_t w) {
    uint32_t iw = 256 - w;
    uint32_t rb = ((p & 0x00FF00FFu) * iw + (q & 0x00FF00FFu) * w + 0x00800080u) >> 8;
    uint32_t ga = ((p >> 8) & 0x00FF00FFu) * iw + ((q >> 8) & 0x00FF00FFu) * w + 0x00800080u;
    return (rb & 0x00FF00FFu) | (ga & 0xFF00FF00u);
}

static void build_combine2_fine(const TexKey* k, const Axis* ax, const Axis* ay, const Axis* ax2, const Axis* ay2,
                                GuTexture* t) {
    uint32_t gw = t->gu_width, gh = t->gu_height;
    uint32_t sx = t->sub_x, sy = t->sub_y;
    static uint32_t cb0[MAX_TEX_DIM], cb1[MAX_TEX_DIM], cw[MAX_TEX_DIM], co[MAX_TEX_DIM];
    static uint32_t rb0[MAX_TEX_DIM], rb1[MAX_TEX_DIM], rw[MAX_TEX_DIM], ro[MAX_TEX_DIM];
    bake_axis(ax, ax2, gw / sx, sx, k->ratio_x, k->off_x, t->clamp_s, cb0, cb1, cw, co);
    bake_axis(ay, ay2, gh / sy, sy, k->ratio_y, k->off_y, t->clamp_t, rb0, rb1, rw, ro);
    uint32_t src_w = ax->src_max + 1, src2_w = ax2->src_max + 1;
    int rgb_from = gBakeRgbTile;
    bool base_second = k->base_second;
    uint32_t* dst = t->pixels;
    t->swizzled = (gw % 4 == 0) && (gh % 8 == 0);
    gfx_bake_alpha_prepare();
    uint8_t split_tag = k->bake_kind != BAKE_COLOUR ? gfx_bake_split_prepare(k->bake_kind) : 0;
    /* A bake is rebuilt whenever it scrolls; its statistics come from every 16th texel. */
    uint64_t sum[4] = { 0 }, sq[4] = { 0 };
    uint32_t stat_n = 0;
    static uint32_t line_px[MAX_TEX_DIM];
    for (uint32_t y = 0; y < gh; y++) {
        /* The base texture's two rows, blended once per row for every column. */
        const uint32_t* r0 = &sDecoded[rb0[y] * src_w];
        const uint32_t* r1 = &sDecoded[rb1[y] * src_w];
        uint32_t wy = rw[y];
        for (uint32_t c = 0; c < src_w; c++) {
            line_px[c] = wy == 0 ? r0[c] : lerp_rgba(r0[c], r1[c], wy);
        }
        const uint32_t* ro2 = &sDecoded2[ro[y] * src2_w];
        uint32_t line = t->swizzled ? (y / 8) * (gw * 8) + (y % 8) * 4 : y * gw;
        uint32_t sum32[4] = { 0 }, sq32[4] = { 0 };
        for (uint32_t x = 0; x < gw; x++) {
            uint32_t wx = cw[x];
            uint32_t base = wx == 0 ? line_px[cb0[x]] : lerp_rgba(line_px[cb0[x]], line_px[cb1[x]], wx);
            uint32_t other = ro2[co[x]];
            uint32_t t0 = base_second ? other : base;
            uint32_t t1 = base_second ? base : other;
            uint32_t texel = split_tag
                                 ? gfx_bake_split_lookup(t0, t1, split_tag)
                                 : ((rgb_from == 0 ? t0 : t1) & 0x00FFFFFFu) |
                                       ((uint32_t)gfx_bake_alpha_lookup((uint8_t)(t0 >> 24), (uint8_t)(t1 >> 24)) << 24);
            if (((x | y) & 3) == 0) {
                stat_n++;
                for (int ch = 0; ch < 4; ch++) {
                    uint32_t v = (texel >> (8 * ch)) & 0xFF;
                    sum32[ch] += v;
                    sq32[ch] += v * v;
                }
            }
            uint32_t at = t->swizzled ? line + (x / 4) * 32 + (x % 4) : line + x;
            dst[at] = texel;
        }
        for (int ch = 0; ch < 4; ch++) {
            sum[ch] += sum32[ch];
            sq[ch] += sq32[ch];
        }
    }
    measure(t, sum, sq, stat_n);
    sceKernelDcacheWritebackRange(t->pixels, (SceSize)gw * gh * 4);
}

/* Time spent decoding and baking, for the stats line (gfx_tex_take_build_stats). */
static uint32_t sBuildUs = 0, sBuilds = 0, sBakes = 0;

uint32_t gfx_tex_take_deferred(void) {
    uint32_t n = sBakesDeferred;
    sBakesDeferred = 0;
    return n;
}

void gfx_tex_take_build_stats(uint32_t* builds, uint32_t* bakes, uint32_t* us) {
    *builds = sBuilds;
    *bakes = sBakes;
    *us = sBuildUs;
    sBuilds = sBakes = sBuildUs = 0;
}

static void build_impl(const TexKey* k, const Axis* ax, const Axis* ay, GuTexture* t);

/* TEXVAR_LERP: each channel c becomes lo + (hi - lo) * c / 255, alpha kept. */
static inline uint32_t lerp_rgb(uint32_t texel, uint32_t lo, uint32_t hi) {
    uint32_t out = texel & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int l = (int)((lo >> sh) & 0xFF), h = (int)((hi >> sh) & 0xFF), c = (int)((texel >> sh) & 0xFF);
        out |= (uint32_t)(l + ((h - l) * c + 127) / 255) << sh;
    }
    return out;
}

static void build(const TexKey* k, const Axis* ax, const Axis* ay, GuTexture* t) {
    uint32_t t0 = sceKernelGetSystemTimeLow();
    t->alpha_binary = false; /* build_impl finds out, where it can tell cheaply */
    build_impl(k, ax, ay, t);
    uint32_t dt = sceKernelGetSystemTimeLow() - t0;
    sBuildUs += dt;
    if (k->variant & TEXVAR_COMBINE2) {
        sBakeUsFrame += dt;
    }
    sBuilds++;
    sBakes += (k->variant & TEXVAR_TWO) != 0;
}

/* Decodes a key's sources and lays them out into t. */
static void build_impl(const TexKey* k, const Axis* ax, const Axis* ay, GuTexture* t) {
    Axis ax2, ay2;
    bool product = (k->variant & TEXVAR_PRODUCT) != 0;
    bool combine2 = (k->variant & TEXVAR_COMBINE2) != 0;
    bool two = product || combine2;
    if (two) {
        TexKey k2 = second_key(k);
        ax2 = make_axis(k2.tile_w, k2.cms, k2.masks);
        ay2 = make_axis(k2.tile_h, k2.cmt, k2.maskt);
        read_source(&k2);
        decode_into(&k2, &ax2, &ay2, sDecoded2);
    }
    read_source(k);
    decode_into(k, ax, ay, sDecoded);
    if ((k->variant & TEXVAR_BLEED) && !two) {
        bleed_colour(sDecoded, ax->src_max + 1, ay->src_max + 1);
    }
    if (combine2 && (t->sub_x > 1 || t->sub_y > 1)) {
        build_combine2_fine(k, ax, ay, &ax2, &ay2, t);
        return;
    }

    uint32_t src_w = ax->src_max + 1;
    uint32_t gw = t->gu_width;
    uint32_t gh = t->gu_height;
    uint32_t* dst = t->pixels;
    uint32_t cols[MAX_TEX_DIM], rows[MAX_TEX_DIM];
    uint32_t cols2[MAX_TEX_DIM], rows2[MAX_TEX_DIM];
    layout_index(ax, ax, gw, cols);
    layout_index(ay, ay, gh, rows);
    if (combine2) {
        layout_index_map(&ax2, ax, gw, k->ratio_x, k->off_x, cols2);
        layout_index_map(&ay2, ay, gh, k->ratio_y, k->off_y, rows2);
    } else if (two) {
        layout_index_off(&ax2, ax, gw, 0, cols2);
        layout_index_off(&ay2, ay, gh, 0, rows2);
    }
    uint32_t white = (k->variant & TEXVAR_WHITE_RGB) ? 0x00FFFFFF : 0;
    bool lerp = (k->variant & TEXVAR_LERP) != 0;
    /*
     * The GE reads textures fastest in its own block order ("swizzled"):
     * blocks 16 bytes wide and 8 rows tall, one after another. Writing the
     * texels straight into that order costs nothing here and saves the
     * hardware a lot of texture cache misses on large surfaces. Sizes that
     * don't fill whole blocks stay linear.
     */
    uint32_t texels_per_block = 16 / psm_bytes(t->psm);
    t->swizzled = (gw % texels_per_block == 0) && (gh % 8 == 0);
    uint16_t* dst16 = (uint16_t*)dst;
    uint64_t sum[4] = { 0 }, sq[4] = { 0 };
    uint8_t split_tag = combine2 && k->bake_kind != BAKE_COLOUR ? gfx_bake_split_prepare(k->bake_kind) : 0;
    int rgb_from = 0;
    if (combine2 && !split_tag) {
        rgb_from = gBakeRgbTile;
        gfx_bake_alpha_prepare();
    }
    /* A bake is rebuilt whenever it scrolls; its statistics come from every 16th texel.
     * Columns added to reach the GE's smallest width (min_gu_width) are left out. */
    uint32_t stat_mask = two ? 3 : 0, stat_n = 0;
    uint32_t stat_w = next_pow2(ax->size) * (t->sub_x ? t->sub_x : 1);
    uint32_t partial_alpha = 0;
    for (uint32_t y = 0; y < gh; y++) {
        const uint32_t* row = &sDecoded[rows[y] * src_w];
        const uint32_t* row2 = two ? &sDecoded2[rows2[y] * (ax2.src_max + 1)] : NULL;
        uint32_t line = t->swizzled ? (y / 8) * (gw * 8) + (y % 8) * texels_per_block : y * gw;
        for (uint32_t x = 0; x < gw; x++) {
            uint32_t texel;
            if (combine2) {
                uint32_t base = row[cols[x]], other = row2[cols2[x]];
                uint32_t t0 = k->base_second ? other : base, t1 = k->base_second ? base : other;
                /* Same as gfx_combiner.c's bake_texel, with the alpha memoised: evaluating the
                 * combiner per texel made the name-entry window's scrolling
                 * background cost 13-51 ms per bake. */
                texel = split_tag ? gfx_bake_split_lookup(t0, t1, split_tag)
                                  : ((rgb_from == 0 ? t0 : t1) & 0x00FFFFFFu) |
                                        ((uint32_t)gfx_bake_alpha_lookup((uint8_t)(t0 >> 24), (uint8_t)(t1 >> 24))
                                         << 24);
            } else if (product) {
                texel = mul_rgba(row[cols[x]], row2[cols2[x]]);
            } else {
                texel = row[cols[x]];
            }
            texel |= white;
            if (lerp) {
                texel = lerp_rgb(texel, k->lerp_lo, k->lerp_hi);
            }
            partial_alpha |= ((texel >> 24) + 1) & 0xFE; /* nonzero unless the alpha is 0 or 255 */
            if (((x | y) & stat_mask) == 0 && x < stat_w) {
                stat_n++;
                for (int ch = 0; ch < 4; ch++) {
                    uint32_t v = (texel >> (8 * ch)) & 0xFF;
                    sum[ch] += v;
                    sq[ch] += v * v;
                }
            }
            uint32_t at = t->swizzled ? line + (x / texels_per_block) * texels_per_block * 8 + (x % texels_per_block)
                                      : line + x;
            if (t->psm == GU_PSM_8888) {
                dst[at] = texel;
            } else if (t->psm == GU_PSM_5551) {
                dst16[at] = to_5551(texel);
            } else {
                dst16[at] = to_4444(texel);
            }
        }
    }
    measure(t, sum, sq, stat_n);
    t->alpha_binary = partial_alpha == 0;
    /* The GE reads textures from physical memory. */
    sceKernelDcacheWritebackRange(t->pixels, (SceSize)gw * gh * psm_bytes(t->psm));
}

/*
 * The GE reads a texture when it executes the draw, which can be long after we
 * queued it -- up to the frame's sync. So a texture is never rewritten or freed
 * in place: a rebuild gets a fresh buffer and the old one is retired, to be
 * freed after a sync that happened after it was retired (hence two lists).
 * Rewriting one under the GE shows up as flashing rubbish on hardware, where
 * the GE really does run behind; emulators hide it.
 */
#define MAX_RETIRED 256
static void* sRetired[2][MAX_RETIRED];
static int sNumRetired[2];

static void retire(void* pixels) {
    if (pixels == NULL) {
        return;
    }
    if (sNumRetired[1] < MAX_RETIRED) {
        sRetired[1][sNumRetired[1]++] = pixels;
    } else {
        free(pixels); /* nothing sensible left to do; a sync is overdue */
    }
}

/* Called after the GE has finished the frame (see rt_gfx_present). */
void gfx_tex_flush_retired(void) {
    for (int i = 0; i < sNumRetired[0]; i++) {
        free(sRetired[0][i]);
    }
    memcpy(sRetired[0], sRetired[1], (size_t)sNumRetired[1] * sizeof(void*));
    sNumRetired[0] = sNumRetired[1];
    sNumRetired[1] = 0;
}

static void free_entry(CacheEntry* e) {
    if (e->tex.pixels != NULL) {
        retire(e->tex.pixels);
        e->tex.pixels = NULL;
    }
    sBytesUsed -= e->bytes;
    e->used = false;
    sSpans[e - sEntries] = (SourceSpan){ 0, 0, 0, 0 };
}

static void rebuild_slots(void) {
    memset(sSlots, 0xFF, sizeof(sSlots));
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!sEntries[i].used) {
            continue;
        }
        uint32_t slot = key_hash(&sEntries[i].key) & (CACHE_SLOTS - 1);
        while (sSlots[slot] >= 0) {
            slot = (slot + 1) & (CACHE_SLOTS - 1);
        }
        sSlots[slot] = (int16_t)i;
    }
}

static void evict_until(uint32_t needed_bytes) {
    bool evicted = false;
    while (sBytesUsed + needed_bytes > sBudget) {
        int oldest = -1;
        for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
            if (sEntries[i].used && sEntries[i].last_used != sFrame &&
                (oldest < 0 || sEntries[i].last_used < sEntries[oldest].last_used)) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            break;
        }
        free_entry(&sEntries[oldest]);
        evicted = true;
    }
    if (evicted) {
        rebuild_slots();
    }
}

static int alloc_entry(void) {
    for (int i = 0; i < CACHE_MAX_ENTRIES; i++) {
        if (!sEntries[i].used) {
            return i;
        }
    }
    int oldest = 0;
    for (int i = 1; i < CACHE_MAX_ENTRIES; i++) {
        if (sEntries[i].last_used < sEntries[oldest].last_used) {
            oldest = i;
        }
    }
    free_entry(&sEntries[oldest]);
    rebuild_slots();
    return oldest;
}

/* Debug: with dump_tex.txt present, the first 400 new GU textures are written
 * to gutex_<source>_<w>x<h>_<n>.rgba (RGBA8888). */
static uint32_t sDumpCount = 0;

static void dump_texture(const TexKey* key, const GuTexture* t) {
    if (!RT_SWITCH("dump_tex.txt") || sDumpCount >= 400) {
        return;
    }
    char name[96];
    char path[256];
    snprintf(name, sizeof(name), "gutex_%08X_%ux%u_%03u.rgba", (unsigned)(key->addr_bits >> 3), t->gu_width,
             t->gu_height, (unsigned)sDumpCount++);
    SceUID fd = sceIoOpen(rt_data_path(name, path, sizeof(path)), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        /* decoded: textures may be 16-bit and swizzled */
        uint32_t* rgba = malloc((size_t)t->gu_width * t->gu_height * 4);
        if (rgba != NULL) {
            for (uint32_t y = 0; y < t->gu_height; y++) {
                for (uint32_t x = 0; x < t->gu_width; x++) {
                    rgba[y * t->gu_width + x] = gfx_tex_texel(t, x, y);
                }
            }
            sceIoWrite(fd, rgba, (SceSize)t->gu_width * t->gu_height * 4);
            free(rgba);
        }
        sceIoClose(fd);
    }
}

/*
 * One texel as RGBA8888, whatever the texture is actually stored as. The
 * combiner fitter reads edge texels to fold a clamped tile into a constant,
 * and it must see what the decoder produced, not the stored bits.
 */
uint32_t gfx_tex_texel(const GuTexture* t, uint32_t x, uint32_t y) {
    uint32_t gw = t->gu_width;
    uint32_t per_block = 16 / psm_bytes(t->psm);
    uint32_t at = t->swizzled ? (y / 8) * (gw * 8) + (y % 8) * per_block + (x / per_block) * per_block * 8 +
                                (x % per_block)
                              : y * gw + x;
    if (t->psm == GU_PSM_8888) {
        return ((const uint32_t*)t->pixels)[at];
    }
    uint32_t v = ((const uint16_t*)t->pixels)[at];
    if (t->psm == GU_PSM_5551) {
        uint32_t r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
        return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (v & 0x8000) ? 255 : 0);
    }
    return rgba((v & 0xF) * 17, ((v >> 4) & 0xF) * 17, ((v >> 8) & 0xF) * 17, ((v >> 12) & 0xF) * 17);
}

/*
 * A COMBINE2 bake on the tile's own grid (sub 1) with a whole-number ratio
 * places the other tile at floor(x * ratio + off + 0.5): only the rounded
 * offset changes the image. Keying on it makes a slow scroll (the name-entry
 * window's pattern moves a quarter texel per frame) reuse one bake for every
 * step that looks the same, instead of baking an identical copy each frame.
 */
static void round_bake_offsets(TexKey* k, const Axis* ax, const Axis* ay) {
    uint32_t gw = next_pow2(ax->size), gh = next_pow2(ay->size);
    if (gw > MAX_TEX_DIM) gw = MAX_TEX_DIM;
    if (gh > MAX_TEX_DIM) gh = MAX_TEX_DIM;
    if (bake_sub(k->ratio_x, gw) != 1 || bake_sub(k->ratio_y, gh) != 1 || k->ratio_x != floorf(k->ratio_x) ||
        k->ratio_y != floorf(k->ratio_y)) {
        return;
    }
    k->off_x = floorf(k->off_x + 0.5f);
    k->off_y = floorf(k->off_y + 0.5f);
}

/*
 * A COMBINE2 bake repeats with the base tile's period, but the other tile may
 * repeat more slowly: the pocket screen's window masks mirror (period 64)
 * over a 32-texel polka-dot tile. A bake one base period wide then holds half
 * the mask, and the GE's repeat shows it flipped on the other half -- opaque
 * dots along the window's outer edge where the N64 has none. So the axis is
 * widened to the longer period. Both are powers of two here (masks), so that
 * is a whole number of each, and the GE repeats at that size too.
 */
static void extend_bake_axis(Axis* a, const Axis* a2, float ratio) {
    float r = ratio < 0 ? -ratio : ratio;
    if (a->clamp || a2->clamp || r == 0.0f) {
        return;
    }
    float p2 = (float)a2->size / r; /* the other tile's period, in base texels */
    uint32_t n = (uint32_t)p2;
    if ((float)n != p2 || (n & (n - 1)) != 0 || (a->size & (a->size - 1)) != 0) {
        return;
    }
    if (n > a->size && n <= 256) {
        a->size = n;
    }
}

const GuTexture* gfx_tex_get(const TexKey* key_in) {
    if (key_in->tile_w == 0 || key_in->tile_h == 0) {
        return NULL;
    }
    Axis ax = make_axis(key_in->tile_w, key_in->cms, key_in->masks);
    Axis ay = make_axis(key_in->tile_h, key_in->cmt, key_in->maskt);
    TexKey rounded;
    const TexKey* key = key_in;
    if (key_in->variant & TEXVAR_COMBINE2) {
        TexKey k2 = second_key(key_in);
        Axis ax2 = make_axis(k2.tile_w, k2.cms, k2.masks);
        Axis ay2 = make_axis(k2.tile_h, k2.cmt, k2.maskt);
        extend_bake_axis(&ax, &ax2, key_in->ratio_x);
        extend_bake_axis(&ay, &ay2, key_in->ratio_y);
        rounded = *key_in;
        round_bake_offsets(&rounded, &ax, &ay);
        key = &rounded;
    }
    if (ax.src_max >= MAX_TEX_DIM || ay.src_max >= MAX_TEX_DIM || texel_bytes(key) > sizeof(sSrc) ||
        (ax.src_max + 1) * (ay.src_max + 1) > MAX_DECODED_TEXELS) {
        return NULL;
    }
    if (key->variant & TEXVAR_TWO) {
        TexKey k2 = second_key(key);
        Axis ax2 = make_axis(k2.tile_w, k2.cms, k2.masks);
        Axis ay2 = make_axis(k2.tile_h, k2.cmt, k2.maskt);
        if (ax2.src_max >= MAX_TEX2_DIM || ay2.src_max >= MAX_TEX2_DIM || texel_bytes(&k2) > sizeof(sSrc)) {
            return NULL;
        }
    }

    /* The RDRAM the texels (and a palette) are read from; a render target drawn there is copied back first. */
    uint32_t src_start = (key->addr_bits >> 3) & RDRAM_MASK, src_end = src_start + texel_bytes(key);
    uint32_t src2_start = 0, src2_end = 0;
    if (key->variant & TEXVAR_TWO) {
        TexKey k2 = second_key(key);
        src2_start = (k2.addr_bits >> 3) & RDRAM_MASK;
        src2_end = src2_start + texel_bytes(&k2);
    }
    if (gTarget.dirty) {
        gfx_target_need(src_start, src_end - src_start);
        gfx_target_need(src2_start, src2_end - src2_start);
        gfx_target_need(key->tlut_addr, 512);
        gfx_target_need(key->src2.tlut_addr, 512);
    }

    uint32_t slot = key_hash(key) & (CACHE_SLOTS - 1);
    while (sSlots[slot] >= 0) {
        CacheEntry* e = &sEntries[sSlots[slot]];
        if (key_equal(&e->key, key)) {
            e->last_used = sFrame;
            if (e->checked_frame != sFrame || e->stale) {
                e->checked_frame = sFrame;
                bool changed = false;
                uint32_t ph = pal_hash(key);
                if (ph != e->pal_hash) {
                    e->pal_hash = ph;
                    changed = true;
                }
                if (e->stale || ((sFrame + (uint32_t)sSlots[slot]) & (TEXEL_CHECK_PERIOD - 1)) == 0) {
                    e->stale = false;
                    uint32_t th = texel_hash(key);
                    if (th != e->texel_hash) {
                        e->texel_hash = th;
                        changed = true;
                    }
                }
                if (changed) {
                    /* Build into a new buffer: the GE may still be reading the old one. */
                    void* fresh = memalign(16, e->bytes);
                    if (fresh != NULL) {
                        retire(e->tex.pixels);
                        e->tex.pixels = fresh;
                        build(key, &ax, &ay, &e->tex);
                    }
                }
            }
            return &e->tex;
        }
        slot = (slot + 1) & (CACHE_SLOTS - 1);
    }

    uint32_t gw = next_pow2(ax.size);
    uint32_t gh = next_pow2(ay.size);
    if (gw > MAX_TEX_DIM) gw = MAX_TEX_DIM;
    if (gh > MAX_TEX_DIM) gh = MAX_TEX_DIM;
    uint32_t sub_x = 1, sub_y = 1;
    if (key->variant & TEXVAR_COMBINE2) {
        sub_x = bake_sub(key->ratio_x, gw);
        sub_y = bake_sub(key->ratio_y, gh);
        gw *= sub_x;
        gh *= sub_y;
    }
    int psm = pick_psm(key);
    if (gw < min_gu_width(psm) && !(key->variant & TEXVAR_TWO)) {
        gw = min_gu_width(psm); /* (not the two-texture bakes, which lay out a grid of their own) */
    }
    uint32_t bytes = gw * gh * psm_bytes(psm);

    if ((key->variant & TEXVAR_COMBINE2) && sBakeUsFrame >= BAKE_BUDGET_US) {
        int fi = sFamily[family_slot(key)];
        if (fi >= 0 && sEntries[fi].used && same_family(&sEntries[fi].key, key)) {
            sEntries[fi].last_used = sFrame;
            sBakesDeferred++;
            return &sEntries[fi].tex;
        }
    }

    evict_until(bytes);
    int index = alloc_entry();
    CacheEntry* e = &sEntries[index];
    memset(e, 0, sizeof(*e));
    e->tex.pixels = memalign(16, bytes);
    if (e->tex.pixels == NULL) {
        rt_log("texture cache: out of memory (%u bytes, %u used)", bytes, sBytesUsed);
        return NULL;
    }
    e->used = true;
    e->key = *key;
    e->bytes = bytes;
    sSpans[index] = (SourceSpan){ src_start, src_end, src2_start, src2_end };
    if (index >= sEntriesTop) {
        sEntriesTop = index + 1;
    }
    e->last_used = sFrame;
    e->checked_frame = sFrame;
    e->tex.psm = (uint8_t)psm;
    e->tex.gu_width = (uint16_t)gw;
    e->tex.gu_height = (uint16_t)gh;
    e->tex.sub_x = (uint8_t)sub_x;
    e->tex.sub_y = (uint8_t)sub_y;
    e->tex.clamp_s = ax.clamp;
    e->tex.clamp_t = ay.clamp;
    sBytesUsed += bytes;

    e->texel_hash = texel_hash(key);
    e->pal_hash = pal_hash(key);
    build(key, &ax, &ay, &e->tex);
    dump_texture(key, &e->tex);
    if (key->variant & TEXVAR_COMBINE2) {
        sFamily[family_slot(key)] = (int16_t)index;
    }

    slot = key_hash(key) & (CACHE_SLOTS - 1);
    while (sSlots[slot] >= 0) {
        slot = (slot + 1) & (CACHE_SLOTS - 1);
    }
    sSlots[slot] = (int16_t)index;
    return &e->tex;
}

/*
 * The renderer drew into RDRAM [addr, addr + len) (a render target copied
 * back): textures read from there must check their contents on next use, even
 * if they were checked earlier this frame -- AF draws several icons into one
 * buffer in a frame, using each as a texture before drawing the next.
 */
void gfx_tex_invalidate_range(uint32_t addr, uint32_t len) {
    addr &= RDRAM_MASK;
    uint32_t end = addr + len;
    for (int i = 0; i < sEntriesTop; i++) {
        const SourceSpan* span = &sSpans[i];
        if ((span->start < end && span->end > addr) || (span->start2 < end && span->end2 > addr)) {
            sEntries[i].stale = true;
        }
    }
}
