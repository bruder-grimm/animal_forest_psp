/*
 * gfx_dl.c -- the display list interpreter: the RSP's F3DZEX2 and S2DEX2
 * microcode and the RDP's commands, as far as Animal Forest uses them.
 *
 * Commands update the RSP and RDP state (gRsp, gRdp) and hand geometry to the
 * vertex stage (gfx_vertex.c), triangles to gfx_draw.c and rectangles to
 * gfx_rect.c. Texture loads are not copied anywhere: texture memory records
 * where in RDRAM each unit was loaded from, and the texture cache reads the
 * texels from there (gfx_tex.c).
 */
#include <pspkernel.h>
#include <string.h>

#include "gfx_internal.h"
#include "prof.h"

/* The S2DEX2 microcode's text in RDRAM: G_LOAD_UCODE with this switches to it. */
#define UCODE_S2DEX2_TEXT 0x800FF370u

/* F3DZEX2 */
#define G_NOOP 0x00
#define G_VTX 0x01
#define G_MODIFYVTX 0x02
#define G_CULLDL 0x03
#define G_BRANCH_Z 0x04
#define G_TRI1 0x05
#define G_TRI2 0x06
#define G_QUAD 0x07
#define G_LINE3D 0x08
#define G_SPECIAL_3 0xD3
#define G_SPECIAL_2 0xD4
#define G_SPECIAL_1 0xD5
#define G_DMA_IO 0xD6
#define G_TEXTURE 0xD7
#define G_POPMTX 0xD8
#define G_GEOMETRYMODE 0xD9
#define G_MTX 0xDA
#define G_MOVEWORD 0xDB
#define G_MOVEMEM 0xDC
#define G_LOAD_UCODE 0xDD
#define G_DL 0xDE
#define G_ENDDL 0xDF
#define G_SPNOOP 0xE0
#define G_RDPHALF_1 0xE1
#define G_SETOTHERMODE_L 0xE2
#define G_SETOTHERMODE_H 0xE3
#define G_TEXRECT 0xE4
#define G_TEXRECTFLIP 0xE5
#define G_RDPLOADSYNC 0xE6
#define G_RDPPIPESYNC 0xE7
#define G_RDPTILESYNC 0xE8
#define G_RDPFULLSYNC 0xE9
#define G_SETKEYGB 0xEA
#define G_SETKEYR 0xEB
#define G_SETCONVERT 0xEC
#define G_SETSCISSOR 0xED
#define G_SETPRIMDEPTH 0xEE
#define G_RDPSETOTHERMODE 0xEF
#define G_LOADTLUT 0xF0
#define G_RDPHALF_2 0xF1
#define G_SETTILESIZE 0xF2
#define G_LOADBLOCK 0xF3
#define G_LOADTILE 0xF4
#define G_SETTILE 0xF5
#define G_FILLRECT 0xF6
#define G_SETFILLCOLOR 0xF7
#define G_SETFOGCOLOR 0xF8
#define G_SETBLENDCOLOR 0xF9
#define G_SETPRIMCOLOR 0xFA
#define G_SETENVCOLOR 0xFB
#define G_SETCOMBINE 0xFC
#define G_SETTIMG 0xFD
#define G_SETZIMG 0xFE
#define G_SETCIMG 0xFF

/* S2DEX2 */
#define G_OBJ_RECTANGLE 0x01
#define G_OBJ_SPRITE 0x02
#define G_SELECT_DL 0x04
#define G_OBJ_LOADTXTR 0x05
#define G_OBJ_LDTX_SPRITE 0x06
#define G_OBJ_LDTX_RECT 0x07
#define G_OBJ_LDTX_RECT_R 0x08
#define G_BG_1CYC 0x09
#define G_BG_COPY 0x0A
#define G_OBJ_RENDERMODE 0x0B
#define G_OBJ_RECTANGLE_R 0xDA
#define G_OBJ_MOVEMEM 0xDC

#define MAX_DL_DEPTH 32

RspState gRsp;
RdpState gRdp;

