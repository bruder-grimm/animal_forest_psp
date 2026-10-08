/*
 * gfx_frame.c -- frames on the GE: colour buffers, projection, the viewport,
 * the game's copies of the screen, the softening pass and presenting.
 *
 * The N64 game draws into framebuffers in RDRAM and tells the VI which one to
 * show. Here every frame is drawn straight into one of three colour buffers
 * in VRAM at the PSP's resolution (N64 pixels are scaled by SCALE_X/Y), and
 * osViSwapBuffer (gfx_present_frame) puts it on screen. RDRAM framebuffers are
 * only tracked by address: when the game copies from one, the copy is made
 * from what the PSP shows (gfx_capture_framebuffer, gfx_copy_from_screen).
 * Smaller pictures of the game's own are drawn on the GE too (gfx_target.c).
 */
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>

#include "gfx_internal.h"

/*
 * The display list, in cache lines of its own. pspgu writes it through the
 * uncached mirror; a variable sharing its first line and written the usual way
 * (gFrameOpen did, as the linker happened to lay things out) leaves that line
 * dirty in the cache with an old copy of the list's first commands -- the ones
 * that name the colour buffer to draw into -- and whenever it was written back
 * between sceGuStart and the GE getting there, the frame's first half went to
 * the buffer of a frame before. Seen on the PSP only (the pocket screen
 * flickered: its second half is drawn after another buffer command).
 */
static unsigned int sGuList[256 * 1024] __attribute__((aligned(64)));
_Static_assert(sizeof(sGuList) % 64 == 0, "the display list must end on a cache line");
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

/* The N64 viewport on the GE's target, in whole pixels: a vertex lands at
 * (*cx + ndc x * *w / 2, *cy - ndc y * *h / 2). */
void gfx_ge_viewport(int* cx, int* cy, int* w, int* h) {
    float hw = gRsp.vscale[0] / 4.0f * gMap.scale_x;
    float hh = gRsp.vscale[1] / 4.0f * gMap.scale_y;
    *cx = (int)((gRsp.vtrans[0] / 4.0f - gMap.crop_x) * gMap.scale_x + 0.5f);
    *cy = (int)((gRsp.vtrans[1] / 4.0f - gMap.crop_y) * gMap.scale_y + 0.5f);
    *w = (int)(hw * 2 + 0.5f);
    *h = (int)(hh * 2 + 0.5f);
}

