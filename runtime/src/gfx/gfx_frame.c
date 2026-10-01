/*
 * gfx_frame.c -- frames on the GE: colour buffers, projection, the viewport,
 * render targets, the softening pass and presenting.
 *
 * The N64 game draws into framebuffers in RDRAM and tells the VI which one to
 * show. Here every frame is drawn straight into one of three colour buffers
 * in VRAM at the PSP's resolution (N64 pixels are scaled by SCALE_X/Y), and
 * osViSwapBuffer (gfx_present_frame) puts it on screen. RDRAM framebuffers are
 * only tracked by address: when the game copies from one, the copy is made
 * from what the PSP shows (gfx_capture_framebuffer, gfx_copy_from_screen).
 */
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>

#include "gfx_internal.h"
#include "prof.h"

static unsigned int sGuList[256 * 1024] __attribute__((aligned(16)));
static bool sGuReady = false;
bool gFrameOpen = false;
int gProjVariant = -1;
GfxStats gStats;

/* ---- the screen map ----------------------------------------------------- */

static const ScreenMap kStretched = { SCALE_X, SCALE_Y, CROP_X, CROP_Y, 0, PSP_SCREEN_W, PSP_SCREEN_H };
#define PILLAR_SCALE_X ((float)PILLAR_W / (N64_SCREEN_W - 2 * CROP_X))
static const ScreenMap kPillar = { PILLAR_SCALE_X, SCALE_Y, CROP_X - PILLAR_X / PILLAR_SCALE_X, CROP_Y,
                                   PILLAR_X, PILLAR_X + PILLAR_W, PSP_SCREEN_H };

/*
 * The game's thread asks for the other shape (START + SELECT); the worker
 * takes it up between frames, so no frame is drawn half in each. A colour
 * buffer last drawn in the other shape has that picture left in the bars and
 * is cleared when its turn comes (sBufStretched).
 */
static volatile int sStretchWanted = -1; /* -1: not yet read from no_stretch.txt */
static bool sStretched = true;
static bool sBufStretched[3] = { true, true, true };
static ScreenMap sScreen = { SCALE_X, SCALE_Y, CROP_X, CROP_Y, 0, PSP_SCREEN_W, PSP_SCREEN_H };
ScreenMap gMap = { SCALE_X, SCALE_Y, CROP_X, CROP_Y, 0, PSP_SCREEN_W, PSP_SCREEN_H };

void rt_gfx_toggle_stretch(void) {
    sStretchWanted = sStretchWanted == 0;
}

static void map_screen(void) {
    gMap = sScreen;
}

/* Between frames: take up the shape asked for. */
static void update_stretch(void) {
    if (sStretchWanted < 0) {
        sStretchWanted = !RT_SWITCH("no_stretch.txt");
    }
    bool wanted = sStretchWanted != 0;
    if (wanted == sStretched) {
        return;
    }
    sStretched = wanted;
    sScreen = wanted ? kStretched : kPillar;
    if (!gTarget.bound) {
        map_screen();
    }
    rt_log("gfx: picture %s", wanted ? "stretched" : "at 4:3");
}

/* ---- projection, viewport, scissor -------------------------------------- */

static const ScePspFMatrix4 kIdentity = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};

static void to_gu_matrix(ScePspFMatrix4* out, const Mat4 m) {
    out->x.x = m[0][0]; out->x.y = m[0][1]; out->x.z = m[0][2]; out->x.w = m[0][3];
    out->y.x = m[1][0]; out->y.y = m[1][1]; out->y.z = m[1][2]; out->y.w = m[1][3];
    out->z.x = m[2][0]; out->z.y = m[2][1]; out->z.z = m[2][2]; out->z.w = m[2][3];
    out->w.x = m[3][0]; out->w.y = m[3][1]; out->w.z = m[3][2]; out->w.w = m[3][3];
}

static bool build_fog_view(Mat4 view, Mat4 proj_out) {
    const float (*p)[4] = (const float (*)[4])gRsp.proj;
    float c = -p[2][3];
    if (c > -0.00001f && c < 0.00001f) {
        return false;
    }
    /* view: x' = x, y' = y, z' = -(clip w), row-vector convention */
    mat_identity(view);
    view[0][2] = -p[0][3];
    view[1][2] = -p[1][3];
    view[2][2] = c;
    view[3][2] = -p[3][3];
    /* inverse of view */
    Mat4 inv;
    mat_identity(inv);
    inv[2][2] = 1.0f / c;
    inv[0][2] = -view[0][2] / c;
    inv[1][2] = -view[1][2] / c;
    inv[3][2] = -view[3][2] / c;
    mat_mul(proj_out, inv, gRsp.proj);
    return true;
}

/* Pin clip z to -w (DEPTH_NEAR) or +w (DEPTH_FAR): the depth the RDP clamps
 * geometry in front of the near plane / beyond the far plane to. */
static void pin_depth(Mat4 m, int depth) {
    float sign = depth == DEPTH_NEAR ? -DEPTH_PIN : DEPTH_PIN;
    for (int i = 0; i < 4; i++) {
        m[i][2] = sign * m[i][3];
    }
}

