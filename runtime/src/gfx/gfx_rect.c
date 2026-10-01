/*
 * gfx_rect.c -- rectangles: fills, texture rectangles and S2DEX2 backgrounds.
 *
 * They are drawn as GE sprites in screen coordinates (through mode). Some
 * copy from one framebuffer to another -- the game saving the screen behind
 * a menu (PreRender), or putting part of it into a render target -- and take
 * what the PSP shows instead (gfx_frame.c).
 */
#include <pspkernel.h>
#include <string.h>

#include "gfx_internal.h"

bool gfx_draw_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                   uint32_t color, const GuTexture* tex, bool flip) {
    gfx_open_frame();
    gfx_flush_batch();
    if (gGu.fog) {
        sceGuDisable(GU_FOG);
        gGu.fog = 0;
    }
    if (gTracing) {
        rt_log("  rect#%u (%.1f,%.1f)-(%.1f,%.1f) col %08X tex %p uv (%.1f %.1f)-(%.1f %.1f) oml %08X omh %06X bl %d at %d",
               gStats.draw_calls, x0, y0, x1, y1, color, tex, u0, v0, u1, v1, gRdp.other_l, gRdp.other_h, gGu.blend, gGu.alpha_test ? gGu.alpha_ref : -1);
    }
    if (!gfx_draw_enabled() || gfx_blender_keeps_memory()) {
        gStats.draw_calls++;
        return false;
    }
    GuVertex* m = sceGuGetMemory(4 * sizeof(GuVertex));
    float px0 = (x0 - gMap.crop_x) * gMap.scale_x, py0 = (y0 - gMap.crop_y) * gMap.scale_y;
    float px1 = (x1 - gMap.crop_x) * gMap.scale_x, py1 = (y1 - gMap.crop_y) * gMap.scale_y;
    int n = 2;
    if (flip) {
        /* Flipped rects swap the s/t axes, which a sprite can't express. */
        n = 4;
        m[0] = (GuVertex){ u0, v0, color, px0, py0, 0 };
        m[1] = (GuVertex){ u0, v1, color, px1, py0, 0 };
        m[2] = (GuVertex){ u1, v0, color, px0, py1, 0 };
        m[3] = (GuVertex){ u1, v1, color, px1, py1, 0 };
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_VTYPE | GU_TRANSFORM_2D, n, NULL, m);
    } else {
        m[0] = (GuVertex){ u0, v0, color, px0, py0, 0 };
        m[1] = (GuVertex){ u1, v1, color, px1, py1, 0 };
        sceGuDrawArray(GU_SPRITES, GU_VTYPE | GU_TRANSFORM_2D, n, NULL, m);
    }
    gStats.draw_calls++;
    return true;
}

/*
 * A rectangle the blender turns into nothing (see blender_keeps_memory in
 * gfx_draw.c) is left out before its texture is even looked up: the pocket
 * screen has sixteen of them a frame, each with a texture read out of the
 * render target it has just drawn, which would have to be copied back for it.
 */
static bool rect_keeps_memory(void) {
    if (!gfx_blender_keeps_memory()) {
        return false;
    }
    if (gTracing) {
        rt_log("  rect#%u left out: the blender keeps the pixel", gStats.draw_calls);
    }
    gfx_target_not_drawn();
    gStats.draw_calls++;
    return true;
}

/* Copy mode draws the texels as they are. */
static void copy_mode_fit(CombinerFit* fit) {
    memset(fit, 0, sizeof(*fit));
    fit->mode = TEX_MODULATE;
    fit->tex_alpha = true;
    fit->uses_texture = true;
    fit->base[0] = fit->base[1] = fit->base[2] = 1.0f;
    fit->abase = 1.0f;
}

static uint32_t fill_color_rgba(void) {
    uint32_t c = gRdp.fill;
    uint32_t r, g, b, a;
    /* 16-bit framebuffer: fill colour is RGBA5551 in the low half. */
    uint16_t v = (uint16_t)c;
    r = (v >> 11) & 31;
    g = (v >> 6) & 31;
    b = (v >> 1) & 31;
    a = 255;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | (a << 24);
}