void gfx_set_viewport(void) {
    int cx, cy, w, h;
    gfx_update_snap();
    gfx_ge_viewport(&cx, &cy, &w, &h);
    sceGuViewport(GU_OFFSET_X + cx, GU_OFFSET_Y + cy, w, h);
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
 * (VRAM layout: gfx_internal.h)
 */
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

/* Points the GE at the screen's colour and depth buffers again (from a render target). */
void gfx_draw_to_screen(void) {
    map_screen();
    sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    sceGuDepthBuffer((void*)DEPTH_VRAM, BUF_WIDTH);
    sceGuDisable(GU_STENCIL_TEST);
    gGu.stencil = GU_STENCIL_OFF;
    gfx_set_viewport();
    gfx_set_scissor();
}

/*
 * Waits for the GE to finish the display list so far, and goes on with a new
 * one: the GE's state stays as it is, and the vertex arena is free again.
 */
void gfx_finish_list(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuStart(GU_DIRECT, sGuList);
    /* (pspgu opens every list with the colour buffer of sceGuDrawBuffer, which isn't ours) */
    if (gTarget.bound) {
        sceGuDrawBufferList(GU_PSM_8888, (void*)TARGET_VRAM, TARGET_MAX);
    } else {
        sceGuDrawBufferList(GU_PSM_8888, (void*)kColorBufs[sDrawBuf], BUF_WIDTH);
    }
    gfx_batches_new_frame();
}

void rt_gfx_init(void) {
    if (sGuReady) {
        return;
    }
    sGuReady = true;
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
static uint32_t sDrawingFb = 0;   /* the screen colour image of the frame being drawn */
uint32_t gDisplayZimg = 0;

void gfx_screen_images_drawn(void) {
    gDisplayZimg = gRdp.zimg & 0x1FFFFFFF;
    sDrawingFb = gRdp.cimg & 0x1FFFFFFF;
}

/* The colour buffer that shows framebuffer fb: the frame being drawn is in the draw buffer, a finished one on screen. */
static bool fb_in_draw_buffer(uint32_t fb) {
    fb &= 0x1FFFFFFF;
    return fb == sDrawingFb || fb != sLastShownFb;
}

static uint32_t fb_vram(uint32_t fb) {
    return kColorBufs[fb_in_draw_buffer(fb) ? sDrawBuf : sShownBuf];
}

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

/*
 * Which colour images are the screen is learned from the frames presented
 * (until the first, every one is): a game resumed from a capture would draw
 * its first frame's render targets on the screen without it.
 */
void gfx_frame_capture(RtCapture* c) {
    struct {
        uint32_t display_fbs[4];
        int32_t num_display_fbs;
        uint32_t last_shown, drawing, display_zimg;
    } s;
    if (rt_cap_saving(c)) {
        memcpy(s.display_fbs, sDisplayFbs, sizeof(s.display_fbs));
        s.num_display_fbs = sNumDisplayFbs;
        s.last_shown = sLastShownFb;
        s.drawing = sDrawingFb;
        s.display_zimg = gDisplayZimg;
    }
    rt_cap_io(c, "GFXF", &s, sizeof(s));
    if (!rt_cap_saving(c)) {
        memcpy(sDisplayFbs, s.display_fbs, sizeof(sDisplayFbs));
        sNumDisplayFbs = s.num_display_fbs;
        sLastShownFb = s.last_shown;
        sDrawingFb = s.drawing;
        gDisplayZimg = s.display_zimg;
        gScreenSelected = false;
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
        /* whole cache lines: the invalidate after the GE has written it must not drop a neighbour's */
        sCapturePixels = memalign(64, BUF_WIDTH * PSP_SCREEN_H * 4);
        if (sCapturePixels == NULL) {
            RT_LOG_ONCE("gfx: no memory for framebuffer captures");
            return;
        }
    }
    uint32_t vram = fb_vram(src);
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
            MEM_HU(0, dst + 2 * (y * width + x)) = (uint16_t)(ge_to_rgba5551(sCapturePixels[sy * BUF_WIDTH + sx]) | 1);
        }
    }
    /* A texture made from the last picture saved there (the screen behind a menu) is out of date. */
    gfx_tex_invalidate_range(dst, width * height * 2);
    if (gTracing) {
        rt_log("  capture fb %08X (%s) -> %08X %ux%u", src, fb_in_draw_buffer(src) ? "back" : "front", dst, width, height);
    }
}

/*
 * The game saving a frame's coverage (PreRender, before a menu opens over the
 * frozen screen): the framebuffer, turned into coverage values by a fill, is
 * drawn into its own memory as an 8-bit image, a byte a pixel, for the game's
 * edge filter on the CPU to read. The port's picture has no N64 coverage, so
 * the fill is left out (blender_keeps_memory) and every pixel counts as fully
 * covered: that is what the image gets, and the filter leaves the saved picture
 * as it is. Drawn on the screen like any rectangle, the image covered the frame
 * with whatever the framebuffer's memory held, and a present that was still on
 * its way showed that.
 *
 * True if the rectangle (in pixels of the colour image) was such a draw.
 */