static inline uint32_t rd_w32_fast(uint8_t* rdram, uint32_t addr) {
    return (uint32_t)MEM_W(0, addr);
}

/* ---- RSP state ---------------------------------------------------------- */

/*
 * An N64 Mtx: sixteen 16.16 fixed-point numbers, their integer halves first
 * (32 bytes), then their fractions. Each RDRAM word holds two elements, the
 * first in its high half. The RSP's DMA ignores the address' low three bits.
 */
static void read_matrix(uint32_t addr, Mat4 out) {
    float* m = &out[0][0];
    addr &= ~7u;
    for (int k = 0; k < 8; k++) {
        uint32_t ip = rd_w32(addr + 4 * k);
        uint32_t fp = rd_w32(addr + 32 + 4 * k);
        m[2 * k] = (float)(int32_t)((ip & 0xFFFF0000u) | (fp >> 16)) / 65536.0f;
        m[2 * k + 1] = (float)(int32_t)((ip << 16) | (fp & 0xFFFFu)) / 65536.0f;
    }
}

static void reset_rsp(void) {
    memset(gRsp.segments, 0, sizeof(gRsp.segments));
    gRsp.mv_depth = 0;
    mat_identity(gRsp.mv_stack[0]);
    mat_identity(gRsp.proj);
    gRsp.mvp_dirty = true;
    gRsp.lights_dirty = true;
    gRsp.num_lights = 0;
    gRsp.geometry_mode = 0;
    gRsp.tex_on = false;
    gRsp.pending_branch = 0;
    gfx_weld_reset();
    gfx_update_snap();
}

static void handle_moveword(uint32_t w0, uint32_t w1) {
    uint32_t index = (w0 >> 16) & 0xFF;
    uint32_t offset = w0 & 0xFFFF;
    switch (index) {
        case 0x02: /* number of lights */
            gRsp.num_lights = (int)(w1 / 24);
            if (gRsp.num_lights > MAX_LIGHTS) gRsp.num_lights = MAX_LIGHTS;
            gRsp.lights_dirty = true;
            break;
        case 0x06: /* segment */
            gRsp.segments[(offset / 4) & 0xF] = w1 & 0x1FFFFFFF;
            break;
        case 0x08: /* fog */
            gRsp.fog_mul = (int16_t)(w1 >> 16);
            gRsp.fog_ofs = (int16_t)w1;
            break;
        default:
            break;
    }
}

static void read_light(uint32_t addr, Light* light) {
    uint8_t raw[16];
    rt_copy_from_rdram(addr, raw, 16);
    light->col[0] = raw[0] / 255.0f;
    light->col[1] = raw[1] / 255.0f;
    light->col[2] = raw[2] / 255.0f;
    light->kc = raw[3];
    light->point = raw[3] != 0;
    light->dir[0] = (int8_t)raw[8];
    light->dir[1] = (int8_t)raw[9];
    light->dir[2] = (int8_t)raw[10];
    normalize3(light->dir);
    light->pos[0] = (int16_t)((raw[8] << 8) | raw[9]);
    light->pos[1] = (int16_t)((raw[10] << 8) | raw[11]);
    light->pos[2] = (int16_t)((raw[12] << 8) | raw[13]);
    light->kl = raw[7];
    light->kq = raw[14];
}