void gfx_fill_rect(uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x0 = ulx / 4.0f, y0 = uly / 4.0f;
    float x1 = lrx / 4.0f, y1 = lry / 4.0f;
    uint32_t cycle = (gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3;
    if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
        x1 += 1.0f;
        y1 += 1.0f;
    }

    bool full = x0 <= 0 && y0 <= 0 && x1 >= N64_SCREEN_W - 1 && y1 >= N64_SCREEN_H - 1;
    bool to_rt = false;
    if (gRdp.cimg == gRdp.zimg) {
        if (gDisplayZimg != 0 && (gRdp.zimg & 0x1FFFFFFF) != gDisplayZimg) {
            gTarget.pending_zclear = gRdp.zimg & 0x1FFFFFFF; /* a render target's depth buffer */
            return;
        }
        gfx_target_leave();
    } else if (gfx_drawing_to_display()) {
        gfx_select_target();
    } else {
        if (cycle == G_CYC_FILL && gRdp.fill == G_MAXFBZ_FILL) {
            /* A depth buffer cleared as a colour image; the game makes it the depth image next. */
            gTarget.pending_zclear = gRdp.cimg & 0x1FFFFFFF;
            return;
        }
        if (!gfx_select_target()) {
            return;
        }
        to_rt = true;
        full = x0 <= 0 && y0 <= 0 && x1 >= gTarget.width && y1 >= gTarget.height;
    }

    gfx_open_frame();
    gfx_flush_batch();

    if (gRdp.cimg == gRdp.zimg) {
        /* Depth buffer clear */
        sceGuClearDepth(0);
        sceGuClear(GU_DEPTH_BUFFER_BIT);
        return;
    }

    uint32_t color;
    if (cycle == G_CYC_FILL) {
        color = fill_color_rgba();
        if (to_rt) {
            /* The alpha (coverage) bit is the fill colour's; the stencil holds it. */
            uint32_t c = gRdp.fill;
            bool a = gTarget.siz == G_IM_SIZ_32b ? (c & 0xFF) != 0 : (c & 1) != 0;
            if (gTarget.siz == G_IM_SIZ_32b) {
                color = (c >> 24) | ((c >> 16) & 0xFF) << 8 | ((c >> 8) & 0xFF) << 16 | 0xFF000000u;
            }
            if (full) {
                sceGuClearColor(color);
                sceGuClearStencil(a ? 0xFF : 0);
                sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
                if (gfx_target_fill_known(false)) {
                    gfx_target_filled(c, false); /* the fill colour is the pixel, coverage bit and all */
                }
                return;
            }
            sceGuStencilFunc(GU_ALWAYS, a ? 0xFF : 0, 0xFF);
        } else if (full) {
            sceGuClearColor(color);
            sceGuClear(GU_COLOR_BUFFER_BIT);
            return;
        }
    } else {
        CombinerFit fit;
        gHintTri[0] = NULL;
        gfx_classify_combiner(&fit);
        color = vertex_color(&fit, 1, 1, 1, 1);
    }

    gRdp.state_dirty = true;
    sceGuDisable(GU_TEXTURE_2D);
    gGu.texture = 0;
    sceGuDisable(GU_DEPTH_TEST);
    gGu.depth_test = 0;
    if (cycle != G_CYC_FILL) {
        gfx_apply_render_state(false);
    } else if (gGu.blend) {
        sceGuDisable(GU_BLEND);
        gGu.blend = 0;
    }
    if (to_rt && cycle != G_CYC_FILL) {
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP); /* blended over: leave coverage as it is */
    }
    /* One colour over the whole target, neither blended nor tested: every pixel
     * gets that colour and keeps its coverage, which RDRAM can be told directly. */
    bool known = to_rt && full && cycle != G_CYC_FILL && !gGu.blend && !gGu.alpha_test && gfx_target_fill_known(true);
    bool drawn = gfx_draw_rect(x0, y0, x1, y1, 0, 0, 0, 0, color, NULL, false);
    if (to_rt) {
        sceGuStencilFunc(GU_ALWAYS, 0xFF, 0xFF);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    }
    if (known && drawn) {
        gfx_target_filled((color & 0xF8) << 8 | (color & 0xF800) >> 5 | (color & 0xF80000) >> 18, true);
    } else if (!drawn) {
        gfx_target_not_drawn();
    }
}

