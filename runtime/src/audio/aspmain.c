/*
 * aspmain.c -- HLE of Animal Forest's audio microcode (aspMain).
 *
 * Command semantics come from AF's own microcode (recompiled with RSPRecomp
 * and read op by op), not from OOT: several commands differ (3 is a no-op,
 * 0 is a DMEM noise/shuffle command, RESAMPLE's flag 2 and FILTER's tap
 * alignment, ...). Where the microcode's vector arithmetic has observable
 * rounding or saturation, it is reproduced.
 *
 * DMEM is 4KB of big-endian memory on the RSP. Nearly every command works on
 * 16-bit samples, so it is kept as host-endian 16-bit words: dm[addr >> 1]
 * holds bytes addr and addr+1. Byte access (ADPCM headers, 8-bit samples,
 * odd addresses) goes through dm_get8/dm_set8. All addresses wrap at 4KB
 * like the RSP's.
 */
#include <string.h>

#define ASP_IMPLEMENTATION
#include "aspmain.h"

#ifdef ASP_DEBUG
#include <stdio.h>
static uint32_t sDebugCmd;
#define ASP_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define ASP_TRACE(...) ((void)0)
#endif

#define DM_WORDS 0x800
#define DM_MASK (DM_WORDS - 1)
#define RDRAM_ADDR_MASK 0x3FFFFFu

#define W(a) dm[((uint32_t)(a) >> 1) & DM_MASK]

/* DMEM locations used by the microcode itself */
#define DM_BUF_IN 0x2E0
#define DM_BUF_OUT 0x2E2
#define DM_BUF_COUNT 0x2E4
#define DM_LOOP 0x2E8
#define DM_DRAM_STACK 0x2EC
#define DM_CMD 0x2F0
#define DM_ADPCM_BOOK 0x300
#define DM_RESAMPLE_TABLE 0x0E0
#define DM_SCRATCH 0xFB0 /* 0xFB0..0xFCF: RESAMPLE/FILTER state image */
#define DM_FILTER_TAPS 0xFD0 /* 0xFD0..0xFFF: FILTER coefficient window */


/*
 * Everything a task writes lives in one block of its own cache lines. On the
 * PSP this runs on the Media Engine, which writes its data cache back in whole
 * 64-byte lines: a variable that merely shared a line with this would be
 * overwritten with the Media Engine's stale copy of it.
 */
AspState g_asp __attribute__((aligned(64)));

void asp_reset(void) {
    memset(dm, 0, sizeof(dm));
    memset(sEnvVol, 0, sizeof(sEnvVol));
    memset(sV31, 0, sizeof(sV31));
    sEnvRampL = sEnvRampR = sEnvRampWet = 0;
    sFilterCount = 0;
}

static inline int16_t sat16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