void gfx_upload_projection_depth(int variant, int depth) {
    ScePspFMatrix4 m;
    Mat4 tmp;
    if (variant == PROJ_FOG) {
        Mat4 view;
        if (build_fog_view(view, tmp)) {
            ScePspFMatrix4 v;
            to_gu_matrix(&v, (const float (*)[4])view);
            sceGuSetMatrix(GU_VIEW, &v);
            if (depth != DEPTH_NORMAL) {
                pin_depth(tmp, depth);
            }
            to_gu_matrix(&m, (const float (*)[4])tmp);
            sceGuSetMatrix(GU_PROJECTION, &m);
            gProjVariant = depth == DEPTH_NORMAL ? variant : -1;
            return;
        }
        variant = PROJ_NORMAL;
    }
    memcpy(tmp, gRsp.proj, sizeof(Mat4));
    if (variant == PROJ_FLAT_Z) {
        tmp[0][2] = tmp[1][2] = tmp[2][2] = tmp[3][2] = 0.0f;
    } else if (depth != DEPTH_NORMAL) {
        pin_depth(tmp, depth);
    }
    if (gProjVariant == PROJ_FOG || gProjVariant < 0) {
        sceGuSetMatrix(GU_VIEW, &kIdentity);
    }
    to_gu_matrix(&m, (const float (*)[4])tmp);
    sceGuSetMatrix(GU_PROJECTION, &m);
    gProjVariant = depth == DEPTH_NORMAL ? variant : -1;
}

void gfx_upload_projection(int variant) {
    gfx_upload_projection_depth(variant, DEPTH_NORMAL);
}

void gfx_set_projection(void) {
    gfx_update_snap();
    gfx_upload_projection(gProjVariant < 0 ? PROJ_NORMAL : gProjVariant);
}

/* GE fog parameters equivalent to the RSP fog (fm/fo) for the current projection. */
bool gfx_compute_fog_range(float* near_out, float* far_out) {
    const float (*p)[4] = (const float (*)[4])gRsp.proj;
    float fm = gRsp.fog_mul;
    float fo = gRsp.fog_ofs;
    if (fm == 0) {
        return false;
    }
    /* clip z = alpha * (clip w - tw) + tz for a perspective projection */
    float pz[3] = { p[0][2], p[1][2], p[2][2] };
    float pw[3] = { p[0][3], p[1][3], p[2][3] };
    float ww = pw[0] * pw[0] + pw[1] * pw[1] + pw[2] * pw[2];
    if (ww < 1e-12f) {
        return false;
    }
    float alpha = (pz[0] * pw[0] + pz[1] * pw[1] + pz[2] * pw[2]) / ww;
    float beta = p[3][2] - alpha * p[3][3];
    float z_start = -fo / fm;
    float z_end = (255.0f - fo) / fm;
    float d0 = z_start - alpha;
    float d1 = z_end - alpha;
    if ((d0 > -1e-6f && d0 < 1e-6f) || (d1 > -1e-6f && d1 < 1e-6f)) {
        return false;
    }
    float w_start = beta / d0;
    float w_end = beta / d1;
    if (w_end <= 0) {
        return false;
    }
    if (w_start < 0) {
        w_start = 0;
    }
    *near_out = w_start;
    *far_out = w_end;
    return true;
}

void gfx_set_viewport(void) {
    gfx_update_snap();
    float hw = gRsp.vscale[0] / 4.0f * gMap.scale_x;
    float hh = gRsp.vscale[1] / 4.0f * gMap.scale_y;
    float cx = (gRsp.vtrans[0] / 4.0f - gMap.crop_x) * gMap.scale_x;
    float cy = (gRsp.vtrans[1] / 4.0f - gMap.crop_y) * gMap.scale_y;
    sceGuViewport(GU_OFFSET_X + (int)(cx + 0.5f), GU_OFFSET_Y + (int)(cy + 0.5f), (int)(hw * 2 + 0.5f), (int)(hh * 2 + 0.5f));
}