static void handle_movemem(uint32_t w0, uint32_t w1) {
    uint32_t index = w0 & 0xFF;
    uint32_t offset = ((w0 >> 8) & 0xFF) * 8;
    uint32_t addr = seg_addr(w1);
    switch (index) {
        case 8: { /* viewport */
            uint8_t raw[16];
            rt_copy_from_rdram(addr, raw, 16);
            for (int i = 0; i < 4; i++) {
                gRsp.vscale[i] = (int16_t)((raw[i * 2] << 8) | raw[i * 2 + 1]);
                gRsp.vtrans[i] = (int16_t)((raw[8 + i * 2] << 8) | raw[8 + i * 2 + 1]);
            }
            if (gFrameOpen) {
                gfx_flush_batch();
                gfx_set_viewport();
            }
            break;
        }
        case 10: { /* lights and lookat */
            if (offset < 48) {
                Light tmp;
                read_light(addr, &tmp);
                memcpy(gRsp.lookat[offset / 24], tmp.dir, sizeof(tmp.dir));
            } else {
                int n = (int)(offset / 24) - 2;
                if (n >= 0 && n <= MAX_LIGHTS) {
                    read_light(addr, &gRsp.lights[n]);
                }
            }
            gRsp.lights_dirty = true;
            break;
        }
        case 2: /* modelview (force matrix) */
            read_matrix(addr, gRsp.mv_stack[gRsp.mv_depth]);
            gRsp.mvp_dirty = true;
            gRsp.lights_dirty = true;
            break;
        default:
            RT_LOG_ONCE("G_MOVEMEM index %u unsupported", index);
            break;
    }
}

static void handle_matrix(uint32_t w0, uint32_t w1) {
    PROF_BEGIN(PROF_GFX_MTX);
    uint32_t params = (w0 & 0xFF) ^ 0x01; /* F3DEX2 stores G_MTX_PUSH inverted */
    Mat4 m __attribute__((aligned(16)));
    read_matrix(seg_addr(w1), m);
    if (params & 0x04) {
        if (params & 0x02) {
            memcpy(gRsp.proj, m, sizeof(Mat4));
        } else {
            mat_mul(gRsp.proj, m, gRsp.proj);
        }
        if (gFrameOpen) {
            gfx_flush_batch();
            gfx_set_projection();
        }
        gfx_mark_dirty(); /* fog range depends on the projection */
    } else {
        if ((params & 0x01) && gRsp.mv_depth < MAX_MATRIX_STACK - 1) {
            memcpy(gRsp.mv_stack[gRsp.mv_depth + 1], gRsp.mv_stack[gRsp.mv_depth], sizeof(Mat4));
            gRsp.mv_depth++;
        }
        if (params & 0x02) {
            memcpy(gRsp.mv_stack[gRsp.mv_depth], m, sizeof(Mat4));
        } else {
            mat_mul(gRsp.mv_stack[gRsp.mv_depth], m, gRsp.mv_stack[gRsp.mv_depth]);
        }
        gRsp.lights_dirty = true;
    }
    gRsp.mvp_dirty = true;
    PROF_END(PROF_GFX_MTX);
}

static void set_other_mode(bool high, uint32_t w0, uint32_t w1) {
    uint32_t len = (w0 & 0xFF) + 1;
    uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
    uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    if (high) {
        gRdp.other_h = (gRdp.other_h & ~mask) | (w1 & mask);
    } else {
        gRdp.other_l = (gRdp.other_l & ~mask) | (w1 & mask);
    }
    gfx_other_mode_changed();
}

/* ---- texture memory ----------------------------------------------------- */

/* Where load `l` put unit `unit`, which it covers. */
static void load_unit(const TmemLoad* l, uint32_t unit, uint32_t* bits, bool* swapped) {
    uint32_t i = unit - l->first;
    if (l->line == 0) {
        /* G_LOADBLOCK: one contiguous block. Each 8-byte step is on load row
         * step * dxt; odd rows are stored swapped. */
        *bits = l->bits + i * 64;
        *swapped = ((i * l->step) >> 11) & 1;
    } else {
        /* G_LOADTILE: a sub-rectangle of the texture image, `line` units per row. */
        uint32_t row = i / l->line;
        *bits = l->bits + row * l->step + (i % l->line) * 64;
        *swapped = row & 1;
    }
}

static void tmem_lookup(uint32_t unit, uint32_t* bits, bool* swapped) {
    for (int i = gRdp.num_tmem_loads - 1; i >= 0; i--) {
        const TmemLoad* l = &gRdp.tmem_loads[i];
        if (unit - l->first < l->count) {
            load_unit(l, unit, bits, swapped);
            return;
        }
    }
    *bits = gRdp.tmem_bits[unit];
    *swapped = gRdp.tmem_swap[unit] != 0;
}