static inline int16_t sat16_64(int64_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* RSP VMULF: round(a * b / 32768), saturated */
static inline int16_t vmulf(int16_t a, int16_t b) {
    return sat16(((int32_t)a * b + 0x4000) >> 15);
}

/* RSP VMUDM: signed * unsigned, high half */
static inline int16_t vmudm(int16_t a, uint16_t b) {
    return (int16_t)(((int32_t)a * b) >> 16);
}

/* ---- DMEM byte access --------------------------------------------------- */

static inline uint8_t dm_get8(uint32_t a) {
    uint16_t w = (uint16_t)W(a);
    return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

static inline void dm_set8(uint32_t a, uint8_t b) {
    uint16_t w = (uint16_t)W(a);
    if (a & 1) {
        w = (uint16_t)((w & 0xFF00) | b);
    } else {
        w = (uint16_t)((w & 0x00FF) | (b << 8));
    }
    W(a) = (int16_t)w;
}

/* ---- RDRAM access (word-swapped image) ---------------------------------- */

static inline uint8_t rd_get8(uint32_t p) {
    return sRdram[(p & RDRAM_ADDR_MASK) ^ 3];
}

static inline void rd_set8(uint32_t p, uint8_t b) {
    sRdram[(p & RDRAM_ADDR_MASK) ^ 3] = b;
}

static inline uint32_t rd_get32(uint32_t p) {
    uint32_t v;
    memcpy(&v, sRdram + (p & (RDRAM_ADDR_MASK & ~3u)), 4);
    return v;
}

static inline void rd_set32(uint32_t p, uint32_t v) {
    memcpy(sRdram + (p & (RDRAM_ADDR_MASK & ~3u)), &v, 4);
}

/* RSP DMA copies: the RDRAM address is aligned down to 8 bytes. */
static void dma_read(uint32_t dmem, uint32_t dram, uint32_t len) {
    if (dmem + len > 0x1000) {
        ASP_TRACE("cmd %u: DMA read %03X+%X past DMEM\n", sDebugCmd, dmem, len);
    }
    dram &= 0xFFFFF8;
    uint32_t i = 0;
    if ((dmem & 1) == 0) {
        uint32_t ia = dmem >> 1;
        for (; i + 4 <= len; i += 4) {
            uint32_t v = rd_get32(dram + i);
            dm[ia++ & DM_MASK] = (int16_t)(v >> 16);
            dm[ia++ & DM_MASK] = (int16_t)v;
        }
    }
    for (; i < len; i++) {
        dm_set8(dmem + i, rd_get8(dram + i));
    }
}

static void dma_write(uint32_t dmem, uint32_t dram, uint32_t len) {
    /* The sample buffer the game plays is the lowest address a task writes. */
    if ((dram & 0xFFFFF8) < asp_out_buffer) {
        asp_out_buffer = dram & 0xFFFFF8;
    }
    if (dmem + len > 0x1000) {
        ASP_TRACE("cmd %u: DMA write %03X+%X past DMEM\n", sDebugCmd, dmem, len);
    }
    dram &= 0xFFFFF8;
    uint32_t i = 0;
    if ((dmem & 1) == 0) {
        uint32_t ia = dmem >> 1;
        for (; i + 4 <= len; i += 4) {
            uint32_t hi = (uint16_t)dm[ia++ & DM_MASK];
            uint32_t lo = (uint16_t)dm[ia++ & DM_MASK];
            rd_set32(dram + i, (hi << 16) | lo);
        }
    }
    for (; i < len; i++) {
        rd_set8(dram + i, dm_get8(dmem + i));
    }
}

/* Byte-exact move through a bounce buffer, in blocks like the microcode. */
static void dm_move_block(uint32_t dst, uint32_t src, uint32_t len) {
    uint8_t tmp[16];
    for (uint32_t i = 0; i < len; i++) {
        tmp[i] = dm_get8(src + i);
    }
    for (uint32_t i = 0; i < len; i++) {
        dm_set8(dst + i, tmp[i]);
    }
}

static void dm_move_words(uint32_t dst, uint32_t src, uint32_t nwords) {
    if (((dst | src) & 1) == 0) {
        int16_t tmp[16];
        uint32_t is = src >> 1;
        uint32_t id = dst >> 1;
        for (uint32_t i = 0; i < nwords; i++) {
            tmp[i] = dm[(is + i) & DM_MASK];
        }
        for (uint32_t i = 0; i < nwords; i++) {
            dm[(id + i) & DM_MASK] = tmp[i];
        }
    } else {
        dm_move_block(dst, src, nwords * 2);
    }
}

/* ---- commands ----------------------------------------------------------- */

/* 0: pseudo-random add/swap over a DMEM region (unused by the game so far) */
static uint32_t noise_pick(uint32_t* src, uint32_t modulus) {
    int32_t v = W(*src) & 0x7FFF;
    *src += 2;
    uint32_t step = modulus << 7;
    int32_t last = v;
    for (int round = 0; round < 8; round++) {
        if (step == 0) {
            break; /* the microcode would hang here */
        }
        do {
            last = v;
            v -= (int32_t)step;
        } while (v > 0);
        v = last;
        step >>= 1;
    }
    return (uint32_t)last;
}

static void cmd_noise(uint32_t w0, uint32_t w1) {
    uint32_t modulus = (w0 >> 16) & 0xFF;
    uint32_t base = w1 & 0xFFFF;
    uint32_t src = base;
    uint32_t adds = w0 & 0xFFFF;
    uint32_t swaps = w1 >> 16;

    for (; adds != 0; adds--) {
        uint32_t a = (noise_pick(&src, modulus) << 1) + base;
        uint32_t b = (noise_pick(&src, modulus) << 1) + base;
        int16_t sum = (int16_t)(W(b) + W(a));
        W(a) = sum;
        W(b) = sum;
    }
    modulus -= 8;
    for (; swaps != 0; swaps--) {
        uint32_t a = (noise_pick(&src, modulus) << 1) + base;
        uint32_t b = (noise_pick(&src, modulus) << 1) + base;
        int16_t ta[4], tb[4];
        for (int i = 0; i < 4; i++) {
            ta[i] = W(a + 2 * i);
            tb[i] = W(b + 2 * i);
        }
        for (int i = 0; i < 4; i++) {
            W(b + 2 * i) = ta[i];
        }
        for (int i = 0; i < 4; i++) {
            W(a + 2 * i) = tb[i];
        }
    }
}

static inline int32_t adpcm_sext(uint32_t v, int bits) {
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

static void adpcm_half(const int16_t* book, int16_t prev2, int16_t prev1, const int16_t* ins, int16_t* out) {
    const int16_t* b0 = book;
    const int16_t* b1 = book + 8;
    for (int i = 0; i < 8; i++) {
        uint32_t x = (uint32_t)((int32_t)b0[i] * prev2) + (uint32_t)((int32_t)b1[i] * prev1) +
                     ((uint32_t)(int32_t)ins[i] << 11);
        for (int k = 0; k < i; k++) {
            x += (uint32_t)((int32_t)b1[i - 1 - k] * ins[k]);
        }
        out[i] = sat16((int32_t)x >> 11);
    }
}

static void adpcm_load_book(int16_t* book, uint8_t hdr) {
    uint32_t addr = DM_ADPCM_BOOK + ((uint32_t)(hdr & 0xF) << 5);
    for (int i = 0; i < 16; i++) {
        book[i] = W(addr + 2 * i);
    }
}

static void cmd_adpcm(uint32_t w0, uint32_t w1) {
    uint32_t flags = w0 >> 16;
    uint32_t in = (uint16_t)W(DM_BUF_IN);
    uint32_t out = (uint16_t)W(DM_BUF_OUT);
    int32_t count = (uint16_t)W(DM_BUF_COUNT);
    uint32_t state = w1 & 0xFFFFFF;
    int two_bit = (flags & 4) != 0;

    for (int i = 0; i < 8; i++) {
        sV31[i] = W(2 * i);
    }
    for (int i = 0; i < 16; i++) {
        W(out + 2 * i) = 0;
    }
    if (!(flags & 1)) {
        uint32_t src = state;
        if (flags & 2) {
            src = ((uint32_t)(uint16_t)W(DM_LOOP) << 16) | (uint16_t)W(DM_LOOP + 2);
        }
        dma_read(out, src, 32);
    }
    out += 32;

    if (count != 0) {
        /* The microcode reads each frame's header, data and codebook row
         * before storing the previous frame's output. */
        int frame_size = two_bit ? 5 : 9;
        uint8_t hdr = dm_get8(in);
        uint8_t data[8];
        int16_t book[16];
        for (int i = 0; i < 8; i++) {
            data[i] = dm_get8(in + 1 + i);
        }
        adpcm_load_book(book, hdr);
        int16_t prev2 = W(out - 4);
        int16_t prev1 = W(out - 2);
        do {
            in += frame_size;
            uint8_t next_hdr = dm_get8(in);
            uint8_t next_data[8];
            for (int i = 0; i < 8; i++) {
                next_data[i] = dm_get8(in + 1 + i);
            }

            int shift = hdr >> 4;
            int16_t ins[16];
            if (two_bit) {
                int eff = shift < 14 ? shift : 14;
                for (int i = 0; i < 16; i++) {
                    uint32_t field = ((uint32_t)data[i >> 2] >> (6 - 2 * (i & 3))) & 3;
                    ins[i] = (int16_t)((adpcm_sext(field, 2) << 14) >> (14 - eff));
                }
            } else {
                int eff = shift < 12 ? shift : 12;
                for (int i = 0; i < 16; i++) {
                    uint32_t byte = data[i >> 1];
                    uint32_t nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
                    ins[i] = (int16_t)((adpcm_sext(nib, 4) << 12) >> (12 - eff));
                }
            }
            int16_t first[8], second[8];
            adpcm_half(book, prev2, prev1, ins, first);
            adpcm_half(book, first[6], first[7], ins + 8, second);
            adpcm_load_book(book, next_hdr);
            for (int i = 0; i < 8; i++) {
                W(out + 2 * i) = first[i];
            }
            for (int i = 0; i < 8; i++) {
                W(out + 16 + 2 * i) = second[i];
            }
            prev2 = second[6];
            prev1 = second[7];
            hdr = next_hdr;
            memcpy(data, next_data, sizeof(data));
            out += 32;
            count -= 32;
        } while (count > 0);
    }
    dma_write(out - 32, state, 32);
}

static void cmd_clearbuff(uint32_t w0, uint32_t w1) {
    int32_t count = w1 & 0xFFFF;
    uint32_t addr = w0 & 0xFFFF;
    if (count == 0) {
        return;
    }
    do {
        if (addr & 1) {
            for (int i = 0; i < 16; i++) {
                dm_set8(addr + i, 0);
            }
        } else {
            for (int i = 0; i < 8; i++) {
                W(addr + 2 * i) = 0;
            }
        }
        addr += 16;
        count -= 16;
    } while (count > 0);
}

static void cmd_addmixer(uint32_t w0, uint32_t w1) {
    int32_t count = (w0 >> 12) & 0xFF0;
    uint32_t out = w1 & 0xFFFF;
    uint32_t in = w1 >> 16;

    /* vaddc v31,v31,v31: leaves carries in VCO for the first add */
    uint32_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t sum = (uint32_t)(uint16_t)sV31[i] * 2;
        if (sum > 0xFFFF) {
            carry |= 1u << i;
        }
        sV31[i] = (int16_t)sum;
    }

    do {
        int16_t a[32], b[32];
        for (int i = 0; i < 32; i++) {
            a[i] = W(out + 2 * i);
            b[i] = W(in + 2 * i);
        }
        for (int i = 0; i < 32; i++) {
            int32_t c = (i < 8) ? (int32_t)((carry >> i) & 1) : 0;
            W(out + 2 * i) = sat16(a[i] + b[i] + c);
        }
        carry = 0;
        in += 64;
        out += 64;
        count -= 64;
    } while (count > 0);
}

static void cmd_resample(uint32_t w0, uint32_t w1) {
    if (W(DM_RESAMPLE_TABLE) != 0x0C39) {
        uint32_t src = ((uint32_t)(uint16_t)W(DM_DRAM_STACK) << 16) | (uint16_t)W(DM_DRAM_STACK + 2);
        dma_read(DM_RESAMPLE_TABLE, src, 0x200);
    }
    uint32_t in = (uint32_t)(int32_t)W(DM_BUF_IN);
    uint32_t out = (uint32_t)(int32_t)W(DM_BUF_OUT);
    int32_t count = W(DM_BUF_COUNT);
    uint32_t state = w1 & 0xFFFFFF;
    uint32_t flags = w0 >> 16;
    uint32_t pitch = w0 & 0xFFFF;

    if (!(flags & 1)) {
        dma_read(DM_SCRATCH, state, 32);
    } else {
        W(DM_SCRATCH + 8) = 0;
        for (int i = 0; i < 4; i++) {
            W(DM_SCRATCH + 2 * i) = 0;
        }
    }
    int16_t hist[4];
    for (int i = 0; i < 4; i++) {
        hist[i] = W(DM_SCRATCH + 2 * i);
    }
    if (flags & 2) {
        in -= 4;
        W(in) = hist[0];
        W(in + 2) = hist[2];
    } else if (flags & 4) {
        in -= 16;
        for (int i = 0; i < 8; i++) {
            W(in + 2 * i) = hist[i >> 1];
        }
    } else {
        in -= 8;
        for (int i = 0; i < 4; i++) {
            W(in + 2 * i) = hist[i];
        }
    }

    /* Positions are 16.16 relative to `in`; the microcode keeps them as an
     * integer part (clamped to 16 bits) plus a 16-bit fraction per batch of
     * eight output samples. */
    int32_t base_int = 0;
    uint32_t base_frac = (uint16_t)W(DM_SCRATCH + 8);
    uint32_t next_addr;
    uint32_t next_frac;
    for (;;) {
        int16_t tmp[8];
        uint32_t last_int = 0, last_frac = 0;
        for (int i = 0; i < 8; i++) {
            int32_t acc = (base_int << 16) + (int32_t)base_frac + (int32_t)(2 * i * pitch);
            int32_t ip = sat16(acc >> 16);
            uint32_t frac = (uint32_t)acc & 0xFFFF;
            uint32_t addr = (in + 2 * (uint32_t)(uint16_t)ip) & 0xFFFF;
            uint32_t tbl = DM_RESAMPLE_TABLE + ((frac >> 10) << 3);
            int16_t p0 = vmulf(W(addr), W(tbl));
            int16_t p1 = vmulf(W(addr + 2), W(tbl + 2));
            int16_t p2 = vmulf(W(addr + 4), W(tbl + 4));
            int16_t p3 = vmulf(W(addr + 6), W(tbl + 6));
            tmp[i] = sat16(sat16(p0 + p1) + sat16(p2 + p3));
            if (i == 7) {
                last_int = (uint32_t)ip;
                last_frac = frac;
            }
        }
        for (int i = 0; i < 8; i++) {
            W(out + 2 * i) = tmp[i];
        }
        /* next batch starts two pitch steps after sample 7 */
        int32_t acc = ((int32_t)(int16_t)last_int << 16) + (int32_t)last_frac + (int32_t)(2 * pitch);
        base_int = sat16(acc >> 16);
        base_frac = (uint32_t)acc & 0xFFFF;
        next_addr = (in + 2 * (uint32_t)(uint16_t)base_int) & 0xFFFF;
        next_frac = base_frac;
        count -= 16;
        if (count <= 0) {
            break;
        }
        out += 16;
    }

    /* the vectorised position update leaves the next batch's input and
     * table addresses in 0xFD0/0xFE0 */
    for (int i = 0; i < 8; i++) {
        int32_t acc = (base_int << 16) + (int32_t)base_frac + (int32_t)(2 * i * pitch);
        int32_t ip = sat16(acc >> 16);
        W(DM_FILTER_TAPS + 2 * i) = (int16_t)(in + 2 * (uint32_t)(uint16_t)ip);
        W(DM_FILTER_TAPS + 16 + 2 * i) = (int16_t)(DM_RESAMPLE_TABLE + ((((uint32_t)acc & 0xFFFF) >> 10) << 3));
    }
    W(DM_SCRATCH + 8) = (int16_t)next_frac;
    for (int i = 0; i < 4; i++) {
        W(DM_SCRATCH + 2 * i) = W(next_addr + 2 * i);
    }
    dma_write(DM_SCRATCH, state, 32);

    for (int i = 0; i < 8; i++) {
        sV31[i] = W(0x70 + 2 * i);
    }
}

static void cmd_resample_zoh(uint32_t w0, uint32_t w1) {
    uint32_t in = (uint32_t)(int32_t)W(DM_BUF_IN);
    uint32_t out = (uint32_t)(int32_t)W(DM_BUF_OUT);
    int32_t count = W(DM_BUF_COUNT);
    uint32_t step = (w0 & 0xFFFF) << 2;
    uint32_t pos = (w1 & 0xFFFF) | (in << 16);
    do {
        int16_t tmp[4];
        for (int i = 0; i < 4; i++) {
            tmp[i] = W((pos >> 16) & 0xFFFE);
            pos += step;
        }
        for (int i = 0; i < 4; i++) {
            W(out + 2 * i) = tmp[i];
        }
        out += 8;
        count -= 8;
    } while (count > 0);
}

static void cmd_filter(uint32_t w0, uint32_t w1) {
    uint32_t flags = (w0 >> 16) & 0xFF;
    uint32_t state = w1 & 0xFFFFFF;

    for (int i = 0; i < 16; i++) {
        W(DM_SCRATCH + 2 * i) = 0;
    }
    if (flags >= 2) {
        sFilterCount = (int32_t)(w0 & 0xFFFF);
        for (int i = 0; i < 8; i++) {
            W(DM_FILTER_TAPS + 2 * i) = 0;
            W(DM_FILTER_TAPS + 32 + 2 * i) = 0;
        }
        dma_read(DM_FILTER_TAPS + 16, state, 16);
        return;
    }
    if (flags == 0) {
        dma_read(DM_SCRATCH, state, 32);
    }

    /* Smooth the coefficients towards the previous call's (kept in the
     * second half of the state). */
    for (int i = 0; i < 8; i++) {
        int64_t acc = 0x8000 + ((int64_t)W(DM_FILTER_TAPS + 16 + 2 * i) + W(DM_SCRATCH + 16 + 2 * i)) * 0x8000;
        int16_t c = sat16_64(acc >> 16);
        W(DM_FILTER_TAPS + 16 + 2 * i) = c;
        W(DM_SCRATCH + 16 + 2 * i) = c;
    }

    /* Tap k (k = 1..15 over [history, input]) for output i uses window
     * entry 16 - k + i, where the window is 0xFD0..0xFFF. */
    int16_t z[24];
    for (int i = 0; i < 24; i++) {
        z[i] = W(DM_FILTER_TAPS + 2 * i);
    }
    for (int i = 0; i < 8; i++) {
        sV31[i] = z[1 + i];
    }

    int16_t x[16];
    for (int i = 0; i < 8; i++) {
        x[i] = W(DM_SCRATCH + 2 * i);
    }
    uint32_t buf = w0 & 0xFFFF;
    do {
        for (int i = 0; i < 8; i++) {
            x[8 + i] = W(buf + 2 * i);
        }
        for (int i = 0; i < 8; i++) {
            int64_t acc = 0x8000;
            for (int k = 1; k < 16; k++) {
                acc += (int64_t)x[k] * z[16 - k + i] * 2;
            }
            W(buf + 2 * i) = sat16_64(acc >> 16);
        }
        memcpy(x, x + 8, 8 * sizeof(int16_t));
        sFilterCount -= 16;
        buf += 16;
    } while (sFilterCount > 0);

    for (int i = 0; i < 8; i++) {
        W(DM_SCRATCH + 2 * i) = x[i];
    }
    /* the microcode passes 0x1F to a routine that subtracts one again, so
     * the state's last byte is never written back */
    dma_write(DM_SCRATCH, state, 31);
}

static void cmd_setbuff(uint32_t w0, uint32_t w1) {
    W(DM_BUF_IN) = (int16_t)w0;
    W(DM_BUF_OUT) = (int16_t)(w1 >> 16);
    W(DM_BUF_COUNT) = (int16_t)w1;
}

static void cmd_duplicate(uint32_t w0, uint32_t w1) {
    int32_t count = (w0 >> 16) & 0xFF;
    uint32_t in = w0 & 0xFFFF;
    uint32_t out = w1 >> 16;
    int16_t tmp[64];
    for (int i = 0; i < 64; i++) {
        tmp[i] = W(in + 2 * i);
    }
    do {
        for (int i = 0; i < 64; i++) {
            W(out + 2 * i) = tmp[i];
        }
        out += 128;
    } while (--count > 0);
}

static void cmd_dmemmove(uint32_t w0, uint32_t w1) {
    int32_t count = w1 & 0xFFFF;
    uint32_t in = w0 & 0xFFFF;
    uint32_t out = w1 >> 16;
    if (count == 0) {
        return;
    }
    while (count - 16 >= 0) {
        dm_move_words(out, in, 8);
        in += 16;
        out += 16;
        count -= 16;
    }
    while (count > 0) {
        dm_move_words(out, in, 1);
        in += 2;
        out += 2;
        count -= 2;
    }
}

static void cmd_loadadpcm(uint32_t w0, uint32_t w1) {
    dma_read(DM_ADPCM_BOOK, w1 & 0xFFFFFF, w0 & 0xFFFF);
}

static void cmd_mixer(uint32_t w0, uint32_t w1) {
    int32_t count = (w0 >> 12) & 0xFF0;
    uint32_t out = w1 & 0xFFFF;
    uint32_t in = w1 >> 16;
    int32_t gain = (int16_t)w0;
    for (int i = 0; i < 8; i++) {
        sV31[i] = W(2 * i);
    }
    do {
        int16_t a[16], b[16];
        for (int i = 0; i < 16; i++) {
            a[i] = W(out + 2 * i);
            b[i] = W(in + 2 * i);
        }
        for (int i = 0; i < 16; i++) {
            /* fits in 32 bits: |a * 0x7FFF + b * gain| < 2^31 - 0x4000 */
            W(out + 2 * i) = sat16((a[i] * 0x7FFF + b[i] * gain + 0x4000) >> 15);
        }
        in += 32;
        out += 32;
        count -= 32;
    } while (count > 0);
}

static void cmd_interleave(uint32_t w0, uint32_t w1) {
    uint32_t out = w0 & 0xFFFF;
    int32_t count = (w0 >> 12) & 0xFF0;
    uint32_t left = w1 >> 16;
    uint32_t right = w1 & 0xFFFF;
    do {
        int16_t l[4], r[4];
        for (int i = 0; i < 4; i++) {
            l[i] = W(left + 2 * i);
            r[i] = W(right + 2 * i);
        }
        for (int i = 0; i < 4; i++) {
            W(out + 4 * i) = l[i];
            W(out + 4 * i + 2) = r[i];
        }
        left += 8;
        right += 8;
        out += 16;
        count -= 8;
    } while (count > 0);
}

static void cmd_hilogain(uint32_t w0, uint32_t w1) {
    int32_t count = w0 & 0xFFFF;
    uint32_t buf = w1 >> 16;
    int32_t gain_int = (w0 >> 20) & 0xF;
    uint32_t gain_frac = (w0 >> 4) & 0xF000;
    do {
        for (int i = 0; i < 16; i++) {
            int32_t s = W(buf + 2 * i);
            int32_t frac = (s * (int32_t)gain_frac) >> 16;
            W(buf + 2 * i) = sat16(s * gain_int + frac);
        }
        buf += 32;
        count -= 32;
    } while (count > 0);
}

static void cmd_setloop(uint32_t w0, uint32_t w1) {
    (void)w0;
    uint32_t v = w1 & 0xFFFFFF;
    W(DM_LOOP) = (int16_t)(v >> 16);
    W(DM_LOOP + 2) = (int16_t)v;
}

static void cmd_copyblocks(uint32_t w0, uint32_t w1) {
    int32_t blocks = (w0 >> 16) & 0xFF;
    uint32_t in = w0 & 0xFFFF;
    uint32_t out = w1 >> 16;
    do {
        int32_t size = w1 & 0xFFFF;
        do {
            dm_move_words(out, in, 16);
            in += 32;
            out += 32;
            size -= 32;
        } while (size > 0);
    } while (--blocks > 0);
}

static void cmd_interl(uint32_t w0, uint32_t w1) {
    int32_t count = w0 & 0xFFFF;
    uint32_t out = w1 & 0xFFFF;
    uint32_t in = w1 >> 16;
    do {
        int16_t tmp[8];
        for (int i = 0; i < 8; i++) {
            tmp[i] = W(in + 4 * i);
        }
        for (int i = 0; i < 8; i++) {
            W(out + 2 * i) = tmp[i];
        }
        in += 32;
        out += 16;
        count -= 8;
    } while (count > 0);
}

static void cmd_envsetup1(uint32_t w0, uint32_t w1) {
    memset(sEnvVol, 0, sizeof(sEnvVol));
    uint32_t wet = (w0 >> 8) & 0xFF00;
    sEnvVol[4] = (int16_t)wet;
    sEnvVol[5] = (int16_t)(wet + (w0 & 0xFFFF));
    sEnvRampL = w1 >> 16;
    sEnvRampR = w1 & 0xFFFF;
    sEnvRampWet = w0 & 0xFFFF;
}

static void cmd_envsetup2(uint32_t w0, uint32_t w1) {
    (void)w0;
    uint32_t l = w1 >> 16;
    uint32_t r = w1 & 0xFFFF;
    sEnvVol[0] = (int16_t)l;
    sEnvVol[1] = (int16_t)(l + sEnvRampL);
    sEnvVol[2] = (int16_t)r;
    sEnvVol[3] = (int16_t)(r + sEnvRampR);
}

static void cmd_envmixer(uint32_t w0, uint32_t w1) {
    sEnvRampL += sEnvRampL;
    sEnvRampR += sEnvRampR;
    sEnvRampWet += sEnvRampWet;
    uint16_t step[8] = {
        (uint16_t)sEnvRampL, (uint16_t)sEnvRampL, (uint16_t)sEnvRampR, (uint16_t)sEnvRampR,
        (uint16_t)sEnvRampWet, (uint16_t)sEnvRampWet, 0, 0,
    };

    uint32_t in = ((w0 >> 16) & 0xFF) << 4;
    uint32_t dry_l = ((w1 >> 24) & 0xFF) << 4;
    uint32_t dry_r = ((w1 >> 16) & 0xFF) << 4;
    uint32_t wet_l = ((w1 >> 8) & 0xFF) << 4;
    uint32_t wet_r = (w1 & 0xFF) << 4;
    int16_t neg_dl = (w0 & 2) ? -1 : 0;
    int16_t neg_dr = (w0 & 1) ? -1 : 0;
    int16_t neg_wl = (w0 & 8) ? -4 : 0;
    int16_t neg_wr = (w0 & 4) ? -2 : 0;
    int swap = (w0 & 0x10) != 0;
    int32_t count = (w0 >> 8) & 0xFF;

    /* Blocks of 16 samples. The microcode loads every destination block
     * before storing any of them and stores in a fixed order, which matters
     * when destinations alias (e.g. both wet channels on one buffer). */
    int16_t src[16];
    for (int i = 0; i < 8; i++) {
        src[i] = W(in + 2 * i);
    }
    do {
        int16_t dl[16], dr[16], wl[16], wr[16];
        for (int i = 0; i < 8; i++) {
            src[8 + i] = W(in + 16 + 2 * i);
        }
        for (int i = 0; i < 16; i++) {
            dl[i] = W(dry_l + 2 * i);
            dr[i] = W(dry_r + 2 * i);
            wl[i] = W(wet_l + 2 * i);
            wr[i] = W(wet_r + 2 * i);
        }
        for (int i = 0; i < 16; i++) {
            int h = i >> 3;
            uint16_t vw = (uint16_t)sEnvVol[4 + h];
            int16_t s = src[i];
            int16_t l = (int16_t)(vmudm(s, (uint16_t)sEnvVol[0 + h]) ^ neg_dl);
            int16_t r = (int16_t)(vmudm(s, (uint16_t)sEnvVol[2 + h]) ^ neg_dr);
            dl[i] = sat16(dl[i] + l);
            dr[i] = sat16(dr[i] + r);
            int16_t el = (int16_t)(vmudm(l, vw) ^ neg_wl);
            int16_t er = (int16_t)(vmudm(r, vw) ^ neg_wr);
            if (swap) {
                wl[i] = sat16(wl[i] + er);
                wr[i] = sat16(wr[i] + el);
            } else {
                wl[i] = sat16(wl[i] + el);
                wr[i] = sat16(wr[i] + er);
            }
        }
        for (int i = 0; i < 8; i++) {
            W(dry_l + 2 * i) = dl[i];
        }
        for (int i = 0; i < 8; i++) {
            W(dry_r + 2 * i) = dr[i];
        }
        for (int i = 8; i < 16; i++) {
            W(dry_l + 2 * i) = dl[i];
        }
        for (int i = 8; i < 16; i++) {
            W(dry_r + 2 * i) = dr[i];
        }
        for (int i = 0; i < 8; i++) {
            W(wet_l + 2 * i) = wl[i];
        }
        for (int i = 0; i < 8; i++) {
            W(wet_r + 2 * i) = wr[i];
        }
        for (int i = 0; i < 8; i++) {
            src[i] = W(in + 32 + 2 * i);
        }
        for (int i = 8; i < 16; i++) {
            W(wet_l + 2 * i) = wl[i];
        }
        for (int i = 8; i < 16; i++) {
            W(wet_r + 2 * i) = wr[i];
        }
        for (int i = 0; i < 8; i++) {
            sEnvVol[i] = (int16_t)((uint16_t)sEnvVol[i] + step[i]);
        }
        in += 32;
        dry_l += 32;
        dry_r += 32;
        wet_l += 32;
        wet_r += 32;
        count -= 16;
    } while (count > 0);
}

static void cmd_loadbuff(uint32_t w0, uint32_t w1) {
    uint32_t size = (w0 >> 12) & 0xFF0;
    if (size != 0) {
        dma_read(w0 & 0xFFFF, w1 & 0xFFFFFF, size);
    }
}

static void cmd_savebuff(uint32_t w0, uint32_t w1) {
    uint32_t size = (w0 >> 12) & 0xFF0;
    if (size != 0) {
        dma_write(w0 & 0xFFFF, w1 & 0xFFFFFF, size);
    }
}

static void cmd_s8dec(uint32_t w0, uint32_t w1) {
    uint32_t flags = w0 >> 16;
    uint32_t in = (uint16_t)W(DM_BUF_IN);
    uint32_t out = (uint16_t)W(DM_BUF_OUT);
    int32_t count = (uint16_t)W(DM_BUF_COUNT);
    uint32_t state = w1 & 0xFFFFFF;

    for (int i = 0; i < 16; i++) {
        W(out + 2 * i) = 0;
    }
    if (!(flags & 1)) {
        uint32_t src = state;
        if (flags & 2) {
            src = ((uint32_t)(uint16_t)W(DM_LOOP) << 16) | (uint16_t)W(DM_LOOP + 2);
        }
        dma_read(out, src, 32);
    }
    out += 32;
    if (count != 0) {
        /* 8-sample chunks; chunk j + 2 is read before chunk j is stored */
        int16_t chunks[3][8];
        uint32_t next_in = in;
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 8; i++) {
                chunks[c][i] = (int16_t)((int8_t)dm_get8(next_in + i) * 256);
            }
            next_in += 8;
        }
        do {
            for (int half = 0; half < 2; half++) {
                for (int i = 0; i < 8; i++) {
                    chunks[2][i] = (int16_t)((int8_t)dm_get8(next_in + i) * 256);
                }
                next_in += 8;
                for (int i = 0; i < 8; i++) {
                    W(out + 16 * half + 2 * i) = chunks[0][i];
                }
                memmove(chunks[0], chunks[1], sizeof(chunks[0]) * 2);
            }
            out += 32;
            count -= 32;
        } while (count > 0);
    }
    dma_write(out - 32, state, 32);
}

