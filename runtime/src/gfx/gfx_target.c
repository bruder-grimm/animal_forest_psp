/*
 * gfx_target.c -- render targets: the game's own small pictures, drawn on the GE.
 *
 * AF draws a few things into small colour images of its own and then uses
 * them as textures: the player's portrait in the pocket screen (128x128, over
 * a copy of the screen behind it) and the item pictures beside it (32x32).
 *
 * Such images, up to TARGET_MAX square, are drawn on the GE at N64 resolution in
 * spare VRAM after the third colour buffer, with their own depth buffer. RDRAM
 * stays the reference: a target is loaded from it when drawing starts, and
 * what the GE drew is copied back in the N64's format, so that every later
 * use -- through the texture cache, in any format -- sees what the RDP would
 * have drawn. The GE's destination alpha is its stencil buffer, so the
 * stencil stands in for the 5551 alpha (coverage) bit: 1 wherever something
 * was drawn.
 *
 * A copy-back has to wait for the GE to finish everything queued, and the
 * pocket screen leaves its targets seven times a frame, which made it the one
 * screen far from 30 fps. On the N64 none of this costs anything: the RDP
 * draws into RDRAM and the next texture load reads the same memory. So the
 * copy is avoided wherever the same can be said here:
 *
 * - It is put off until something reads the picture from RDRAM
 *   (gfx_target_need: a texture built from it, another target laid over it,
 *   the end of the task). While the display list is merely away drawing on
 *   the screen or clearing a depth buffer, the picture stays in VRAM and the
 *   target is taken up again as it is.
 * - A texture that is the picture itself, whole, is not built at all: the GE
 *   samples it where it drew it (gfx_target_texture), as the RDP would.
 * - A fill of the whole target with one colour is repeated in RDRAM directly
 *   (gfx_target_filled). Where the fill keeps the coverage bits, that takes
 *   knowing them: RDRAM's are right if nothing was drawn since the last
 *   copy, and they are all set once rectangles have covered the target from
 *   the top down (gfx_target_covered), as the portrait's background does.
 * - Loading goes the same way: the picture in RDRAM is not brought into VRAM
 *   before something is drawn that shows through to it (gfx_target_ready).
 *
 * With these the pocket screen moves no pixels between RDRAM and VRAM at all:
 * each of its pictures starts with a clear or with rectangles that cover it,
 * is drawn, sampled from VRAM, and painted over before the task ends.
 */
#include <pspkernel.h>
#include <string.h>

#include "gfx_internal.h"

RenderTarget gTarget;
/* Staging for block transfers: the GE moves pixels to and from VRAM (the CPU
   reading VRAM directly works on hardware, but emulators don't see it). */
static uint32_t sRtPixels[TARGET_MAX * TARGET_MAX] __attribute__((aligned(64)));

static uint32_t target_bytes(void) {
    return gTarget.width * gTarget.height * (gTarget.siz == G_IM_SIZ_32b ? 4 : 2);
}

/* The part of sRtPixels the target's rows take up (whole cache lines). */
static uint32_t staging_bytes(void) {
    return gTarget.height * TARGET_MAX * 4;
}

/*
 * A 16-bit target that is whole words in RDRAM -- every one the game has --
 * as those words: two pixels each, the first in the high half. NULL for any
 * other, which is converted a pixel at a time.
 */
static uint32_t* target_words(void) {
    uint32_t at = gTarget.addr & RDRAM_MASK;
    if (gTarget.siz != G_IM_SIZ_16b || (gTarget.addr & 3) != 0 || (gTarget.width & 1) != 0 ||
        at + target_bytes() > RDRAM_SIZE) {
        return NULL;
    }
    return (uint32_t*)(void*)(g_rdram + at);
}

static inline uint32_t sum_step(uint32_t sum, uint32_t word) {
    return (sum << 5 | sum >> 27) ^ word;
}