uint32_t gfx_tmem_bits(uint32_t unit) {
    uint32_t bits;
    bool swapped;
    tmem_lookup(unit & 0x1FF, &bits, &swapped);
    return bits;
}

bool gfx_tmem_swapped(uint32_t unit) {
    uint32_t bits;
    bool swapped;
    tmem_lookup(unit & 0x1FF, &bits, &swapped);
    return swapped;
}

void gfx_tmem_reset(void) {
    memset(gRdp.tmem_bits, 0xFF, sizeof(gRdp.tmem_bits));
    memset(gRdp.tmem_swap, 0, sizeof(gRdp.tmem_swap));
    gRdp.num_tmem_loads = 0;
}

/* Notes a load of `count` units from unit `first` on (clipped to texture memory). */
static void tmem_load(uint32_t first, uint32_t count, uint32_t line, uint32_t bits, uint32_t step) {
    if (count > 512 - first) {
        count = 512 - first;
    }
    if (count == 0) {
        return;
    }
    /* Loads it covers completely can never be seen again. */
    int n = 0;
    for (int i = 0; i < gRdp.num_tmem_loads; i++) {
        const TmemLoad* l = &gRdp.tmem_loads[i];
        if (l->first < first || l->first + l->count > first + count) {
            gRdp.tmem_loads[n++] = *l;
        }
    }
    if (n == TMEM_LOADS) {
        /* No room: the oldest goes into the tables, which hold what came before it. */
        const TmemLoad* l = &gRdp.tmem_loads[0];
        for (uint32_t u = l->first; u < (uint32_t)l->first + l->count; u++) {
            bool swapped;
            load_unit(l, u, &gRdp.tmem_bits[u], &swapped);
            gRdp.tmem_swap[u] = swapped;
        }
        memmove(&gRdp.tmem_loads[0], &gRdp.tmem_loads[1], (TMEM_LOADS - 1) * sizeof(TmemLoad));
        n--;
    }
    gRdp.tmem_loads[n] = (TmemLoad){ (uint16_t)first, (uint16_t)count, (uint16_t)line, bits, step };
    gRdp.num_tmem_loads = n + 1;
}

/* ---- the interpreter ---------------------------------------------------- */