typedef void (*AspCmd)(uint32_t w0, uint32_t w1);

static void cmd_nop(uint32_t w0, uint32_t w1) {
    (void)w0;
    (void)w1;
}

static const AspCmd sCommands[24] = {
    cmd_noise,       /* 0 */
    cmd_adpcm,       /* 1 */
    cmd_clearbuff,   /* 2 */
    cmd_nop,         /* 3 */
    cmd_addmixer,    /* 4 */
    cmd_resample,    /* 5 */
    cmd_resample_zoh, /* 6 */
    cmd_filter,      /* 7 */
    cmd_setbuff,     /* 8 */
    cmd_duplicate,   /* 9 */
    cmd_dmemmove,    /* 10 */
    cmd_loadadpcm,   /* 11 */
    cmd_mixer,       /* 12 */
    cmd_interleave,  /* 13 */
    cmd_hilogain,    /* 14 */
    cmd_setloop,     /* 15 */
    cmd_copyblocks,  /* 16 */
    cmd_interl,      /* 17 */
    cmd_envsetup1,   /* 18 */
    cmd_envmixer,    /* 19 */
    cmd_loadbuff,    /* 20 */
    cmd_savebuff,    /* 21 */
    cmd_envsetup2,   /* 22 */
    cmd_s8dec,       /* 23 */
};