void gfx_set_scissor(void) {
    int x0 = (int)((gRdp.scissor[0] / 4.0f - gMap.crop_x) * gMap.scale_x);
    int y0 = (int)((gRdp.scissor[1] / 4.0f - gMap.crop_y) * gMap.scale_y);
    int x1 = (int)((gRdp.scissor[2] / 4.0f - gMap.crop_x) * gMap.scale_x + 0.5f);
    int y1 = (int)((gRdp.scissor[3] / 4.0f - gMap.crop_y) * gMap.scale_y + 0.5f);
    if (x1 > gMap.x1) x1 = gMap.x1;
    if (y1 > gMap.y1) y1 = gMap.y1;
    if (x0 < gMap.x0) x0 = gMap.x0;
    if (y0 < 0) y0 = 0;
    sceGuScissor(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

/* ---- colour buffers ----------------------------------------------------- */

/*
 * Three colour buffers, rotated by hand: one on screen, one handed to the
 * display for the next vertical blank, one to draw into. With two, the
 * renderer had to wait after every swap until it reached the screen before it
 * could draw again -- up to a whole retrace, and since frames are presented by
 * the worker whenever it gets there (gfx_worker.c), in the village that pushed
 * frames past two retraces and the game fell from 30 to 20 fps.
 *
 * VRAM (2 MB): colour buffers at 0, 0x88000 and 0x154000 (512x272x4 each),
 * depth at 0x110000, render targets and the softening pass from 0x1DC000.
 */
#define DEPTH_VRAM 0x110000
static const uint32_t kColorBufs[3] = { 0x000000, 0x088000, 0x154000 };
static int sDrawBuf = 0;           /* where the open (or next) frame is drawn */
static int sShownBuf = 1;          /* last buffer handed to the display */
static int sPrevShownBuf = 2;      /* the one before it */
static uint32_t sShownVcount = 0;  /* vblank count when sShownBuf was handed over */
static uint32_t sPrevShownVcount = 0; /* ... and when sPrevShownBuf was */
static bool sFrameDrawn = false;   /* something was drawn since the last hand-over */

void gfx_open_frame(void) {
    if (gFrameOpen) {
        return;
    }
    /*
     * The buffer we draw into was on screen until sPrevShownBuf replaced it, at
     * the first vertical blank after sPrevShownBuf was handed over. Only if two
     * frames were handed over within one retrace can it still be showing. This
     * runs on the renderer's worker thread, so the game keeps running if it has
     * to wait.
     */
    uint32_t vw0 = sceKernelGetSystemTimeLow();
    while (sceDisplayGetVcount() == sPrevShownVcount) {
        sceDisplayWaitVblankStart();
    }
    gStats.vblank_wait_us += sceKernelGetSystemTimeLow() - vw0;
    gFrameOpen = true;
    sFrameDrawn = true;
    gfx_batches_new_frame();
    sceGuStart(GU_DIRECT, sGuList);
    sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    sceGuDepthBuffer((void*)DEPTH_VRAM, BUF_WIDTH);
    sceGuDisable(GU_STENCIL_TEST);
    gfx_gu_reset_cache();
    sceGuSetMatrix(GU_VIEW, &kIdentity);
    sceGuSetMatrix(GU_MODEL, &kIdentity);
    sceGuSetMatrix(GU_TEXTURE, &kIdentity);
    sceGuDepthRange(65535, 0);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_CULL_FACE);
    /* With depth clipping off the GE drops any triangle with a vertex outside
     * the depth range. gfx_draw_triangle keeps vertices inside it, pinning the
     * depth of pieces the RDP would clamp; clipping on only guards against
     * rounding at the planes. */
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuDisable(GU_FOG);
    gGu.fog = 0;
    if (sBufStretched[sDrawBuf] != sStretched) {
        sBufStretched[sDrawBuf] = sStretched;
        sceGuScissor(0, 0, PSP_SCREEN_W, PSP_SCREEN_H);
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT);
    }
    gfx_upload_projection(PROJ_NORMAL);
    gfx_set_viewport();
    gfx_set_scissor();
}

void rt_gfx_init(void) {
    if (sGuReady) {
        return;
    }
#ifdef RT_GFXPROF
    gfx_prof_init();
#endif
    sGuReady = true;
    gfx_debug_init();
    gfx_vertex_init();
    gfx_tmem_reset();
    gfx_tex_init();
    sceGuInit();
    sceGuStart(GU_DIRECT, sGuList);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(PSP_SCREEN_W, PSP_SCREEN_H, (void*)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void*)DEPTH_VRAM, BUF_WIDTH);
    sceGuOffset(GU_OFFSET_X, GU_OFFSET_Y);
    sceGuViewport(2048, 2048, PSP_SCREEN_W, PSP_SCREEN_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, PSP_SCREEN_W, PSP_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    /*
     * pspgu swaps on the next hsync by default, which tears: the screen shows
     * the old buffer above the scanline and the new one below it. Swap on the
     * vertical blank instead. The pointers still change straight away, so
     * gfx_open_frame waits for the swap to reach the screen before drawing into
     * the buffer it just took (which is the one still being displayed).
     */
    guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTVSYNC);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    /* clear the third colour buffer too, so it never shows garbage */
    sceGuStart(GU_DIRECT, sGuList);
    for (int i = 0; i < 3; i++) {
        sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[i], BUF_WIDTH);
        sceGuClear(GU_COLOR_BUFFER_BIT);
    }
    sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    sceGuFinish();
    sceGuSync(0, 0);
}

/* ---- framebuffers ------------------------------------------------------- */

static uint32_t sDisplayFbs[4];
static int sNumDisplayFbs = 0;
static uint32_t sLastShownFb = 0;

bool gfx_is_display_fb(uint32_t addr) {
    addr &= 0x1FFFFFFF;
    for (int i = 0; i < sNumDisplayFbs; i++) {
        if (sDisplayFbs[i] == addr) {
            return true;
        }
    }
    return false;
}

static void note_display_fb(uint32_t addr) {
    addr &= 0x1FFFFFFF;
    sLastShownFb = addr;
    gScreenSelected = false; /* the list of display framebuffers may change */
    if (!gfx_is_display_fb(addr)) {
        if (sNumDisplayFbs < 4) {
            sDisplayFbs[sNumDisplayFbs++] = addr;
        } else {
            memmove(sDisplayFbs, sDisplayFbs + 1, sizeof(sDisplayFbs[0]) * 3);
            sDisplayFbs[3] = addr;
        }
    }
}

/* Is the current RDP colour image one that ends up on screen? */
bool gfx_drawing_to_display(void) {
    if (gRdp.cimg == gRdp.zimg) {
        return false;
    }
    return sNumDisplayFbs == 0 || gfx_is_display_fb(gRdp.cimg);
}