static void run_dl(uint32_t dl) {
    uint32_t stack[MAX_DL_DEPTH];
    int depth = 0;
    uint32_t pc = seg_addr(dl);
    uint8_t* rdram = g_rdram;
    int budget = 200000;

    while (budget-- > 0) {
        uint32_t w0 = rd_w32_fast(rdram, pc);
        uint32_t w1 = rd_w32_fast(rdram, pc + 4);
        uint32_t op = w0 >> 24;
        pc += 8;

        if (gRsp.s2dex) {
            bool handled = true;
            switch (op) {
                case G_BG_COPY:
                    gfx_s2dex_bg(seg_addr(w1), true);
                    break;
                case G_BG_1CYC:
                    gfx_s2dex_bg(seg_addr(w1), false);
                    break;
                case G_OBJ_RENDERMODE:
                    break;
                case G_OBJ_RECTANGLE:
                case G_OBJ_SPRITE:
                case G_OBJ_LOADTXTR:
                case G_OBJ_LDTX_SPRITE:
                case G_OBJ_LDTX_RECT:
                case G_OBJ_LDTX_RECT_R:
                case G_OBJ_RECTANGLE_R:
                case G_OBJ_MOVEMEM:
                case G_SELECT_DL:
                    RT_LOG_ONCE("S2DEX object command %02X not implemented", op);
                    break;
                default:
                    handled = false;
                    break;
            }
            if (handled) {
                continue;
            }
        }

        switch (op) {
            case G_NOOP:
            case G_SPNOOP:
            case G_RDPLOADSYNC:
            case G_RDPPIPESYNC:
            case G_RDPTILESYNC:
            case G_RDPFULLSYNC:
            case G_SETKEYGB:
            case G_SETKEYR:
            case G_SETCONVERT:
            case G_SPECIAL_1:
            case G_SPECIAL_2:
            case G_SPECIAL_3:
            case G_DMA_IO:
            case G_RDPHALF_2:
                break;
            case G_LOAD_UCODE:
                gRsp.s2dex = (w1 & 0x1FFFFFFF) == (UCODE_S2DEX2_TEXT & 0x1FFFFFFF);
                break;

            case G_VTX: {
                int n = (w0 >> 12) & 0xFF;
                int end = (w0 & 0xFF) >> 1;
                PROF_BEGIN(PROF_GFX_VTX);
                gfx_process_vertices(seg_addr(w1), end - n, n);
                PROF_END(PROF_GFX_VTX);
                gStats.vertices += n;
                {
                    PROF_BEGIN(PROF_EMPTY);
                    PROF_END(PROF_EMPTY);
                }
                break;
            }
            case G_MODIFYVTX: {
                int where = (w0 >> 16) & 0xFF;
                int vtx_index = ((w0 & 0xFFFF) >> 1) & 0x3F;
                gfx_forget_packed(vtx_index, 1);
                RspVertex* v = &gRsp.verts[vtx_index];
                switch (where) {
                    case 0x10:
                        v->r = ((w1 >> 24) & 0xFF) / 255.0f;
                        v->g = ((w1 >> 16) & 0xFF) / 255.0f;
                        v->b = ((w1 >> 8) & 0xFF) / 255.0f;
                        v->a = (w1 & 0xFF) / 255.0f;
                        break;
                    case 0x14:
                        v->u = (int16_t)(w1 >> 16) * (gRsp.tex_scale_s / 65536.0f);
                        v->v = (int16_t)w1 * (gRsp.tex_scale_t / 65536.0f);
                        break;
                    default:
                        RT_LOG_ONCE("G_MODIFYVTX where=%02X unsupported", where);
                        break;
                }
                break;
            }
            case G_CULLDL: {
                int first = ((w0 & 0xFFFF) >> 1) & 0x3F;
                int last = ((w1 & 0xFFFF) >> 1) & 0x3F;
                uint16_t all = 0xFFFF;
                for (int i = first; i <= last; i++) {
                    all &= gRsp.verts[i].clip;
                }
                if (all & CLIP_OUTSIDE) {
                    goto end_dl;
                }
                break;
            }
            case G_BRANCH_Z: {
                RspVertex* v = &gRsp.verts[((w0 & 0xFFF) >> 1) & 0x3F];
                float zs = v->cw != 0 ? (v->cz / v->cw) * gRsp.vscale[2] + gRsp.vtrans[2] : 0;
                if (zs * 65536.0f <= (float)w1 || v->cw <= 0) {
                    pc = seg_addr(gRsp.pending_branch);
                }
                break;
            }
            case G_TRI1:
                gfx_draw_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                break;
            case G_TRI2:
            case G_QUAD:
                gfx_draw_triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                gfx_draw_triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case G_LINE3D:
                break;

            case G_TEXTURE:
                gRsp.tex_scale_s = (uint16_t)(w1 >> 16);
                gRsp.tex_scale_t = (uint16_t)w1;
                gRsp.tex_tile = (w0 >> 8) & 7;
                gRsp.tex_on = ((w0 >> 1) & 0x7F) != 0 || (w0 & 1);
                gfx_mark_dirty();
                break;
            case G_POPMTX: {
                uint32_t count = w1 / 64;
                if (count > (uint32_t)gRsp.mv_depth) count = (uint32_t)gRsp.mv_depth;
                gRsp.mv_depth -= (int)count;
                gRsp.mvp_dirty = true;
                gRsp.lights_dirty = true;
                break;
            }
            case G_GEOMETRYMODE: {
                uint32_t prev = gRsp.geometry_mode;
                gRsp.geometry_mode = (gRsp.geometry_mode & (w0 & 0xFFFFFF)) | w1;
                if (prev != gRsp.geometry_mode) {
                    gfx_mark_dirty();
                }
                break;
            }
            case G_MTX:
                handle_matrix(w0, w1);
                break;
            case G_MOVEWORD:
                handle_moveword(w0, w1);
                break;
            case G_MOVEMEM:
                handle_movemem(w0, w1);
                break;
            case G_DL:
                if (((w0 >> 16) & 0xFF) == 0) {
                    if (depth >= MAX_DL_DEPTH) {
                        RT_LOG_ONCE("display list stack overflow");
                        return;
                    }
                    stack[depth++] = pc;
                }
                pc = seg_addr(w1);
                break;
            case G_ENDDL:
            end_dl:
                if (depth == 0) {
                    return;
                }
                pc = stack[--depth];
                break;
            case G_RDPHALF_1:
                gRsp.pending_branch = w1;
                break;
            case G_SETOTHERMODE_L:
                set_other_mode(false, w0, w1);
                break;
            case G_SETOTHERMODE_H:
                set_other_mode(true, w0, w1);
                break;
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                uint32_t w2 = rd_w32_fast(rdram, pc + 4);
                uint32_t w3 = rd_w32_fast(rdram, pc + 12);
                pc += 16;
                gfx_tex_rect(w0, w1, w2, w3, op == G_TEXRECTFLIP);
                break;
            }
            case G_SETSCISSOR:
                gRdp.scissor[0] = (w0 >> 12) & 0xFFF;
                gRdp.scissor[1] = w0 & 0xFFF;
                gRdp.scissor[2] = (w1 >> 12) & 0xFFF;
                gRdp.scissor[3] = w1 & 0xFFF;
                if (gFrameOpen) {
                    gfx_flush_batch();
                    gfx_set_scissor();
                }
                break;
            case G_SETPRIMDEPTH:
                gRdp.prim_depth = (uint16_t)(w1 >> 16);
                break;
            case G_RDPSETOTHERMODE:
                gRdp.other_h = w0 & 0xFFFFFF;
                gRdp.other_l = w1;
                gfx_other_mode_changed();
                break;
            case G_LOADTLUT: {
                uint32_t tile = (w1 >> 24) & 7;
                uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
                uint32_t start = gRdp.tiles[tile].tmem;
                start = start >= 0x100 ? start - 0x100 : 0;
                for (uint32_t i = 0; i < count && start + i < 256; i++) {
                    gRdp.tlut[start + i] = gRdp.timg + i * 2;
                }
                gfx_mark_dirty();
                break;
            }
            case G_SETTILESIZE: {
                TileDesc* t = &gRdp.tiles[(w1 >> 24) & 7];
                t->uls = (w0 >> 12) & 0xFFF;
                t->ult = w0 & 0xFFF;
                t->lrs = (w1 >> 12) & 0xFFF;
                t->lrt = w1 & 0xFFF;
                gfx_mark_dirty();
                break;
            }
            case G_LOADBLOCK: {
                /* Copies texels from the texture image into texture memory
                 * at the load tile's tmem, as one contiguous block. */
                TileDesc* t = &gRdp.tiles[(w1 >> 24) & 7];
                uint32_t texels = ((w1 >> 12) & 0xFFF) + 1;
                uint32_t dxt = w1 & 0xFFF;
                uint32_t units = (texels * kTexelBits[t->siz & 3] / 8 + 7) / 8;
                tmem_load(t->tmem, units, 0, (gRdp.timg & 0xFFFFFF) << 3, dxt);
                gfx_mark_dirty();
                break;
            }
            case G_LOADTILE: {
                /* Copies a sub-rectangle of the texture image, one row per
                 * `line` units. */
                TileDesc* t = &gRdp.tiles[(w1 >> 24) & 7];
                uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
                uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
                uint32_t bits = kTexelBits[t->siz & 3];
                uint32_t line = t->line ? t->line : ((lrs - uls + 1) * bits + 63) / 64;
                uint32_t base = (gRdp.timg & 0xFFFFFF) << 3;
                if (lrt >= ult && line != 0) {
                    tmem_load(t->tmem, (lrt - ult + 1) * line, line, base + (ult * gRdp.timg_width + uls) * bits,
                              gRdp.timg_width * bits);
                }
                gfx_mark_dirty();
                break;
            }
            case G_SETTILE: {
                TileDesc* t = &gRdp.tiles[(w1 >> 24) & 7];
                t->fmt = (w0 >> 21) & 7;
                t->siz = (w0 >> 19) & 3;
                t->tmem = w0 & 0x1FF;
                t->line = (w0 >> 9) & 0x1FF;
                t->palette = (w1 >> 20) & 0xF;
                t->cmt = (w1 >> 18) & 3;
                t->maskt = (w1 >> 14) & 0xF;
                t->shiftt = (w1 >> 10) & 0xF;
                t->cms = (w1 >> 8) & 3;
                t->masks = (w1 >> 4) & 0xF;
                t->shifts = w1 & 0xF;
                gfx_mark_dirty();
                break;
            }
            case G_FILLRECT:
                gfx_fill_rect((w1 >> 12) & 0xFFF, w1 & 0xFFF, (w0 >> 12) & 0xFFF, w0 & 0xFFF);
                break;
            case G_SETFILLCOLOR:
                gRdp.fill = w1;
                break;
            case G_SETFOGCOLOR:
                gRdp.fog = w1;
                break;
            case G_SETBLENDCOLOR:
                gRdp.blend = w1;
                gfx_mark_dirty();
                break;
            case G_SETPRIMCOLOR: {
                /* AF's gDPSetPrimColor carries the prim LOD fraction in the low byte. */
                float lod = (w0 & 0xFF) / 255.0f;
                if (gRdp.prim != w1 || gRdp.prim_lod_frac != lod) {
                    gRdp.prim = w1;
                    gRdp.prim_lod_frac = lod;
                    gfx_mark_dirty();
                }
                break;
            }
            case G_SETENVCOLOR:
                if (gRdp.env != w1) {
                    gRdp.env = w1;
                    gfx_mark_dirty();
                }
                break;
            case G_SETCOMBINE:
                gRdp.combine0 = w0 & 0xFFFFFF;
                gRdp.combine1 = w1;
                gfx_mark_dirty();
                break;
            case G_SETTIMG:
                gRdp.timg = seg_addr(w1);
                gRdp.timg_fmt = (w0 >> 21) & 7;
                gRdp.timg_siz = (w0 >> 19) & 3;
                gRdp.timg_width = (uint16_t)((w0 & 0xFFF) + 1);
                break;
            case G_SETZIMG:
                gRdp.zimg = seg_addr(w1);
                gfx_color_image_changed();
                break;
            case G_SETCIMG:
                gRdp.cimg = seg_addr(w1);
                gRdp.cimg_width = (w0 & 0xFFF) + 1;
                gRdp.cimg_fmt = (w0 >> 21) & 7;
                gRdp.cimg_siz = (w0 >> 19) & 3;
                gfx_color_image_changed();
                break;
            default:
                RT_LOG_ONCE("unknown display list command %02X at %08X", op, pc - 8);
                break;
        }
    }
    RT_LOG_ONCE("display list budget exhausted");
}

void gfx_run_task(uint32_t task) {
    uint32_t ucode = rd_w32(task + 0x10);
    uint32_t data_ptr = rd_w32(task + 0x30);
    gStats.tasks++;
    gfx_debug_task_start(gStats.tasks, task);

    reset_rsp();
    gRsp.s2dex = ucode == UCODE_S2DEX2_TEXT;
    if (gRsp.s2dex) {
        RT_LOG_ONCE("first S2DEX2 task (%08X)", task);
    }
    gfx_mark_dirty();
    uint64_t t0 = sceKernelGetSystemTimeWide();
    run_dl(data_ptr);
    gfx_target_leave();
    gfx_flush_batch();
    gfx_target_flush(); /* the game may look at its picture, and the next task starts from RDRAM */
    gStats.render_us += sceKernelGetSystemTimeWide() - t0;
}