void asp_run_task(uint8_t* rdram, const AspTask* task) {
    sRdram = rdram;
    asp_out_buffer = 0xFFFFFFFF;

    /* The microcode reloads its data section every task (the DMA length
     * register takes size - 1, so one extra byte is copied). */
    dma_read(0, task->ucode_data, task->ucode_data_size + 1);
    W(DM_DRAM_STACK) = (int16_t)(task->dram_stack >> 16);
    W(DM_DRAM_STACK + 2) = (int16_t)task->dram_stack;

    uint32_t ptr = task->data_ptr & 0xFFFFFF;
    int32_t remaining = (int32_t)task->data_size;
    while (remaining > 0) {
        uint32_t w0 = rd_get32(ptr & ~7u);
        uint32_t w1 = rd_get32((ptr & ~7u) + 4);
        W(DM_CMD) = (int16_t)(w0 >> 16);
        W(DM_CMD + 2) = (int16_t)w0;
        W(DM_CMD + 4) = (int16_t)(w1 >> 16);
        W(DM_CMD + 6) = (int16_t)w1;
        uint32_t op = (w0 >> 24) & 0x7F;
        remaining -= 8;
#ifdef ASP_DEBUG
        sDebugCmd = (task->data_size - (uint32_t)remaining) / 8 - 1;
#endif
        if (op < 24) {
            asp_opcode_counts[op]++;
            sCommands[op](w0, w1);
        } else {
            asp_opcode_counts[31]++;
        }
        ptr += 8;
    }
}