static uint32_t target_rdram_sum(void) {
    uint32_t sum = 0;
    const uint32_t* words = target_words();
    if (words != NULL) {
        for (uint32_t n = target_bytes() / 4; n > 0; n--) {
            sum = sum_step(sum, *words++);
        }
        return sum;
    }
    uint8_t* rdram = g_rdram;
    for (uint32_t off = 0; off < target_bytes(); off += 4) {
        sum = sum_step(sum, (uint32_t)MEM_W(0, gTarget.addr + off));
    }
    return sum;
}

/* The GE's pixels (TARGET_MAX to a row) into the target in RDRAM. Returns target_rdram_sum of the result. */
static uint32_t pixels_to_rdram(const uint32_t* px) {
    uint32_t width = gTarget.width, height = gTarget.height;
    uint32_t* words = target_words();
    if (words != NULL) {
        uint32_t sum = 0;
        for (uint32_t y = 0; y < height; y++, px += TARGET_MAX) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint32_t word = ge_to_rgba5551(px[x]) << 16 | ge_to_rgba5551(px[x + 1]);
                *words++ = word;
                sum = sum_step(sum, word);
            }
        }
        return sum;
    }
    uint8_t* rdram = g_rdram;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t c = px[y * TARGET_MAX + x];
            uint32_t i = y * width + x;
            if (gTarget.siz == G_IM_SIZ_32b) {
                uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
                MEM_W(0, gTarget.addr + 4 * i) = (int32_t)(r << 24 | g << 16 | b << 8 | a);
            } else {
                MEM_HU(0, gTarget.addr + 2 * i) = (uint16_t)ge_to_rgba5551(c);
            }
        }
    }
    return target_rdram_sum();
}

/* The target's rows from `first` down, as they are in RDRAM, as pixels for the GE (its width to a row). */
static void rdram_to_pixels(uint32_t* px, uint32_t first) {
    uint32_t width = gTarget.width, height = gTarget.height;
    const uint32_t* words = target_words();
    if (words != NULL) {
        words += first * width / 2;
        for (uint32_t y = first; y < height; y++, px += width) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint32_t word = *words++;
                px[x] = rgba5551_to_ge(word >> 16);
                px[x + 1] = rgba5551_to_ge(word & 0xFFFF);
            }
        }
        return;
    }
    uint8_t* rdram = g_rdram;
    for (uint32_t y = first; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x, c;
            if (gTarget.siz == G_IM_SIZ_32b) {
                uint32_t v = (uint32_t)MEM_W(0, gTarget.addr + 4 * i);
                c = rgb32_to_ge(v) | (v & 0xFF ? 0xFF000000u : 0);
            } else {
                c = rgba5551_to_ge(MEM_HU(0, gTarget.addr + 2 * i));
            }
            px[(y - first) * width + x] = c;
        }
    }
}

/*
 * Brings the target's picture from RDRAM into VRAM, if that is still to do
 * (gTarget.unloaded): the rows from solid_rows down, the ones above having
 * been drawn over since. The pixels go through the display list's own memory:
 * the GE fetches them when it gets there, which can be after the next load.
 */
void gfx_target_ready(void) {
    if (!gTarget.unloaded) {
        return;
    }
    gTarget.unloaded = false;
    uint32_t first = gTarget.solid_rows;
    if (first >= gTarget.height) {
        return;
    }
    uint32_t rows = gTarget.height - first;
    gfx_flush_batch();
    uint32_t* px = sceGuGetMemory((int)(rows * gTarget.width * 4));
    rdram_to_pixels(px, first);
    sceGuCopyImage(GU_PSM_8888, 0, 0, gTarget.width, rows, gTarget.width, px, 0, first, TARGET_MAX, VRAM_ADDR(TARGET_VRAM));
    sceGuTexSync();
    if (gTracing) {
        rt_log("  render target %08X %ux%u loaded from row %u", gTarget.addr, gTarget.width, gTarget.height, first);
    }
}