static uint32_t* sCapturePixels = NULL; /* allocated on the first capture */

/* Copy what the PSP shows for N64 framebuffer `src` into RDRAM at `dst` as RGBA5551. */
void gfx_capture_framebuffer(uint32_t src, uint32_t dst, uint32_t width, uint32_t height) {
    src &= 0x1FFFFFFF;
    if (sCapturePixels == NULL) {
        sCapturePixels = memalign(16, BUF_WIDTH * PSP_SCREEN_H * 4);
        if (sCapturePixels == NULL) {
            RT_LOG_ONCE("gfx: no memory for framebuffer captures");
            return;
        }
    }
    bool from_back = src != sLastShownFb;
    uint32_t vram = kColorBufs[from_back ? sDrawBuf : sShownBuf];
    gfx_target_need(dst, width * height * 2); /* a target's picture there goes in first, under the capture */

    uint32_t cw0 = sceKernelGetSystemTimeLow();
    gfx_open_frame();
    gfx_flush_batch();
    sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_SCREEN_W, PSP_SCREEN_H, BUF_WIDTH, VRAM_ADDR(vram), 0, 0,
                   BUF_WIDTH, sCapturePixels);
    sceGuFinish();
    sceGuSync(0, 0);
    gStats.capture_us += sceKernelGetSystemTimeLow() - cw0;
    gStats.captures++;
    sceKernelDcacheInvalidateRange(sCapturePixels, BUF_WIDTH * PSP_SCREEN_H * 4);
    gFrameOpen = false;
    gfx_open_frame();

    if (width > N64_SCREEN_W) width = N64_SCREEN_W;
    if (height > N64_SCREEN_H) height = N64_SCREEN_H;
    uint8_t* rdram = g_rdram;
    for (uint32_t y = 0; y < height; y++) {
        int sy = (int)(((float)y + 0.5f - sScreen.crop_y) * sScreen.scale_y);
        sy = sy < 0 ? 0 : sy >= sScreen.y1 ? sScreen.y1 - 1 : sy;
        for (uint32_t x = 0; x < width; x++) {
            int sx = (int)(((float)x + 0.5f - sScreen.crop_x) * sScreen.scale_x);
            sx = sx < sScreen.x0 ? sScreen.x0 : sx >= sScreen.x1 ? sScreen.x1 - 1 : sx;
            uint32_t c = sCapturePixels[sy * BUF_WIDTH + sx];
            uint32_t r = (c >> 3) & 31, g = (c >> 11) & 31, b = (c >> 19) & 31;
            uint16_t v = (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
            MEM_HU(0, dst + 2 * (y * width + x)) = v;
        }
    }
    if (gTracing) {
        rt_log("  capture fb %08X (%s) -> %08X %ux%u", src, from_back ? "back" : "front", dst, width, height);
    }
}

/* ---- render targets ----------------------------------------------------- */

/*
 * AF draws a few things into small colour images of its own and then uses
 * them as textures: the player's portrait in the pocket screen (128x128, over
 * a copy of the screen behind it) and the item pictures beside it (32x32).
 *
 * Such images, up to RT_MAX square, are drawn on the GE at N64 resolution in
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
 * screen far from 30 fps. So the copy is put off until something reads the
 * picture from RDRAM (gfx_target_need: a texture made from it, another target
 * laid over it, the end of the task): while the display list is merely away
 * drawing on the screen or clearing a depth buffer, the picture stays in
 * VRAM and the target is taken up again as it is. And a fill of the whole
 * target with one colour is repeated in RDRAM directly (gfx_target_filled),
 * which needs nothing back from the GE at all.
 */
#define RT_VRAM 0x1DC000       /* 128x128x4 */
#define RT_DEPTH_VRAM 0x1EC000 /* 128x128x2, ends at 0x1F4000 */
#define RT_MAX 128

RenderTarget gTarget;
uint32_t gDisplayZimg = 0;
static uint32_t sDrawingFb = 0;   /* the screen colour image of the frame being drawn */
/* Staging for block transfers: the GE moves pixels to and from VRAM (the CPU
   reading VRAM directly works on hardware, but emulators don't see it). */
static uint32_t sRtPixels[RT_MAX * RT_MAX] __attribute__((aligned(64)));

static uint32_t target_bytes(void) {
    return gTarget.width * gTarget.height * (gTarget.siz == G_IM_SIZ_32b ? 4 : 2);
}

/* The part of sRtPixels the target's rows take up (whole cache lines). */
static uint32_t staging_bytes(void) {
    return gTarget.height * RT_MAX * 4;
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

/* A GE pixel as the N64's RGBA5551 (the stencil is its alpha bit) and back. */
static inline uint32_t to_rgba16(uint32_t c) {
    return (c & 0xF8) << 8 | (c & 0xF800) >> 5 | (c & 0xF80000) >> 18 | (c >> 24 != 0);
}

static inline uint32_t from_rgba16(uint32_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (v & 1 ? 0xFF000000u : 0);
}

/* The GE's pixels (RT_MAX to a row) into the target in RDRAM. Returns target_rdram_sum of the result. */
static uint32_t pixels_to_rdram(const uint32_t* px) {
    uint32_t width = gTarget.width, height = gTarget.height;
    uint32_t* words = target_words();
    if (words != NULL) {
        uint32_t sum = 0;
        for (uint32_t y = 0; y < height; y++, px += RT_MAX) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint32_t word = to_rgba16(px[x]) << 16 | to_rgba16(px[x + 1]);
                *words++ = word;
                sum = sum_step(sum, word);
            }
        }
        return sum;
    }
    uint8_t* rdram = g_rdram;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t c = px[y * RT_MAX + x];
            uint32_t i = y * width + x;
            if (gTarget.siz == G_IM_SIZ_32b) {
                uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
                MEM_W(0, gTarget.addr + 4 * i) = (int32_t)(r << 24 | g << 16 | b << 8 | a);
            } else {
                MEM_HU(0, gTarget.addr + 2 * i) = (uint16_t)to_rgba16(c);
            }
        }
    }
    return target_rdram_sum();
}