bool gfx_coverage_rect(float x0, float y0, float x1, float y1) {
    if (gRdp.cimg_siz != G_IM_SIZ_8b || !gfx_drawing_to_display()) {
        return false;
    }
    if (gfx_is_display_fb(gRdp.cimg)) {
        uint32_t width = gRdp.cimg_width < N64_SCREEN_W ? gRdp.cimg_width : N64_SCREEN_W;
        uint32_t left = x0 > 0 ? (uint32_t)x0 : 0, right = x1 < (float)width ? (uint32_t)x1 : width;
        uint32_t top = y0 > 0 ? (uint32_t)y0 : 0, bottom = y1 < N64_SCREEN_H ? (uint32_t)y1 : N64_SCREEN_H;
        uint8_t* rdram = g_rdram;
        for (uint32_t y = top; y < bottom; y++) {
            for (uint32_t x = left; x < right; x++) {
                MEM_BU(0, gRdp.cimg + y * gRdp.cimg_width + x) = 0xFF;
            }
        }
    }
    return true;
}

/*
 * A texture rectangle or background whose source is the screen, drawn into a
 * render target: sample the PSP's colour buffer. dst is in target pixels, src
 * in N64 screen pixels.
 */
void gfx_copy_from_screen(uint32_t fb, float dx0, float dy0, float dx1, float dy1, float sx0, float sy0,
                          float sx1, float sy1) {
    /* The rows this covers of a target still to be loaded needn't be; anywhere else, what it is drawn
     * over has to be there first. */
    uint32_t solid = gTarget.solid_rows;
    if (gfx_draw_enabled()) {
        gfx_target_covered(dx0, dy0, dx1, dy1);
    }
    if (gTarget.solid_rows == solid) {
        gfx_target_ready();
    }
    gfx_flush_batch();
    uint32_t vram = fb_vram(fb);
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
 * with a blurred copy of itself, by the strength in soften.txt (percent; 0
 * turns it off, SOFTEN_DEFAULT without the file).
 *
 * The blur is the frame shrunk to half size with bilinear filtering (each
 * texel the average of 2x2 pixels) and drawn back over it enlarged, bilinear
 * again, with the strength as a fixed blend weight: two sprites, neither
 * reading the buffer it writes. The half-size copy lives in the render
 * target's VRAM and the spare VRAM after it (256x136x4 from TARGET_VRAM, ending
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
#define SOFT_VRAM TARGET_VRAM /* 256 x 136 x 4 = 0x22000: ends at 0x1FE000 */
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
        uint32_t pct = SOFTEN_DEFAULT;
        if (rt_load_number_list("soften.txt", &pct, 1) == 1 && pct > 100) {
            pct = 100;
        }
        sSoften = (int)(pct * 255 / 100);
        rt_log("soften: %u%%", (unsigned)pct);
    }
    if (sSoften == 0) {
        return;
    }
    soften_passes();
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
    rt_log("frame %u (poll %u): %u gfx tasks, %u yields, %u verts, %u draws, %u tris (in %u, trivial %u, culled %u, clipped %u, %u GE verts) | busy %u%% = %u us a frame (gfx %u%%, blocked %u%%, audio %u%%) |%s",
           gStats.frames, (unsigned)rt_input_polls(), gStats.tasks, (unsigned)gStats.yields, gStats.vertices,
           gStats.draw_calls, gStats.triangles, gStats.tri_in, gStats.tri_trivial, gStats.tri_culled,
           gStats.tri_clipped, gStats.ge_verts, span ? (unsigned)(100 - idle * 100 / span) : 0,
           (unsigned)((span - idle) / 120), span ? (unsigned)(gStats.render_us * 100 / span) : 0,
           span ? (unsigned)((uint64_t)gStats.blocked_us * 100 / span) : 0,
           span ? (unsigned)(g_audio_us * 100 / span) : 0, threads);
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
    uint32_t* shot = gfx_debug_shot_buffer(gStats.frames, gFrameOpen);
    if (gFrameOpen) {
        gfx_flush_batch();
        if (!gTarget.bound) {
            soften_frame();
        }
        if (shot != NULL) {
            sceGuCopyImage(GU_PSM_8888, 0, 0, PSP_SCREEN_W, PSP_SCREEN_H, BUF_WIDTH, VRAM_ADDR(kColorBufs[sDrawBuf]),
                           0, 0, BUF_WIDTH, shot);
        }
        sceGuFinish();
        rt_sched_native_wait(ge_sync, NULL);
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