/* Points the GE at the target's buffers; its stencil is the coverage bit. */
static void ge_draw_to_target(void) {
    sceGuDrawBufferList(GU_PSM_8888, (void*)TARGET_VRAM, TARGET_MAX);
    sceGuDepthBuffer((void*)TARGET_DEPTH_VRAM, TARGET_MAX);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0xFF, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    gGu.stencil = -1;
    gMap.scale_x = gMap.scale_y = 1.0f;
    gMap.crop_x = gMap.crop_y = 0.0f;
    gMap.x0 = 0;
    gMap.x1 = (int)gTarget.width;
    gMap.y1 = (int)gTarget.height;
    gfx_set_viewport();
    gfx_set_scissor();
}

/* The display list goes back to the screen. What it drew into the target stays in VRAM (gTarget.dirty). */
void gfx_target_leave(void) {
    if (!gTarget.bound) {
        return;
    }
    if (gTarget.unloaded && !gTarget.dirty) {
        gTarget.unloaded = false; /* nothing was drawn: TARGET_VRAM never got the picture, and holds none */
        gTarget.vram_addr = 0;
    }
    gfx_target_ready();
    gfx_flush_batch();
    gTarget.bound = false;
    gfx_draw_to_screen();
}

/*
 * A draw that sets the coverage of every pixel in its rectangle: a fill with a
 * covered colour, a copy from the screen. Rows covered all the way across are
 * counted from the top, which is how the game lays such rectangles down.
 */
void gfx_target_covered(float x0, float y0, float x1, float y1) {
    float top = gRdp.scissor[1] / 4.0f, bottom = gRdp.scissor[3] / 4.0f;
    if (y0 < top) y0 = top;
    if (y1 > bottom) y1 = bottom;
    if (!gTarget.bound || x0 > 0.0f || x1 < (float)gTarget.width || gRdp.scissor[0] != 0 ||
        gRdp.scissor[2] < gTarget.width * 4 || y0 > (float)gTarget.solid_rows || y1 <= (float)gTarget.solid_rows) {
        return;
    }
    gTarget.solid_rows = y1 >= (float)gTarget.height ? gTarget.height : (uint32_t)y1;
}

/* A fill with an uncovered colour: which pixels are covered is no longer known. */
void gfx_target_uncovered(void) {
    gTarget.solid_rows = 0;
}

/*
 * The target's picture as a texture, sampled where the GE drew it. The texture
 * has to be the whole picture read as what it is, 16-bit colour with the
 * coverage bit for alpha (here the stencil: 0 or 255), and the GE must not be
 * drawing into it. Anything else is built from RDRAM after a copy-back.
 * no_target_tex.txt: never.
 */
const GuTexture* gfx_target_texture(uint32_t addr, uint32_t row_texels, uint32_t siz, uint32_t width, uint32_t height,
                                    bool clamp_s, bool clamp_t) {
    static GuTexture sPicture;
    if (!gTarget.dirty || gTarget.bound || gTarget.siz != G_IM_SIZ_16b || siz != G_IM_SIZ_16b ||
        ((addr ^ gTarget.addr) & RDRAM_MASK) != 0 || row_texels != gTarget.width || width != gTarget.width ||
        height != gTarget.height || width < 4 || (width & (width - 1)) != 0 || (height & (height - 1)) != 0 ||
        RT_SWITCH("no_target_tex.txt")) {
        return NULL;
    }
    memset(&sPicture, 0, sizeof(sPicture));
    sPicture.pixels = VRAM_ADDR(TARGET_VRAM);
    sPicture.psm = GU_PSM_8888;
    sPicture.gu_width = (uint16_t)width;
    sPicture.gu_height = (uint16_t)height;
    sPicture.buf_width = TARGET_MAX;
    sPicture.clamp_s = clamp_s;
    sPicture.clamp_t = clamp_t;
    sPicture.sub_x = sPicture.sub_y = 1;
    sPicture.alpha_binary = true;
    if (gTracing) {
        rt_log("  render target %08X %ux%u sampled in VRAM", gTarget.addr, gTarget.width, gTarget.height);
    }
    return &sPicture;
}