/* The target in RDRAM as pixels for the GE (RT_MAX to a row). */
static void rdram_to_pixels(uint32_t* px) {
    uint32_t width = gTarget.width, height = gTarget.height;
    const uint32_t* words = target_words();
    if (words != NULL) {
        for (uint32_t y = 0; y < height; y++, px += RT_MAX) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint32_t word = *words++;
                px[x] = from_rgba16(word >> 16);
                px[x + 1] = from_rgba16(word & 0xFFFF);
            }
        }
        return;
    }
    uint8_t* rdram = g_rdram;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x, c;
            if (gTarget.siz == G_IM_SIZ_32b) {
                uint32_t v = (uint32_t)MEM_W(0, gTarget.addr + 4 * i);
                c = (v >> 24) | ((v >> 16) & 0xFF) << 8 | ((v >> 8) & 0xFF) << 16 | (v & 0xFF ? 0xFF000000u : 0);
            } else {
                c = from_rgba16(MEM_HU(0, gTarget.addr + 2 * i));
            }
            px[y * RT_MAX + x] = c;
        }
    }
}

/* Points the GE at the target's buffers; its stencil is the coverage bit. */
static void ge_draw_to_target(void) {
    sceGuDrawBufferList(GU_PSM_8888, (void*)RT_VRAM, RT_MAX);
    sceGuDepthBuffer((void*)RT_DEPTH_VRAM, RT_MAX);
    sceGuEnable(GU_STENCIL_TEST);
    sceGuStencilFunc(GU_ALWAYS, 0xFF, 0xFF);
    sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
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
    gfx_flush_batch();
    gTarget.bound = false;
    map_screen();
    sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    sceGuDepthBuffer((void*)DEPTH_VRAM, BUF_WIDTH);
    sceGuDisable(GU_STENCIL_TEST);
    gfx_set_viewport();
    gfx_set_scissor();
}