void gfx_tex_rect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip) {
    uint32_t xh = (w0 >> 12) & 0xFFF, yh = w0 & 0xFFF;
    uint32_t tile_index = (w1 >> 24) & 7;
    uint32_t xl = (w1 >> 12) & 0xFFF, yl = w1 & 0xFFF;
    int16_t s = (int16_t)(w2 >> 16), t = (int16_t)w2;
    int16_t dsdx = (int16_t)(w3 >> 16), dtdy = (int16_t)w3;

    float x0 = xl / 4.0f, y0 = yl / 4.0f;
    float x1 = xh / 4.0f, y1 = yh / 4.0f;
    uint32_t cycle = (gRdp.other_h >> G_MDSFT_CYCLETYPE) & 3;
    if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
        x1 += 1.0f;
        y1 += 1.0f;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (!gfx_drawing_to_display()) {
        TileDesc* src_tile = &gRdp.tiles[tile_index];
        uint32_t src_bits = gfx_tmem_bits(src_tile->tmem);
        uint32_t src = src_bits == TMEM_INVALID ? 0 : src_bits >> 3;
        if (!gfx_select_target()) {
            /* Framebuffer-to-buffer copies (PreRender) with a texrect: capture the screen instead.
               PreRender may use the depth buffer's memory as its save buffer. */
            if (gfx_is_display_fb(src)) {
                gfx_capture_framebuffer(src, gRdp.cimg, gRdp.cimg_width, N64_SCREEN_H);
            }
            return;
        }
        uint32_t fb;
        float sx, sy;
        if (src != 0 && !flip && gfx_screen_position(src, &fb, &sx, &sy)) {
            /* Part of the screen copied into a render target (the portrait's background). */
            float fs = s / 32.0f - src_tile->uls / 4.0f, ft = t / 32.0f - src_tile->ult / 4.0f;
            float dsx = dsdx / 1024.0f, dty = dtdy / 1024.0f;
            if (cycle == G_CYC_COPY) {
                dsx /= 4.0f; /* copy mode steps s by 4 per pixel; t steps normally */
            }
            gfx_copy_from_screen(fb, x0, y0, x1, y1, sx + fs, sy + ft, sx + fs + dsx * (x1 - x0),
                                 sy + ft + dty * (y1 - y0));
            return;
        }
    } else {
        gfx_select_target();
    }
    if (rect_keeps_memory()) {
        return;
    }

    gfx_open_frame();
    gfx_flush_batch();

    uint8_t saved_tile = gRsp.tex_tile;
    gRsp.tex_tile = (uint8_t)tile_index;

    CombinerFit fit;
    bool copy_mode = cycle == G_CYC_COPY;
    if (copy_mode) {
        copy_mode_fit(&fit);
    } else {
        gHintTri[0] = NULL;
        gfx_classify_combiner(&fit);
    }
    gFit = fit;
    gRdp.state_dirty = true;

    const GuTexture* tex = NULL;
    if (fit.uses_texture) {
        tex = gfx_bind_texture(tile_index + fit.tex_tile, &fit);
    }
    gRsp.tex_tile = saved_tile;

    gfx_apply_render_state(false);
    if (copy_mode && gGu.blend) {
        sceGuDisable(GU_BLEND);
        gGu.blend = 0;
    }
    if (!copy_mode) {
        gfx_apply_aa_edge(tex, &fit);
        gBatchTex = tex;
    }
    gfx_gu_texturing(tex != NULL);
    if (tex != NULL) {
        gfx_gu_tex_func(&fit);
    }

    uint32_t color = vertex_color(&fit, 1, 1, 1, 1);
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (tex != NULL) {
        TileDesc* tile = &gRdp.tiles[tile_index];
        float fs = s / 32.0f - tile->uls / 4.0f;
        float ft = t / 32.0f - tile->ult / 4.0f;
        float dsx = dsdx / 1024.0f;
        float dty = dtdy / 1024.0f;
        if (copy_mode) {
            dsx /= 4.0f;
            dty /= 4.0f;
        }
        float w = x1 - x0;
        float h = y1 - y0;
        if (!flip) {
            u0 = fs; u1 = fs + dsx * w;
            v0 = ft; v1 = ft + dty * h;
        } else {
            u0 = fs; u1 = fs + dsx * h;
            v0 = ft; v1 = ft + dty * w;
        }
        /* Through-mode (2D) texture coordinates are in texels, not normalised. */
    }
    /* flipped: swapped axes, s follows y and t follows x */
    gfx_draw_rect(x0, y0, x1, y1, u0, v0, u1, v1, color, tex, flip);
}

/* ---- S2DEX2 backgrounds ------------------------------------------------- */