/* The target's picture has changed in RDRAM; TARGET_VRAM holds the same. */
static void target_in_rdram(uint32_t sum) {
    gTarget.dirty = false;
    gTarget.vram_addr = gTarget.addr;
    gTarget.vram_sum = sum;
    gfx_tex_invalidate_range(gTarget.addr, target_bytes());
}

/*
 * Copies what the GE drew into the target back into RDRAM, if that is still
 * to do. The wait for the GE ends its display list; the next one carries on
 * with the GE's state as it is, so this may be called in the middle of
 * setting a draw up (a texture turns out to be made from the target).
 */
void gfx_target_flush(void) {
    if (!gTarget.dirty) {
        return;
    }
    uint32_t t0 = sceKernelGetSystemTimeLow();
    gfx_target_ready();
    gfx_flush_batch();
    sceGuCopyImage(GU_PSM_8888, 0, 0, gTarget.width, gTarget.height, TARGET_MAX, VRAM_ADDR(TARGET_VRAM), 0, 0, TARGET_MAX,
                   sRtPixels);
    gfx_finish_list();
    sceKernelDcacheInvalidateRange(sRtPixels, staging_bytes());
    target_in_rdram(pixels_to_rdram(sRtPixels));
    /* The GE is done with the textures retired so far. One made from now on may get
     * the address of the one the GE was last pointed at: that pointer says nothing any more. */
    gfx_tex_flush_retired();
    gGu.tex_pixels = NULL;
    gStats.target_copies++;
    gStats.target_us += sceKernelGetSystemTimeLow() - t0;
    if (gTracing) {
        uint8_t* rdram = g_rdram;
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < TARGET_MAX * gTarget.height; i++) {
            nonzero += (sRtPixels[i] & 0xFFFFFF) != 0;
        }
        rt_log("  render target %08X %ux%u copied back (%u coloured pixels, centre %08X, rdram %04X)", gTarget.addr,
               gTarget.width, gTarget.height, nonzero, sRtPixels[gTarget.height / 2 * TARGET_MAX + gTarget.width / 2],
               MEM_HU(0, gTarget.addr + 2 * (gTarget.height / 2 * gTarget.width + gTarget.width / 2)));
    }
}

void gfx_target_flush_range(uint32_t addr, uint32_t len) {
    uint32_t start = gTarget.addr & RDRAM_MASK;
    addr &= RDRAM_MASK;
    if (addr < start + target_bytes() && addr + len > start) {
        gfx_target_flush();
    }
}

/* Does the scissor leave the whole target open to a fill? */
static bool scissor_covers_target(void) {
    return gRdp.scissor[0] == 0 && gRdp.scissor[1] == 0 && gRdp.scissor[2] >= gTarget.width * 4 &&
           gRdp.scissor[3] >= gTarget.height * 4;
}

/* The whole bound target is about to be overwritten (a clear): what RDRAM holds is not needed in VRAM. */
void gfx_target_overwritten(void) {
    if (scissor_covers_target()) {
        gTarget.unloaded = false;
    } else {
        gfx_target_ready();
    }
}

/*
 * Can a fill of the whole bound target be repeated in RDRAM without asking the
 * GE (gfx_target_filled)? keep_alpha: the fill leaves the coverage bits alone,
 * which must then be up to date in RDRAM.
 */
bool gfx_target_fill_known(bool keep_alpha) {
    return gTarget.bound && target_words() != NULL && scissor_covers_target() &&
           (!keep_alpha || gTarget.was_clean || gTarget.solid_rows >= gTarget.height);
}

/* The GE has filled the whole target with `pixel` (RGBA5551); RDRAM gets the same. */
void gfx_target_filled(uint32_t pixel, bool keep_alpha) {
    uint32_t* words = target_words();
    if (keep_alpha && gTarget.solid_rows >= gTarget.height) {
        pixel |= 1; /* every pixel is covered: nothing to look up */
        keep_alpha = false;
    }
    uint32_t fill = keep_alpha ? (pixel & 0xFFFE) * 0x10001u : (pixel & 0xFFFF) * 0x10001u;
    uint32_t kept = keep_alpha ? 0x00010001u : 0;
    uint32_t sum = 0;
    for (uint32_t n = target_bytes() / 4; n > 0; n--, words++) {
        uint32_t word = fill | (*words & kept);
        *words = word;
        sum = sum_step(sum, word);
    }
    target_in_rdram(sum);
    if (gTracing) {
        rt_log("  render target %08X %ux%u filled with %04X%s", gTarget.addr, gTarget.width, gTarget.height,
               (unsigned)(pixel & 0xFFFF), keep_alpha ? ", coverage kept" : "");
    }
}