/* The target's picture has changed in RDRAM; RT_VRAM holds the same. */
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
    gfx_flush_batch();
    sceGuCopyImage(GU_PSM_8888, 0, 0, gTarget.width, gTarget.height, RT_MAX, VRAM_ADDR(RT_VRAM), 0, 0, RT_MAX,
                   sRtPixels);
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, sGuList);
    /* (pspgu opens every list with the colour buffer of sceGuDrawBuffer, which isn't ours) */
    if (gTarget.bound) {
        sceGuDrawBufferList(GU_PSM_8888, (void*)RT_VRAM, RT_MAX);
    } else {
        sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    }
    gfx_batches_new_frame();
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
        for (uint32_t i = 0; i < RT_MAX * gTarget.height; i++) {
            nonzero += (sRtPixels[i] & 0xFFFFFF) != 0;
        }
        rt_log("  render target %08X %ux%u copied back (%u coloured pixels, centre %08X, rdram %04X)", gTarget.addr,
               gTarget.width, gTarget.height, nonzero, sRtPixels[gTarget.height / 2 * RT_MAX + gTarget.width / 2],
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

/*
 * Can a fill of the whole bound target be repeated in RDRAM without asking the
 * GE (gfx_target_filled)? keep_alpha: the fill leaves the coverage bits alone,
 * which must then be up to date in RDRAM.
 */
bool gfx_target_fill_known(bool keep_alpha) {
    return gTarget.bound && target_words() != NULL && scissor_covers_target() && (!keep_alpha || gTarget.was_clean);
}

/* The GE has filled the whole target with `pixel` (RGBA5551); RDRAM gets the same. */
void gfx_target_filled(uint32_t pixel, bool keep_alpha) {
    uint32_t* words = target_words();
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
    gTarget.addr = gRdp.cimg;
    gTarget.width = gRdp.cimg_width;
    gTarget.height = h;
    gTarget.siz = gRdp.cimg_siz;
    gTarget.bound = true;
    gTarget.dirty = false;

    /* Load the target from RDRAM unless RT_VRAM already holds exactly that. */
    if (gTarget.vram_addr != gTarget.addr || gTarget.vram_sum != target_rdram_sum()) {
        rdram_to_pixels(sRtPixels);
        sceKernelDcacheWritebackRange(sRtPixels, staging_bytes());
        sceGuCopyImage(GU_PSM_8888, 0, 0, gTarget.width, gTarget.height, RT_MAX, sRtPixels, 0, 0, RT_MAX,
                       VRAM_ADDR(RT_VRAM));
        sceGuTexSync();
        gTarget.vram_addr = 0;
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

bool gfx_select_target_slow(void) {
    if (gfx_drawing_to_display()) {
        gfx_target_leave();
        gDisplayZimg = gRdp.zimg & 0x1FFFFFFF;
        sDrawingFb = gRdp.cimg & 0x1FFFFFFF;
        gScreenSelected = true;
        return true;
    }
    if (gRdp.cimg == gRdp.zimg || gRdp.cimg_fmt != G_IM_FMT_RGBA ||
        (gRdp.cimg_siz != G_IM_SIZ_16b && gRdp.cimg_siz != G_IM_SIZ_32b) || gRdp.cimg_width > RT_MAX ||
        gRdp.scissor[3] > RT_MAX * 4 || gRdp.scissor[3] == 0) {
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
    return true;
}

/*
 * A texture rectangle or background whose source is the screen, drawn into a
 * render target: sample the PSP's colour buffer. dst is in target pixels, src
 * in N64 screen pixels.
 */
void gfx_copy_from_screen(uint32_t fb, float dx0, float dy0, float dx1, float dy1, float sx0, float sy0,
                          float sx1, float sy1) {
    gfx_flush_batch();
    /* The frame being drawn is in the draw buffer; a finished one is on screen. */
    bool from_back = (fb & 0x1FFFFFFF) == sDrawingFb || (fb & 0x1FFFFFFF) != sLastShownFb;
    uint32_t vram = kColorBufs[from_back ? sDrawBuf : sShownBuf];
    gfx_gu_reset_cache();
    gRdp.state_dirty = true;
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 512, 512, BUF_WIDTH, VRAM_ADDR(vram));
    sceGuTexFlush();
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    gfx_draw_rect(dx0, dy0, dx1, dy1, (sx0 - sScreen.crop_x) * sScreen.scale_x, (sy0 - sScreen.crop_y) * sScreen.scale_y,
                  (sx1 - sScreen.crop_x) * sScreen.scale_x, (sy1 - sScreen.crop_y) * sScreen.scale_y, 0xFFFFFFFF, NULL,
                  false);
    gfx_gu_reset_cache();
}

/* The screen position of RDRAM address `addr`, if it lies in a display framebuffer. */
bool gfx_screen_position(uint32_t addr, uint32_t* fb, float* x, float* y) {
    addr &= 0x1FFFFFFF;
    for (int i = 0; i < sNumDisplayFbs; i++) {
        uint32_t off = addr - sDisplayFbs[i];
        if (addr >= sDisplayFbs[i] && off < N64_SCREEN_W * N64_SCREEN_H * 2) {
            *fb = sDisplayFbs[i];
            *x = (float)(off / 2 % N64_SCREEN_W);
            *y = (float)(off / 2 / N64_SCREEN_W);
            return true;
        }
    }
    return false;
}

/* ---- softening ---------------------------------------------------------- */

/*
 * Softening. The N64's picture reaches the TV through the VI, which filters
 * polygon edges and blurs a little; the GE's picture, drawn straight at the
 * PSP's resolution, looks harsh beside it. So the finished frame is blended
 * with a blurred copy of itself, by the strength in soften.txt (percent; a
 * file holding 0 turns it off, SOFTEN_DEFAULT without the file).
 *
 * The blur is the frame shrunk to half size with bilinear filtering (each
 * texel the average of 2x2 pixels) and drawn back over it enlarged, bilinear
 * again, with the strength as a fixed blend weight: two sprites, neither
 * reading the buffer it writes. The half-size copy lives in the render
 * target's VRAM and the spare VRAM after it (256x136x4 from RT_VRAM, ending
 * below 2 MB). Nothing is bound there between frames, and a render target's
 * depth is cleared by the game before use, so only the cached colour must be
 * forgotten.
 *
 * Measured on the PSP-1000: this is GE time at the end of every frame. A
 * version that blended four neighbour samples from banded copies of the
 * frame cost 7-29 ms; wide textured sprites are slow, so both passes are
 * drawn in strips 64 pixels wide.
 *
 * A scaled through-mode sprite is sampled at pixel centres, texel centres at
 * i + 0.5: shrinking samples half-size pixel i at 2i + 1, between pixels 2i
 * and 2i + 1, and enlarging samples pixel x at (x + 0.5) / 2 -- the plain
 * mapping both ways, with no shift (measured in PPSSPP).
 */
#define SOFTEN_DEFAULT 30
#define SOFT_VRAM RT_VRAM   /* 256 x 136 x 4 = 0x22000: ends at 0x1FE000 */
#define SOFT_W (PSP_SCREEN_W / 2)
#define SOFT_H (PSP_SCREEN_H / 2)
#define SOFT_STRIP 64
static int sSoften = -1;    /* strength, 0..255 */

/* A sprite from (x0,y0)-(x1,y1) sampling (u0,v0)-(u1,v1), in strips. */
static void soften_sprite(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    int n = (int)((x1 - x0 + SOFT_STRIP - 1) / SOFT_STRIP);
    float du = (u1 - u0) / (x1 - x0);
    GuVertex* m = sceGuGetMemory(2 * n * sizeof(GuVertex));
    for (int i = 0; i < n; i++) {
        float a = x0 + i * SOFT_STRIP, b = a + SOFT_STRIP < x1 ? a + SOFT_STRIP : x1;
        m[2 * i] = (GuVertex){ u0 + (a - x0) * du, v0, 0xFFFFFFFF, a, y0, 0 };
        m[2 * i + 1] = (GuVertex){ u0 + (b - x0) * du, v1, 0xFFFFFFFF, b, y1, 0 };
    }
    sceGuDrawArray(GU_SPRITES, GU_VTYPE | GU_TRANSFORM_2D, 2 * n, NULL, m);
}

/* soften_frame's passes, queued into the current display list. */
static void soften_passes(void) {
    void* screen = (void*)kColorBufs[sDrawBuf];
    gfx_gu_reset_cache();
    gRdp.state_dirty = true;
    gTarget.vram_addr = 0; /* the render target's VRAM is about to be overwritten */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(1);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuPixelMask(0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    /* the picture (not the bars beside it, if any) at half size */
    int x0 = sScreen.x0, x1 = sScreen.x1, hx0 = x0 / 2, hx1 = x1 / 2;
    sceGuDisable(GU_BLEND);
    sceGuDrawBufferList(GU_PSM_8888, (void*)SOFT_VRAM, 256);
    sceGuTexImage(0, 512, 512, BUF_WIDTH, VRAM_ADDR(screen));
    sceGuTexFlush();
    soften_sprite(hx0, 0, hx1, SOFT_H, x0, 0, x1, PSP_SCREEN_H);
    /* its last column and row repeated: the enlarged sprite's right and bottom
     * pixels sample a quarter texel past them (and its left ones, where the
     * picture does not start at the texture's edge) */
    void* half = VRAM_ADDR(SOFT_VRAM);
    sceGuCopyImage(GU_PSM_8888, hx1 - 1, 0, 1, SOFT_H, 256, half, hx1, 0, 256, half);
    if (hx0 > 0) {
        sceGuCopyImage(GU_PSM_8888, hx0, 0, 1, SOFT_H, 256, half, hx0 - 1, 0, 256, half);
    }
    sceGuCopyImage(GU_PSM_8888, 0, SOFT_H - 1, SOFT_W + 1, 1, 256, half, 0, SOFT_H, 256, half);

    /* and back over it, enlarged */
    sceGuDrawBufferList(GU_PSM_8888, screen, BUF_WIDTH);
    sceGuTexSync();
    sceGuTexImage(0, 256, 256, 256, VRAM_ADDR(SOFT_VRAM));
    sceGuTexFlush();
    uint32_t k = (uint32_t)sSoften, ik = 255 - k;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, k | (k << 8) | (k << 16), ik | (ik << 8) | (ik << 16));
    soften_sprite(x0, 0, x1, PSP_SCREEN_H, hx0, 0, hx1, SOFT_H);
    gfx_gu_reset_cache();
}

static void soften_frame(void) {
    if (sSoften < 0) {
        uint32_t v[1];
        /* (the list reader skips zeros: a file holding just 0 reads as empty) */
        int n = rt_load_number_list("soften.txt", v, 1);
        uint32_t pct = n >= 1 ? (v[0] > 100 ? 100 : v[0]) : rt_data_file_exists("soften.txt") ? 0 : SOFTEN_DEFAULT;
        sSoften = (int)(pct * 255 / 100);
        rt_log("soften: %u%%", (unsigned)pct);
    }
    if (sSoften == 0) {
        return;
    }
    /*
     * soften_bench.txt: time the pass on the GE alone -- the list so far is
     * finished and waited for, the pass runs in a list of its own, and the
     * frame goes on in a new one. Costs the overlap of GE and CPU, so only
     * for measuring.
     */
    bool bench = RT_SWITCH("soften_bench.txt");
    static uint32_t bench_us = 0, bench_max = 0, bench_n = 0;
    uint32_t t0 = 0;
    if (bench) {
        sceGuFinish();
        sceGuSync(0, 0);
        sceGuStart(GU_DIRECT, sGuList);
        t0 = sceKernelGetSystemTimeLow();
    }
    soften_passes();
    if (bench) {
        sceGuFinish();
        sceGuSync(0, 0);
        uint32_t dt = sceKernelGetSystemTimeLow() - t0;
        sceGuStart(GU_DIRECT, sGuList);
        sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
        bench_us += dt;
        bench_max = dt > bench_max ? dt : bench_max;
        if (++bench_n == 120) {
            rt_log("soften bench: %u us per frame on the GE (max %u)", (unsigned)(bench_us / bench_n),
                   (unsigned)bench_max);
            bench_us = bench_max = bench_n = 0;
        }
    }
}

/* ---- presenting --------------------------------------------------------- */

static void ge_sync(void* arg) {
    sceGuSync(0, 0);
}

/* Every 120 frames: where the time went (see runtime/README.md for how to read these lines). */
static void log_stats(void) {
    static uint64_t sStatStart = 0;
    static uint64_t sStatIdleStart = 0;
    static uint32_t sStatTasks = 0;
    uint64_t now = sceKernelGetSystemTimeWide();
    uint64_t span = now - sStatStart;
    uint64_t idle = g_idle_us - sStatIdleStart;
    char prof[160];
    rt_prof_report(prof, sizeof(prof), (uint32_t)span, 120);
    char threads[160];
    rt_sched_report(threads, sizeof(threads), (uint32_t)span);
    rt_log("idle %u%% of the CPU", (unsigned)rt_idle_percent((uint32_t)span));
    /*
     * Where the renderer's time went: its share of wall time, the time per
     * task, how much of that it was on the CPU (the rest is preemption by the
     * game or waiting), vertical-blank waits before drawing, framebuffer
     * captures, and how long the game's scheduler waited to hand over a task.
     */
    uint32_t tb, tbk, tbus;
    gfx_tex_take_build_stats(&tb, &tbk, &tbus);
    uint32_t tasks = gStats.tasks - sStatTasks;
    rt_log("gfx: render %u%% of wall time (%u us a task, %u us of them on the CPU), vblank waits %u us, %u captures (%u us), %u render targets (%u us), %u textures built (%u baked, %u us, %u put off), submit wait avg %u max %u us",
           span ? (unsigned)(gStats.render_us * 100 / span) : 0,
           tasks ? (unsigned)(gStats.render_us / tasks) : 0,
           tasks ? (unsigned)(gStats.render_cpu_us / tasks) : 0,
           (unsigned)gStats.vblank_wait_us, (unsigned)gStats.captures, (unsigned)gStats.capture_us,
           (unsigned)gStats.target_copies, (unsigned)gStats.target_us, (unsigned)tb, (unsigned)tbk, (unsigned)tbus,
           (unsigned)gfx_tex_take_deferred(),
           (unsigned)(gStats.submit_wait_n ? gStats.submit_wait_sum / gStats.submit_wait_n : 0),
           (unsigned)gStats.submit_wait_max);
    rt_log("frame %u (poll %u): %u gfx tasks, %u yields, %u verts, %u draws, %u tris (in %u, trivial %u, culled %u, clipped %u, %u GE verts) | busy %u%% = %u us a frame (gfx %u%%, blocked %u%%, audio %u%%) |%s |%s",
           gStats.frames, (unsigned)rt_input_polls(), gStats.tasks, (unsigned)gStats.yields, gStats.vertices,
           gStats.draw_calls, gStats.triangles, gStats.tri_in, gStats.tri_trivial, gStats.tri_culled,
           gStats.tri_clipped, gStats.ge_verts, span ? (unsigned)(100 - idle * 100 / span) : 0,
           (unsigned)((span - idle) / 120), span ? (unsigned)(gStats.render_us * 100 / span) : 0,
           span ? (unsigned)((uint64_t)gStats.blocked_us * 100 / span) : 0,
           span ? (unsigned)(g_audio_us * 100 / span) : 0, prof, threads);
    gStats.render_us = gStats.render_cpu_us = 0;
    gStats.blocked_us = 0;
    gStats.submit_wait_max = gStats.submit_wait_sum = gStats.submit_wait_n = 0;
    gStats.vblank_wait_us = gStats.captures = gStats.capture_us = 0;
    gStats.target_copies = gStats.target_us = 0;
    gStats.yields = 0;
    g_audio_us = 0;
    sStatStart = now;
    sStatIdleStart = g_idle_us;
    sStatTasks = gStats.tasks;
}

/* Closes the frame the worker has been drawing and shows it. Runs on the
 * worker, in order with the tasks (see gfx_worker.c). */
void gfx_present_frame(uint32_t framebuffer) {
    gStats.frames++;
    note_display_fb(framebuffer);
    uint32_t* shot = gfx_debug_shot_buffer(gStats.frames);
    if (gFrameOpen || shot != NULL) {
        bool drawn = gFrameOpen;
        gfx_open_frame();
        gfx_flush_batch();
        if (drawn && !gTarget.bound) {
            soften_frame();
        }
        if (shot != NULL) {
            sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_SCREEN_W, PSP_SCREEN_H, BUF_WIDTH, VRAM_ADDR(kColorBufs[sDrawBuf]),
                           0, 0, BUF_WIDTH, shot);
        }
        sceGuFinish();
        PROF_BEGIN(PROF_GFX_SYNC);
        rt_sched_native_wait(ge_sync, NULL);
        PROF_END(PROF_GFX_SYNC);
        gFrameOpen = false;
        /* The GE is done with this frame: buffers it might have read are free. */
        gfx_tex_flush_retired();
        if (shot != NULL) {
            gfx_debug_save_shot(gStats.frames);
        }
    }
    /*
     * Hand the finished frame to the display at the next vertical blank (not
     * mid-scan, which tears). If nothing was drawn since the last hand-over,
     * keep showing what is there: there is no new picture to show.
     */
    if (sFrameDrawn) {
        sceDisplaySetFrameBuf(VRAM_ADDR(kColorBufs[sDrawBuf]), BUF_WIDTH, PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_NEXTVSYNC);
        int free_buf = sPrevShownBuf;
        sPrevShownBuf = sShownBuf;
        sShownBuf = sDrawBuf;
        sDrawBuf = free_buf;
        sPrevShownVcount = sShownVcount;
        sShownVcount = sceDisplayGetVcount();
        sFrameDrawn = false;
    }
    update_stretch();
    gfx_tex_new_frame();
    if ((gStats.frames % 120) == 1) {
        log_stats();
    }
    gStats.draw_calls = 0;
    gStats.vertices = 0;
    gStats.triangles = 0;
    gStats.ge_verts = 0;
    gStats.tri_in = gStats.tri_trivial = gStats.tri_culled = gStats.tri_clipped = 0;
}