void gfx_s2dex_bg(uint32_t addr, bool copy) {
    uint8_t raw[40];
    rt_copy_from_rdram(addr, raw, 40);
    #define U16(o) ((uint16_t)((raw[o] << 8) | raw[(o) + 1]))
    uint16_t image_x = U16(0), image_w = U16(2);
    int16_t frame_x = (int16_t)U16(4);
    uint16_t frame_w = U16(6);
    uint16_t image_y = U16(8), image_h = U16(10);
    int16_t frame_y = (int16_t)U16(12);
    uint16_t frame_h = U16(14);
    uint32_t image_ptr = ((uint32_t)raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    uint8_t fmt = raw[22], siz = raw[23];
    uint16_t flip = U16(26);
    uint16_t scale_w = copy ? 1024 : U16(28);
    uint16_t scale_h = copy ? 1024 : U16(30);
    #undef U16
    if (scale_w == 0) scale_w = 1024;
    if (scale_h == 0) scale_h = 1024;
    image_ptr = seg_addr(image_ptr);

    float fx0 = frame_x / 4.0f, fy0 = frame_y / 4.0f;
    float fx1 = fx0 + frame_w / 4.0f, fy1 = fy0 + frame_h / 4.0f;
    float tex_x = image_x / 32.0f, tex_y = image_y / 32.0f;
    float span_w = frame_w / 4.0f * scale_w / 1024.0f;
    float span_h = frame_h / 4.0f * scale_h / 1024.0f;
    uint32_t img_w = image_w / 4, img_h = image_h / 4;

    if (gTracing) {
        rt_log("  bg %s img %08X %ux%u fmt %u siz %u frame (%.1f,%.1f)-(%.1f,%.1f) tex (%.1f,%.1f) span %.1fx%.1f cimg %08X",
               copy ? "copy" : "1cyc", image_ptr, img_w, img_h, fmt, siz, fx0, fy0, fx1, fy1, tex_x, tex_y,
               span_w, span_h, gRdp.cimg);
    }

    if (!gfx_drawing_to_display()) {
        if (!gfx_select_target()) {
            /* PreRender may use the depth buffer's memory as its save buffer. */
            if (gfx_is_display_fb(image_ptr)) {
                gfx_capture_framebuffer(image_ptr, gRdp.cimg, gRdp.cimg_width, N64_SCREEN_H);
            }
            return;
        }
        uint32_t fb;
        float sx, sy;
        if (gfx_screen_position(image_ptr, &fb, &sx, &sy)) {
            gfx_copy_from_screen(fb, fx0, fy0, fx1, fy1, sx + tex_x, sy + tex_y, sx + tex_x + span_w,
                                 sy + tex_y + span_h);
            return;
        }
    } else {
        gfx_select_target();
    }
    if (rect_keeps_memory()) {
        return;
    }

    TexKey key;
    memset(&key, 0, sizeof(key));
    uint32_t xoff = (uint32_t)tex_x, yoff = (uint32_t)tex_y;
    uint32_t bits = kTexelBits[siz & 3];
    key.addr_bits = ((image_ptr & 0xFFFFFF) << 3) + (yoff * img_w + xoff) * bits;
    key.row_bits = img_w * bits;
    key.fmt = fmt;
    key.siz = siz;
    uint32_t w = (uint32_t)(span_w + 0.999f), h = (uint32_t)(span_h + 0.999f);
    if (xoff + w > img_w) w = img_w > xoff ? img_w - xoff : 0;
    if (yoff + h > img_h) h = img_h > yoff ? img_h - yoff : 0;
    if (w > 512) w = 512;
    if (h > 512) h = 512;
    if (w == 0 || h == 0) {
        return;
    }
    key.tile_w = (uint16_t)w;
    key.tile_h = (uint16_t)h;
    key.cms = key.cmt = G_TX_CLAMP;
    if (fmt == G_IM_FMT_CI) {
        uint32_t tlut_type = (gRdp.other_h >> G_MDSFT_TEXTLUT) & 3;
        key.tlut_type = tlut_type == 3 ? TLUT_IA16 : TLUT_RGBA16;
        key.tlut_addr = gRdp.tlut[0] & 0x1FFFFFFF;
    }

    gfx_open_frame();
    gfx_flush_batch();

    CombinerFit fit;
    if (copy) {
        copy_mode_fit(&fit);
    } else {
        gHintTri[0] = NULL;
        gfx_classify_combiner(&fit);
    }
    gFit = fit;
    gRdp.state_dirty = true;

    const GuTexture* tex = gfx_tex_get(&key);
    if (tex == NULL) {
        return;
    }
    gfx_gu_texture_image(tex);
    if (gGu.wrap_u != GU_CLAMP || gGu.wrap_v != GU_CLAMP) {
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        gGu.wrap_u = gGu.wrap_v = GU_CLAMP;
    }
    int filter = (!copy && scale_w != 1024) ? GU_LINEAR : GU_NEAREST;
    if (filter != gGu.tex_filter) {
        sceGuTexFilter(filter, filter);
        gGu.tex_filter = filter;
    }
    gfx_apply_render_state(false);
    if (copy && gGu.blend) {
        sceGuDisable(GU_BLEND);
        gGu.blend = 0;
    }
    if (!copy) {
        gfx_apply_aa_edge(tex, &fit);
    }
    gfx_gu_texturing(true);
    gfx_gu_tex_func(&fit);

    float u0 = 0, u1 = span_w, v0 = 0, v1 = span_h;
    if (flip & 0x01) { float t = u0; u0 = u1; u1 = t; }
    if (flip & 0x10) { float t = v0; v0 = v1; v1 = t; }
    gfx_draw_rect(fx0, fy0, fx1, fy1, u0, v0, u1, v1, vertex_color(&fit, 1, 1, 1, 1), tex, false);
}