/* The draw that gfx_select_target was asked for did not reach the GE after all. */
void gfx_target_not_drawn(void) {
    if (gTarget.bound && gTarget.was_clean) {
        gTarget.dirty = false;
    }
}

static void target_bind(void) {
    uint32_t h = (gRdp.scissor[3] + 3) / 4;
    bool same = gTarget.addr == gRdp.cimg && gTarget.width == gRdp.cimg_width && gTarget.siz == gRdp.cimg_siz;
    if (gTarget.bound && same) {
        return;
    }
    if (same && gTarget.dirty && gTarget.height == h) {
        /* back to a target whose picture is still in VRAM */
        gfx_flush_batch();
        gTarget.bound = true;
        ge_draw_to_target();
        if (gTracing) {
            rt_log("  render target %08X %ux%u again", gTarget.addr, gTarget.width, gTarget.height);
        }
        return;
    }
    /* Another target, or the same one from RDRAM: whatever is still in VRAM goes back first. */
    gfx_target_leave();
    gfx_target_flush();
    gfx_open_frame();
    gfx_flush_batch();
    bool again = same && gTarget.height == h; /* the target of last time, which TARGET_VRAM may still hold */
    gTarget.addr = gRdp.cimg;
    gTarget.width = gRdp.cimg_width;
    gTarget.height = h;
    gTarget.siz = gRdp.cimg_siz;
    gTarget.bound = true;
    gTarget.dirty = false;

    /* The target is to be loaded from RDRAM (when it comes to that: gfx_target_ready) unless TARGET_VRAM holds exactly that. */
    if (!again || gTarget.vram_addr != gTarget.addr || gTarget.vram_sum != target_rdram_sum()) {
        gTarget.unloaded = true;
        gTarget.vram_addr = 0;
        gTarget.solid_rows = 0;
    }
    ge_draw_to_target();
    if (gTracing) {
        rt_log("  render target %08X %ux%u siz %u", gTarget.addr, gTarget.width, gTarget.height, gTarget.siz);
    }
}

/*
 * gfx_select_target (gfx_internal.h) takes the screen at once while this is
 * set: the last selection chose it, and neither the colour and depth images
 * nor the list of display framebuffers -- all it depends on -- have changed.
 * Every triangle asks, and the answer is nearly always the same.
 */
bool gScreenSelected = false;

void gfx_color_image_changed(void) {
    gScreenSelected = false;
}

bool gfx_select_target_slow(bool load) {
    if (gfx_drawing_to_display()) {
        gfx_target_leave();
        gfx_screen_images_drawn();
        gScreenSelected = true;
        return true;
    }
    if (gRdp.cimg == gRdp.zimg || gRdp.cimg_fmt != G_IM_FMT_RGBA ||
        (gRdp.cimg_siz != G_IM_SIZ_16b && gRdp.cimg_siz != G_IM_SIZ_32b) || gRdp.cimg_width > TARGET_MAX ||
        gRdp.scissor[3] > TARGET_MAX * 4 || gRdp.scissor[3] == 0) {
        return false;
    }
    target_bind();
    if (gTarget.pending_zclear != 0 && gTarget.pending_zclear == (gRdp.zimg & 0x1FFFFFFF)) {
        gfx_flush_batch();
        sceGuClearDepth(0);
        sceGuClear(GU_DEPTH_BUFFER_BIT);
        gTarget.pending_zclear = 0;
    }
    gTarget.was_clean = !gTarget.dirty;
    gTarget.dirty = true;
    if (load) {
        gfx_target_ready();
    }
    return true;
}
